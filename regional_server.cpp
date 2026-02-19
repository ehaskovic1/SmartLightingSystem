/*
// regional_server.cpp
// ============================================================
// Regionalni server
// ============================================================

#include <boost/asio.hpp>
#include <thread>
#include <vector>
#include <iostream>
#include <functional>
#include <algorithm>
#include <map>
#include <string>
#include <ctime>

#include "tls_device_server.hpp"
#include "udp_telemetry_server.hpp"
#include "region_sync_client.hpp"
#include "registry.hpp"
#include "db.hpp"
#include "proto.hpp"
#include <unordered_set>

// =========================== FSM pomoćne strukture ===========================
enum class ServerState { IDLE=0, REGISTRATION_HANDLING, NORMAL_OPERATION, FAULT_HANDLING };

static std::string to_string(ServerState s){
  switch(s){
    case ServerState::IDLE: return "IDLE";
    case ServerState::REGISTRATION_HANDLING: return "REGISTRATION_HANDLING";
    case ServerState::NORMAL_OPERATION: return "NORMAL_OPERATION";
    case ServerState::FAULT_HANDLING: return "FAULT_HANDLING";
  }
  return "UNKNOWN";
}

using TransitionMatrix = std::multimap<std::string,std::string>;

class ServerFsm {
public:
  ServerFsm(): state_(ServerState::IDLE){
    transitions_.insert({"IDLE","REGISTRATION_HANDLING"});
    transitions_.insert({"REGISTRATION_HANDLING","REGISTRATION_HANDLING"});
    transitions_.insert({"REGISTRATION_HANDLING","NORMAL_OPERATION"});
    transitions_.insert({"NORMAL_OPERATION","NORMAL_OPERATION"});
    transitions_.insert({"NORMAL_OPERATION","FAULT_HANDLING"});
    transitions_.insert({"FAULT_HANDLING","NORMAL_OPERATION"});
    transitions_.insert({"FAULT_HANDLING","IDLE"});
  }

  ServerState get() const { return state_; }

  bool switch_to(ServerState next, const std::string& event_label){
    const std::string oldS = to_string(state_);
    const std::string newS = to_string(next);

    bool allowed = false;
    auto range = transitions_.equal_range(oldS);
    for(auto it=range.first; it!=range.second; ++it){
      if(it->second == newS){ allowed = true; break; }
    }

    if(!allowed){
      std::cerr << "[FSM] INVALID transition: " << oldS << " -> " << newS
                << " (event=" << event_label << ")\n";
      return false;
    }

    state_ = next;
    std::cout << "[FSM] " << oldS << " -> " << newS
              << " (event=" << event_label << ")\n";
    return true;
  }

private:
  ServerState state_;
  TransitionMatrix transitions_;
};

// ====================== comm_lost timer ======================
static void schedule_comm_lost_check(boost::asio::io_context& io,
                                     sls::Registry& reg,
                                     uint32_t region_id,
                                     uint64_t timeout_ms,
                                     ServerFsm& fsm,
                                     sls::RegionSyncClient& sync)
{
  auto timer = std::make_shared<boost::asio::steady_timer>(io);
  auto tick  = std::make_shared<std::function<void()>>();

  // da ne šalje alarm svakih 5s, nego samo kad URI "prvi put" postane lost
  auto active_lost = std::make_shared<std::unordered_set<std::string>>();

  *tick = [timer, &reg, region_id, timeout_ms, &fsm, &sync, tick, active_lost](){
    timer->expires_after(std::chrono::seconds(5));
    timer->async_wait([timer, &reg, region_id, timeout_ms, &fsm, &sync, tick, active_lost](const boost::system::error_code& ec){
      if(ec) return;

      auto lost = reg.detect_comm_lost(sls::now_ms(), timeout_ms);

      // napravi set trenutnih lost uri
      std::unordered_set<std::string> now_lost(lost.begin(), lost.end());

      // NOVI lost -> okini fault + alarm
      for(const auto& uri : lost){
        if(active_lost->insert(uri).second){
          // prvi put je postao lost
          fsm.switch_to(ServerState::FAULT_HANDLING, "fault_detected/comm_lost");

          sls::DeviceState st;
          uint32_t zone_id = 0;
          uint8_t devType = 1; // 1 lamp default
          if(reg.get(uri, st)){
            zone_id = st.zone_id;
            devType = (st.type == sls::DeviceType::LUMINAIRE) ? 1 : 2;
          }

          std::cerr << "[REGION " << region_id << "] comm_lost -> ALARM uri=" << uri << "\n";

          sls::AlarmUp a;
          a.region_id   = region_id;
          a.ts_unix     = (uint32_t)std::time(nullptr);
          a.device_type = devType;
          a.zone_id     = zone_id;
          a.code        = 9;              // npr. 9 = comm_lost (možeš i 1)
          a.uri         = uri;
          a.text        = "comm_lost";

          sync.send_alarm_now(a);
        }
      }

      // recovered -> makni iz active_lost
      for(auto it = active_lost->begin(); it != active_lost->end(); ){
        if(now_lost.find(*it) == now_lost.end()){
          it = active_lost->erase(it);
        } else {
          ++it;
        }
      }

      // ako više ništa nije lost, možeš izaći iz fault
      if(active_lost->empty()){
        if(fsm.get() == ServerState::FAULT_HANDLING){
          fsm.switch_to(ServerState::NORMAL_OPERATION, "recover_ok");
        }
      }

      (*tick)();
    });
  };

  (*tick)();
}

// ================================ main =================================
int main(int argc, char** argv){
  if(argc < 10){
    std::cerr
      << "Usage: regional_server <region_id> <tls_port> <udp_port> <central_host> <central_tls_port> "
         "<sync_interval_s> <cert.pem> <key.pem> <comm_lost_timeout_s>\n"
      << "Example: regional_server 1 5555 7777 127.0.0.1 6000 5 cert.pem key.pem 20\n";
    return 1;
  }

  uint32_t region_id = (uint32_t)std::stoul(argv[1]);
  uint16_t tls_port  = (uint16_t)std::stoi(argv[2]);
  uint16_t udp_port  = (uint16_t)std::stoi(argv[3]);
  std::string central_host = argv[4];
  uint16_t central_port = (uint16_t)std::stoi(argv[5]);
  int sync_interval_s = std::stoi(argv[6]);
  std::string cert = argv[7];
  std::string key  = argv[8];
  uint64_t comm_lost_timeout_s = (uint64_t)std::stoull(argv[9]);

  boost::asio::io_context io;

  ServerFsm fsm;
  fsm.switch_to(ServerState::REGISTRATION_HANDLING, "boot_done");

  sls::Registry reg;

  sls::DbWriter db;
  db.start("sls.db", "schema.sql");

  // 1) Region -> central sync (KREIRAJ SAMO JEDNOM)
  auto sync = std::make_shared<sls::RegionSyncClient>(
      io, reg, region_id, central_host, central_port, sync_interval_s);

  // Počni odmah (1x sync odmah, pa interval)
  sync->start(true);

  // 2) TLS server za uređaje
  sls::RegionalDeviceServer dev_srv(io, tls_port, cert, key, reg, region_id, db);

  // Fault callback iz TLS-a -> FSM + ALARM centrali
  dev_srv.set_fault_callback([&](const sls::FaultReport& f, sls::DeviceType dtype){
    std::cerr << "[REGION " << region_id << "] DEVICE FAULT cb uri="<<f.uri
              << " zone="<<f.zone_id<<" code="<<(int)f.code<<" text="<<f.text<<"\n";

    fsm.switch_to(ServerState::FAULT_HANDLING, "fault_detected/device_fault");

    sls::AlarmUp a;
    a.region_id   = region_id;
    a.ts_unix     = (uint32_t)std::time(nullptr);
    a.device_type = (dtype == sls::DeviceType::LUMINAIRE) ? 1 : 2; // 1 lamp, 2 sensor
    a.zone_id     = f.zone_id;
    a.code        = f.code;
    a.uri         = f.uri;
    a.text        = f.text;

    // KLJUČNO: odmah pošalji alarm centrali
    sync->send_alarm_now(a);
  });

  // 3) UDP server za telemetriju senzora
  sls::UdpTelemetryServer udp_srv(io, udp_port, reg, region_id);

  constexpr uint8_t CMD_SWITCH_ON     = 1;
  constexpr uint8_t CMD_SWITCH_OFF    = 2;
  constexpr uint8_t CMD_SET_INTENSITY = 3;

  udp_srv.set_motion_on_callback([&](uint32_t zone_id, const std::string& sensor_uri){
    (void)sensor_uri;

    // Motion komande šalji samo u NORMAL
    if(fsm.get() != ServerState::NORMAL_OPERATION) return;

    auto lamps = reg.get_lamps_in_zone(zone_id);
    if(lamps.empty()){
      std::cerr << "[REGION " << region_id << "] motion ON in zone=" << zone_id << " but no lamps registered.\n";
      return;
    }

    for(const auto& lamp_uri : lamps){
      bool ok1 = dev_srv.send_cmd_to_uri(lamp_uri, CMD_SWITCH_ON, 0);
      bool ok2 = dev_srv.send_cmd_to_uri(lamp_uri, CMD_SET_INTENSITY, 60);
      std::cout << "[REGION " << region_id << "] motion ON -> CMD lamp=" << lamp_uri
                << " ok_on=" << ok1 << " ok_int=" << ok2 << "\n";
    }
  });

  udp_srv.set_motion_off_callback([&](uint32_t zone_id, const std::string& sensor_uri){
    (void)sensor_uri;

    if(fsm.get() != ServerState::NORMAL_OPERATION) return;

    auto lamps = reg.get_lamps_in_zone(zone_id);
    if(lamps.empty()){
      std::cerr << "[REGION " << region_id << "] motion OFF in zone=" << zone_id << " but no lamps registered.\n";
      return;
    }

    for(const auto& lamp_uri : lamps){
      bool ok = dev_srv.send_cmd_to_uri(lamp_uri, CMD_SWITCH_OFF, 0);
      std::cout << "[REGION " << region_id << "] motion OFF -> CMD lamp=" << lamp_uri
                << " ok_off=" << ok << "\n";
    }
  });

  // SENSOR fault (UDP) -> FSM + ALARM centrali + DB
  udp_srv.set_sensor_fault_callback([&](uint32_t zone_id, const std::string& sensor_uri, uint8_t code){
    std::cerr << "[REGION " << region_id << "] SENSOR FAULT callback uri=" << sensor_uri
              << " zone=" << zone_id << " code=" << int(code) << "\n";

    // 1) FSM ide u FAULT i NE vraćaj odmah recover_ok (da se stvarno vidi fault stanje)
    fsm.switch_to(ServerState::FAULT_HANDLING, "fault_detected/sensor_fault");

    // 2) DB: upsert device + log_fault
    db.upsert_device(
      sensor_uri,
      2,              // 2 = sensor
      region_id,
      zone_id,
      true,           // fault = true
      0,              // on
      0,              // intensity
      0               // power_mw
    );
    db.log_fault(sensor_uri, region_id, zone_id, code, "sensor_fault");

    // 3) Alarm centrali
    sls::AlarmUp a;
    a.region_id   = region_id;
    a.ts_unix     = (uint32_t)std::time(nullptr);
    a.device_type = 2;          // sensor
    a.zone_id     = zone_id;
    a.code        = code;
    a.uri         = sensor_uri;
    a.text        = "sensor_fault";

    sync->send_alarm_now(a);

    // (Opcionalno) ako hoćeš automatski recover nakon X sekundi,
    // uradi timer ovdje. Za sad ostavi FAULT da se vidi.
  });

  fsm.switch_to(ServerState::NORMAL_OPERATION, "server_ready");

  schedule_comm_lost_check(io, reg, region_id, comm_lost_timeout_s * 1000ull, fsm, *sync);

  unsigned n = std::max(2u, std::thread::hardware_concurrency());
  std::vector<std::thread> threads;
  threads.reserve(n);
  for(unsigned i=0; i<n; i++){
    threads.emplace_back([&](){ io.run(); });
  }
  for(auto& t: threads) t.join();
  return 0;
}
*/

