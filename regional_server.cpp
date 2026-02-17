/*

// regional_server.cpp
// ============================================================
// Regionalni server (pokrenuti 2 instance, npr. region_id=1 i region_id=2):
// - TLS server za uređaje (REGISTER/STATUS/FAULT, CMD)
// - UDP server za telemetriju senzora (event-driven sender; server samo "listen")
// - Periodično šalje agregaciju ka CENTRAL serveru (REGION_SYNC_UP)
// - Detektuje "comm_lost" (timeout) i izoluje uređaj (fault=true)
//
// DODANO (za vaš zahtjev: motion -> upali lampu):
// - UDP Telemetry server ima motion callback (motion==1)
// - Kada dođe motion event za zonu, regional nađe sve lampe u toj zoni
//   i pošalje im TLS CMD: SWITCH_ON + SET_INTENSITY.
//
// Napomena (BUGFIX):
// Rekurzivni timer koristi shared_ptr<std::function<void()>> (nema segfault).
// ============================================================

#include <boost/asio.hpp>
#include <thread>
#include <vector>
#include <iostream>
#include <functional>
#include <algorithm>
#include <map>
#include <string>

#include "tls_device_server.hpp"
#include "udp_telemetry_server.hpp"
#include "region_sync_client.hpp"
#include "registry.hpp"

// =========================== FSM pomoćne strukture ===========================

enum class ServerState {
  IDLE = 0,
  REGISTRATION_HANDLING,
  NORMAL_OPERATION,
  FAULT_HANDLING
};

static std::string to_string(ServerState s){
  switch(s){
    case ServerState::IDLE: return "IDLE";
    case ServerState::REGISTRATION_HANDLING: return "REGISTRATION_HANDLING";
    case ServerState::NORMAL_OPERATION: return "NORMAL_OPERATION";
    case ServerState::FAULT_HANDLING: return "FAULT_HANDLING";
  }
  return "UNKNOWN";
}

// Tranzicijska matrica (PingPong pattern): lista dozvoljenih prelaza
using TransitionMatrix = std::multimap<std::string, std::string>;

class ServerFsm {
public:
  ServerFsm()
    : state_(ServerState::IDLE)
  {
    transitions_.insert({"IDLE", "REGISTRATION_HANDLING"});                   // boot_done
    transitions_.insert({"REGISTRATION_HANDLING", "REGISTRATION_HANDLING"}); // invalid_uri
    transitions_.insert({"REGISTRATION_HANDLING", "NORMAL_OPERATION"});       // valid_uri / server_ready
    transitions_.insert({"NORMAL_OPERATION", "NORMAL_OPERATION"});            // regular_op
    transitions_.insert({"NORMAL_OPERATION", "FAULT_HANDLING"});              // fault_detected
    transitions_.insert({"FAULT_HANDLING", "NORMAL_OPERATION"});              // recover_ok
    transitions_.insert({"FAULT_HANDLING", "IDLE"});                          // manual_reset
  }

  ServerState get() const { return state_; }

  bool switch_to(ServerState next, const std::string& event_label){
    const std::string oldS = to_string(state_);
    const std::string newS = to_string(next);

    bool allowed = false;
    auto range = transitions_.equal_range(oldS);
    for(auto it = range.first; it != range.second; ++it){
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

// ====================== comm_lost timer (BUGFIX included) ======================

static void schedule_comm_lost_check(boost::asio::io_context& io,
                                     sls::Registry& reg,
                                     uint32_t region_id,
                                     uint64_t timeout_ms,
                                     ServerFsm& fsm)
{
  auto timer = std::make_shared<boost::asio::steady_timer>(io);
  auto tick = std::make_shared<std::function<void()>>();

  *tick = [timer, &reg, region_id, timeout_ms, &fsm, tick]() {
    timer->expires_after(std::chrono::seconds(5));

    timer->async_wait([timer, &reg, region_id, timeout_ms, &fsm, tick](const boost::system::error_code& ec) {
      if (ec) return;

      auto lost = reg.detect_comm_lost(sls::now_ms(), timeout_ms);

      if(!lost.empty()){
        fsm.switch_to(ServerState::FAULT_HANDLING, "fault_detected/comm_lost");
      }

      for(auto& uri : lost){
        std::cerr << "[REGION " << region_id << "] comm_lost -> isolate uri=" << uri << "\n";
      }

      if(lost.empty() && fsm.get() == ServerState::FAULT_HANDLING){
        fsm.switch_to(ServerState::NORMAL_OPERATION, "recover_ok");
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

  // 1) TLS server za uređaje
  sls::RegionalDeviceServer dev_srv(io, tls_port, cert, key, reg, region_id);

  // 2) UDP server za telemetriju senzora
  sls::UdpTelemetryServer udp_srv(io, udp_port, reg, region_id);

  // --- Motion callback: kada senzor javi motion, upali lampe u toj zoni ---
  // CMD kodovi (kao u vašem protokolu/klijentu):
  // 1 = SWITCH_ON, 2 = SWITCH_OFF, 3 = SET_INTENSITY
  constexpr uint8_t CMD_SWITCH_ON = 1;
  constexpr uint8_t CMD_SET_INTENSITY = 3;

  udp_srv.set_motion_callback([&](uint32_t zone_id, const std::string& sensor_uri){
    (void)sensor_uri;

    // U NORMAL_OPERATION šaljemo komande; u FAULT_HANDLING možemo preskočiti
    if(fsm.get() != ServerState::NORMAL_OPERATION) return;

    auto lamps = reg.get_lamps_in_zone(zone_id);
    if(lamps.empty()){
      std::cerr << "[REGION " << region_id << "] motion in zone=" << zone_id
                << " but no lamps registered in that zone yet.\n";
      return;
    }

    for(const auto& lamp_uri : lamps){
      // SWITCH_ON
      bool ok1 = dev_srv.send_cmd_to_uri(lamp_uri, CMD_SWITCH_ON, 0);
      // SET_INTENSITY (npr. 60%)
      bool ok2 = dev_srv.send_cmd_to_uri(lamp_uri, CMD_SET_INTENSITY, 60);

      std::cout << "[REGION " << region_id << "] motion->CMD lamp=" << lamp_uri
                << " ok_on=" << ok1 << " ok_int=" << ok2 << "\n";
    }
  });

  // 3) Region -> central sync
  auto sync = std::make_shared<sls::RegionSyncClient>(
      io, reg, region_id, central_host, central_port, sync_interval_s);
  sync->start();

  // spreman za normalan rad
  fsm.switch_to(ServerState::NORMAL_OPERATION, "server_ready");

  // 4) Comm-lost provjera
  schedule_comm_lost_check(io, reg, region_id, comm_lost_timeout_s * 1000ull, fsm);

  // Thread pool
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

#include <boost/asio.hpp>
#include <thread>
#include <vector>
#include <iostream>
#include <functional>
#include <algorithm>
#include <map>
#include <string>

#include "tls_device_server.hpp"
#include "udp_telemetry_server.hpp"
#include "region_sync_client.hpp"
#include "registry.hpp"

// =========================== FSM pomoćne strukture ===========================

enum class ServerState {
  IDLE = 0,
  REGISTRATION_HANDLING,
  NORMAL_OPERATION,
  FAULT_HANDLING
};

static std::string to_string(ServerState s){
  switch(s){
    case ServerState::IDLE: return "IDLE";
    case ServerState::REGISTRATION_HANDLING: return "REGISTRATION_HANDLING";
    case ServerState::NORMAL_OPERATION: return "NORMAL_OPERATION";
    case ServerState::FAULT_HANDLING: return "FAULT_HANDLING";
  }
  return "UNKNOWN";
}

using TransitionMatrix = std::multimap<std::string, std::string>;

class ServerFsm {
public:
  ServerFsm()
    : state_(ServerState::IDLE)
  {
    transitions_.insert({"IDLE", "REGISTRATION_HANDLING"});                   // boot_done
    transitions_.insert({"REGISTRATION_HANDLING", "REGISTRATION_HANDLING"}); // invalid_uri
    transitions_.insert({"REGISTRATION_HANDLING", "NORMAL_OPERATION"});      // valid_uri / server_ready
    transitions_.insert({"NORMAL_OPERATION", "NORMAL_OPERATION"});           // regular_op
    transitions_.insert({"NORMAL_OPERATION", "FAULT_HANDLING"});             // fault_detected
    transitions_.insert({"FAULT_HANDLING", "NORMAL_OPERATION"});             // recover_ok
    transitions_.insert({"FAULT_HANDLING", "IDLE"});                         // manual_reset
  }

  ServerState get() const { return state_; }

  bool switch_to(ServerState next, const std::string& event_label){
    const std::string oldS = to_string(state_);
    const std::string newS = to_string(next);

    bool allowed = false;
    auto range = transitions_.equal_range(oldS);
    for(auto it = range.first; it != range.second; ++it){
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

// ====================== comm_lost timer (BUGFIX included) ======================

static void schedule_comm_lost_check(boost::asio::io_context& io,
                                     sls::Registry& reg,
                                     uint32_t region_id,
                                     uint64_t timeout_ms,
                                     ServerFsm& fsm)
{
  auto timer = std::make_shared<boost::asio::steady_timer>(io);
  auto tick = std::make_shared<std::function<void()>>();

  *tick = [timer, &reg, region_id, timeout_ms, &fsm, tick]() {
    timer->expires_after(std::chrono::seconds(5));

    timer->async_wait([timer, &reg, region_id, timeout_ms, &fsm, tick](const boost::system::error_code& ec) {
      if (ec) return;

      auto lost = reg.detect_comm_lost(sls::now_ms(), timeout_ms);

      if(!lost.empty()){
        fsm.switch_to(ServerState::FAULT_HANDLING, "fault_detected/comm_lost");
      }

      for(auto& uri : lost){
        std::cerr << "[REGION " << region_id << "] comm_lost -> isolate uri=" << uri << "\n";
      }

      if(lost.empty() && fsm.get() == ServerState::FAULT_HANDLING){
        fsm.switch_to(ServerState::NORMAL_OPERATION, "recover_ok");
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

  // 1) TLS server za uređaje (lampe)
  sls::RegionalDeviceServer dev_srv(io, tls_port, cert, key, reg, region_id);

  // 2) UDP server za telemetriju senzora
  sls::UdpTelemetryServer udp_srv(io, udp_port, reg, region_id);

  // CMD kodovi (proto.hpp / luminaire_client):
  constexpr uint8_t CMD_SWITCH_ON     = 1;
  constexpr uint8_t CMD_SWITCH_OFF    = 2;
  constexpr uint8_t CMD_SET_INTENSITY = 3;

  // --- motion ON: upali + postavi intenzitet ---
  udp_srv.set_motion_on_callback([&](uint32_t zone_id, const std::string& sensor_uri){
    (void)sensor_uri;
    if(fsm.get() != ServerState::NORMAL_OPERATION) return;

    auto lamps = reg.get_lamps_in_zone(zone_id);
    if(lamps.empty()){
      std::cerr << "[REGION " << region_id << "] motion ON in zone=" << zone_id
                << " but no lamps registered.\n";
      return;
    }

    for(const auto& lamp_uri : lamps){
      bool ok1 = dev_srv.send_cmd_to_uri(lamp_uri, CMD_SWITCH_ON, 0);
      bool ok2 = dev_srv.send_cmd_to_uri(lamp_uri, CMD_SET_INTENSITY, 60);
      std::cout << "[REGION " << region_id << "] motion ON -> CMD lamp=" << lamp_uri
                << " ok_on=" << ok1 << " ok_int=" << ok2 << "\n";
    }
  });

  // --- motion OFF: ugasi lampu (štedi energiju) ---
  udp_srv.set_motion_off_callback([&](uint32_t zone_id, const std::string& sensor_uri){
    (void)sensor_uri;
    if(fsm.get() != ServerState::NORMAL_OPERATION) return;

    auto lamps = reg.get_lamps_in_zone(zone_id);
    if(lamps.empty()){
      std::cerr << "[REGION " << region_id << "] motion OFF in zone=" << zone_id
                << " but no lamps registered.\n";
      return;
    }

    for(const auto& lamp_uri : lamps){
      bool ok = dev_srv.send_cmd_to_uri(lamp_uri, CMD_SWITCH_OFF, 0);
      std::cout << "[REGION " << region_id << "] motion OFF -> CMD lamp=" << lamp_uri
                << " ok_off=" << ok << "\n";
    }
  });

  // --- sensor FAULT marker ---
  udp_srv.set_sensor_fault_callback([&](uint32_t zone_id, const std::string& sensor_uri, uint8_t code){
    std::cerr << "[REGION " << region_id << "] SENSOR FAULT callback uri=" << sensor_uri
              << " zone=" << zone_id << " code=" << int(code) << "\n";
    // ako želiš: server-level FSM prebaciti u FAULT_HANDLING
    fsm.switch_to(ServerState::FAULT_HANDLING, "fault_detected/sensor_fault");
    // a onda (demo) odmah nazad u normal:
    fsm.switch_to(ServerState::NORMAL_OPERATION, "recover_ok");
  });

  // 3) Region -> central sync
  auto sync = std::make_shared<sls::RegionSyncClient>(
      io, reg, region_id, central_host, central_port, sync_interval_s);
  sync->start();

  fsm.switch_to(ServerState::NORMAL_OPERATION, "server_ready");

  // 4) comm_lost provjera
  schedule_comm_lost_check(io, reg, region_id, comm_lost_timeout_s * 1000ull, fsm);

  // Thread pool
  unsigned n = std::max(2u, std::thread::hardware_concurrency());
  std::vector<std::thread> threads;
  threads.reserve(n);
  for(unsigned i=0; i<n; i++){
    threads.emplace_back([&](){ io.run(); });
  }
  for(auto& t: threads) t.join();
  return 0;
}