// regional_server.cpp
// ============================================================
// Regionalni server
// ============================================================

#include <boost/asio.hpp>
#include <thread>
#include <vector>
#include <iostream>
#include <functional>
#include <algorithm>
#include <map>
#include <string>
#include <ctime>
#include <unordered_set>

#include "tls_device_server.hpp"
#include "udp_telemetry_server.hpp"
#include "region_sync_client.hpp"
#include "registry.hpp"
#include "db.hpp"
#include "proto.hpp"

// =========================== FSM pomoćne strukture ===========================
enum class ServerState { IDLE=0, REGISTRATION_HANDLING, NORMAL_OPERATION, FAULT_HANDLING };

static std::string to_string(ServerState s){
  switch(s){
    case ServerState::IDLE: return "IDLE";
    case ServerState::REGISTRATION_HANDLING: return "REGISTRATION_HANDLING";
    case ServerState::NORMAL_OPERATION: return "NORMAL_OPERATION";
    case ServerState::FAULT_HANDLING: return "FAULT_HANDLING";
  }
  return "UNKNOWN";
}

using TransitionMatrix = std::multimap<std::string,std::string>;

class ServerFsm {
public:
  ServerFsm(): state_(ServerState::IDLE){
    transitions_.insert({"IDLE","REGISTRATION_HANDLING"});
    transitions_.insert({"REGISTRATION_HANDLING","REGISTRATION_HANDLING"});
    transitions_.insert({"REGISTRATION_HANDLING","NORMAL_OPERATION"});
    transitions_.insert({"NORMAL_OPERATION","NORMAL_OPERATION"});
    transitions_.insert({"NORMAL_OPERATION","FAULT_HANDLING"});
    transitions_.insert({"FAULT_HANDLING","NORMAL_OPERATION"});
    transitions_.insert({"FAULT_HANDLING","IDLE"});
  }

  ServerState get() const { return state_; }

  bool switch_to(ServerState next, const std::string& event_label){
    const std::string oldS = to_string(state_);
    const std::string newS = to_string(next);

    bool allowed = false;
    auto range = transitions_.equal_range(oldS);
    for(auto it=range.first; it!=range.second; ++it){
      if(it->second == newS){ allowed = true; break; }
    }

    if(!allowed){
      std::cerr << "[FSM] INVALID transition: " << oldS << " -> " << newS
                << " (event=" << event_label << ")\n";
      return false;
    }

    state_ = next;
    std::cout << "[FSM] " << oldS << " -> " << newS
              << " (event=" << event_label << ")\n";
    return true;
  }

private:
  ServerState state_;
  TransitionMatrix transitions_;
};

// ====================== comm_lost timer ======================
static void schedule_comm_lost_check(boost::asio::io_context& io,
                                     sls::Registry& reg,
                                     uint32_t region_id,
                                     uint64_t timeout_ms,
                                     ServerFsm& fsm,
                                     sls::RegionSyncClient& sync)
{
  auto timer = std::make_shared<boost::asio::steady_timer>(io);
  auto tick  = std::make_shared<std::function<void()>>();

  // da ne šalje alarm svakih 5s, nego samo kad URI "prvi put" postane lost
  auto active_lost = std::make_shared<std::unordered_set<std::string>>();

  *tick = [timer, &reg, region_id, timeout_ms, &fsm, &sync, tick, active_lost](){
    timer->expires_after(std::chrono::seconds(5));
    timer->async_wait([timer, &reg, region_id, timeout_ms, &fsm, &sync, tick, active_lost](const boost::system::error_code& ec){
      if(ec) return;

      auto lost = reg.detect_comm_lost(sls::now_ms(), timeout_ms);
      std::unordered_set<std::string> now_lost(lost.begin(), lost.end());

      for(const auto& uri : lost){
        if(active_lost->insert(uri).second){
          fsm.switch_to(ServerState::FAULT_HANDLING, "fault_detected/comm_lost");

          sls::DeviceState st;
          uint32_t zone_id = 0;
          uint8_t devType = 1; // 1 lamp default
          if(reg.get(uri, st)){
            zone_id = st.zone_id;
            devType = (st.type == sls::DeviceType::LUMINAIRE) ? 1 : 2;
          }

          std::cerr << "[REGION " << region_id << "] comm_lost -> ALARM uri=" << uri << "\n";

          sls::AlarmUp a;
          a.region_id   = region_id;
          a.ts_unix     = (uint32_t)std::time(nullptr);
          a.device_type = devType;
          a.zone_id     = zone_id;
          a.code        = 9;           // npr. 9 = comm_lost
          a.uri         = uri;
          a.text        = "comm_lost";

          sync.send_alarm_now(a);
        }
      }

      // recovered -> makni iz active_lost
      for(auto it = active_lost->begin(); it != active_lost->end(); ){
        if(now_lost.find(*it) == now_lost.end()){
          it = active_lost->erase(it);
        } else {
          ++it;
        }
      }

      if(active_lost->empty()){
        if(fsm.get() == ServerState::FAULT_HANDLING){
          fsm.switch_to(ServerState::NORMAL_OPERATION, "recover_ok");
        }
      }

      (*tick)();
    });
  };

  (*tick)();
}

// ================================ main =================================
int main(int argc, char** argv){
  if(argc < 10){
    std::cerr
      << "Usage: regional_server <region_id> <tls_port> <udp_port> <central_host> <central_tls_port> "
         "<sync_interval_s> <cert.pem> <key.pem> <comm_lost_timeout_s>\n"
      << "Example: regional_server 1 5555 7777 127.0.0.1 6000 5 cert.pem key.pem 20\n";
    return 1;
  }

  uint32_t region_id = (uint32_t)std::stoul(argv[1]);
  uint16_t tls_port  = (uint16_t)std::stoi(argv[2]);
  uint16_t udp_port  = (uint16_t)std::stoi(argv[3]);
  std::string central_host = argv[4];
  uint16_t central_port = (uint16_t)std::stoi(argv[5]);
  int sync_interval_s = std::stoi(argv[6]);
  std::string cert = argv[7];
  std::string key  = argv[8];
  uint64_t comm_lost_timeout_s = (uint64_t)std::stoull(argv[9]);

  boost::asio::io_context io;

  ServerFsm fsm;
  fsm.switch_to(ServerState::REGISTRATION_HANDLING, "boot_done");

  sls::Registry reg;

  sls::DbWriter db;
  db.start("sls.db", "schema.sql");

  // 1) Region -> central sync (KREIRAJ SAMO JEDNOM)
  auto sync = std::make_shared<sls::RegionSyncClient>(
      io, reg, region_id, central_host, central_port, sync_interval_s);

  // Počni odmah
  sync->start(true);

  // 2) TLS server za uređaje
  sls::RegionalDeviceServer dev_srv(io, tls_port, cert, key, reg, region_id, db);

  // Fault callback iz TLS-a -> FSM + ALARM centrali
  dev_srv.set_fault_callback([&](const sls::FaultReport& f, sls::DeviceType dtype){
    std::cerr << "[REGION " << region_id << "] DEVICE FAULT cb uri="<<f.uri
              << " zone="<<f.zone_id<<" code="<<(int)f.code<<" text="<<f.text<<"\n";

    fsm.switch_to(ServerState::FAULT_HANDLING, "fault_detected/device_fault");

    sls::AlarmUp a;
    a.region_id   = region_id;
    a.ts_unix     = (uint32_t)std::time(nullptr);
    a.device_type = (dtype == sls::DeviceType::LUMINAIRE) ? 1 : 2;
    a.zone_id     = f.zone_id;
    a.code        = f.code;
    a.uri         = f.uri;
    a.text        = f.text;

    sync->send_alarm_now(a);
  });

  // 3) UDP server za telemetriju senzora
  sls::UdpTelemetryServer udp_srv(io, udp_port, reg, region_id);

  constexpr uint8_t CMD_SWITCH_ON     = 1;
  constexpr uint8_t CMD_SWITCH_OFF    = 2;
  constexpr uint8_t CMD_SET_INTENSITY = 3;

  udp_srv.set_motion_on_callback([&](uint32_t zone_id, const std::string& sensor_uri){
    // Motion komande šalji samo u NORMAL
    if(fsm.get() != ServerState::NORMAL_OPERATION) return;

    auto lamps = reg.get_lamps_in_zone(zone_id);
    if(lamps.empty()){
      std::cerr << "[REGION " << region_id << "] motion ON in zone=" << zone_id << " but no lamps registered.\n";
      return;
    }

    for(const auto& lamp_uri : lamps){
      bool ok1 = dev_srv.send_cmd_to_uri(lamp_uri, CMD_SWITCH_ON, 0);
      bool ok2 = dev_srv.send_cmd_to_uri(lamp_uri, CMD_SET_INTENSITY, 60);
      std::cout << "[REGION " << region_id << "] motion ON -> CMD lamp=" << lamp_uri
                << " ok_on=" << ok1 << " ok_int=" << ok2 << "\n";
    }
  });

  udp_srv.set_motion_off_callback([&](uint32_t zone_id, const std::string& sensor_uri){
    if(fsm.get() != ServerState::NORMAL_OPERATION) return;

    auto lamps = reg.get_lamps_in_zone(zone_id);
    if(lamps.empty()){
      std::cerr << "[REGION " << region_id << "] motion OFF in zone=" << zone_id << " but no lamps registered.\n";
      return;
    }

    for(const auto& lamp_uri : lamps){
      bool ok = dev_srv.send_cmd_to_uri(lamp_uri, CMD_SWITCH_OFF, 0);
      std::cout << "[REGION " << region_id << "] motion OFF -> CMD lamp=" << lamp_uri
                << " ok_off=" << ok << "\n";
    }
  });

  // SENSOR fault (UDP) -> FSM + ALARM centrali + DB + Registry (DA SE VIDI U SNAPSHOTU)
  udp_srv.set_sensor_fault_callback([&](uint32_t zone_id, const std::string& sensor_uri, uint8_t code){
    std::cerr << "[REGION " << region_id << "] SENSOR FAULT callback uri=" << sensor_uri
              << " zone=" << zone_id << " code=" << int(code) << "\n";

    // 1) FSM ide u FAULT
    fsm.switch_to(ServerState::FAULT_HANDLING, "fault_detected/sensor_fault");

    // 2) Registry: OBAVEZNO upiši sensor, inače snapshot kaže sensors total=0
    sls::DeviceState st;
    st.type = sls::DeviceType::SENSOR;
    st.zone_id = zone_id;
    st.on = 0;
    st.intensity = 0;
    st.power_mw = 0;
    st.fault = true;
    st.last_seen_ms = sls::now_ms();
    reg.upsert(sensor_uri, st);

    // 3) DB
    db.upsert_device(
      sensor_uri,
      2,              // 2 = sensor
      region_id,
      zone_id,
      true,
      0, 0, 0
    );
    db.log_fault(sensor_uri, region_id, zone_id, code, "sensor_fault");

    // 4) Alarm centrali
    sls::AlarmUp a;
    a.region_id   = region_id;
    a.ts_unix     = (uint32_t)std::time(nullptr);
    a.device_type = 2;          // sensor
    a.zone_id     = zone_id;
    a.code        = code;
    a.uri         = sensor_uri;
    a.text        = "sensor_fault";

    sync->send_alarm_now(a);
  });

  fsm.switch_to(ServerState::NORMAL_OPERATION, "server_ready");

  schedule_comm_lost_check(io, reg, region_id, comm_lost_timeout_s * 1000ull, fsm, *sync);

  unsigned n = std::max(2u, std::thread::hardware_concurrency());
  std::vector<std::thread> threads;
  threads.reserve(n);
  for(unsigned i=0; i<n; i++){
    threads.emplace_back([&](){ io.run(); });
  }
  for(auto& t: threads) t.join();
  return 0;
}
