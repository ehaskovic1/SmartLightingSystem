
// sensor_udp.cpp
// ============================================================
// Senzor (UDP, EVENT-DRIVEN)
// - Šalje telemetriju samo kada se desi EVENT:
//     1) Motion rising edge (0 -> 1)
//     2) Motion falling edge (1 -> 0)   <-- koristi se za "ugasiti lampu" (preko regionalnog)
//     3) Promjena lux-a >= lux_threshold
//     4) Promjena temperature >= temp_threshold_x10
// - Opcionalno: rijedak heartbeat (default 30s)
//
// NOTE:
// - UDP paket je TelemetryUdp (proto.hpp).
// - Regionalni server tretira motion==1 kao "motion event" i tada šalje
//   CMD lampi u istoj zoni (SWITCH_ON + SET_INTENSITY).
// - Za gašenje: senzor šalje event sa motion==0 (falling edge), a regionalni
//   treba na to poslati CMD SWITCH_OFF lampi.
//
// FSM (demo):
//   IDLE -> UNREGISTERED (boot_done)
//   UNREGISTERED -> REGISTERING (send_uri/REGISTER_REQ)  // demo
//   REGISTERING -> ACTIVE (udp_ready/REGISTER_ACK(demo))
//   ACTIVE -> ACTIVE (event_report)
//   ACTIVE -> FAULT (fault_detected/FAULT_REPORT)
//   FAULT -> IDLE (reset)
// ============================================================

#include <boost/asio.hpp>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <map>
#include <string>
#include <functional>
#include <algorithm>
#include <random>

#include "proto.hpp"

namespace asio = boost::asio;
using udp = asio::ip::udp;

// ===================== ENDIAN HELPERS =====================
static uint16_t swap16(uint16_t x){ return uint16_t((x>>8) | (x<<8)); }
static uint32_t swap32(uint32_t x){
  return ((x & 0x000000FFu) << 24) |
         ((x & 0x0000FF00u) << 8)  |
         ((x & 0x00FF0000u) >> 8)  |
         ((x & 0xFF000000u) >> 24);
}

// ===================== RANDOM HELPERS =====================
static uint32_t urand(uint32_t a, uint32_t b){
  static std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<uint32_t> d(a,b);
  return d(rng);
}

// ===================== FSM DEFINICIJA =====================
enum class SensorState { IDLE, UNREGISTERED, REGISTERING, ACTIVE, FAULT };

static std::string to_string(SensorState s){
  switch(s){
    case SensorState::IDLE: return "IDLE";
    case SensorState::UNREGISTERED: return "UNREGISTERED";
    case SensorState::REGISTERING: return "REGISTERING";
    case SensorState::ACTIVE: return "ACTIVE";
    case SensorState::FAULT: return "FAULT";
  }
  return "UNKNOWN";
}

using TransitionMatrix = std::multimap<std::string, std::string>;

class SensorFsm {
public:
  SensorFsm(){
    // baš kako si tražila:
    transitions_.insert({"IDLE", "UNREGISTERED"});
    transitions_.insert({"UNREGISTERED", "REGISTERING"});
    transitions_.insert({"REGISTERING", "ACTIVE"});
    transitions_.insert({"REGISTERING", "UNREGISTERED"});
    transitions_.insert({"ACTIVE", "ACTIVE"});
    transitions_.insert({"ACTIVE", "FAULT"});
    transitions_.insert({"FAULT", "IDLE"});
  }

  SensorState get() const { return st_; }

  bool switch_to(SensorState next, const std::string& ev){
    std::string a = to_string(st_);
    std::string b = to_string(next);

    bool ok=false;
    auto r = transitions_.equal_range(a);
    for(auto it=r.first; it!=r.second; ++it){
      if(it->second == b){ ok=true; break; }
    }
    if(!ok){
      std::cerr<<"[SENSOR-FSM] INVALID "<<a<<" -> "<<b<<" (event="<<ev<<")\n";
      return false;
    }

    st_ = next;
    std::cout<<"[SENSOR-FSM] "<<a<<" -> "<<b<<" (event="<<ev<<")\n";
    return true;
  }

private:
  SensorState st_{SensorState::IDLE};
  TransitionMatrix transitions_;
};

// ===================== UDP SEND HELPERS =====================
// motion meanings for UDP layer (dogovor sa regionalnim):
//   motion = 0/1  -> normalno
//   motion = 254  -> FAULT marker (demo)
// (TelemetryUdp nema msg_type, pa marker mora biti kroz motion vrijednost)
static void send_telemetry(udp::socket& sock,
                           const udp::endpoint& server,
                           const std::string& uri,
                           uint32_t zone_id,
                           uint16_t lux,
                           uint8_t motion,
                           int16_t temp_x10)
{
  sls::TelemetryUdp t{};
  std::snprintf(t.uri, sizeof(t.uri), "%s", uri.c_str());
  t.zone_id_be = swap32(zone_id);
  t.lux_be = swap16(lux);
  t.motion = motion;

  uint16_t temp_be = swap16((uint16_t)temp_x10);
  std::memcpy(&t.temp_c_x10_be, &temp_be, 2);

  sock.send_to(asio::buffer(&t, sizeof(t)), server);
}

// =============================== MAIN ===============================
int main(int argc, char** argv){
  if(argc < 6){
    std::cerr
      << "Usage: sensor_udp <server_ip> <udp_port> <uri> <zone_id> <run_seconds>\n"
      << "             [lux_threshold] [temp_threshold_x10] [heartbeat_seconds]\n"
      << "             [fault_prob_per_mille] [fault_reset_after_s]\n\n"
      << "Example: sensor_udp 127.0.0.1 7777 sls://sensor-001 1 0 25 10 30 5 10\n"
      << "  run_seconds=0  -> radi beskonačno (Ctrl+C gasi)\n"
      << "  fault_prob_per_mille: default 0 (0=nikad), npr 5 => 0.5% po tick-u\n"
      << "  fault_reset_after_s: default 0 (0=ne resetuje automatski)\n";
    return 1;
  }

  asio::io_context io;

  // Ctrl+C gašenje (praktično za demo)
  asio::signal_set signals(io, SIGINT, SIGTERM);
  signals.async_wait([&](auto, auto){
    std::cout << "[SENSOR] Signal received, stopping...\n";
    io.stop();
  });

  udp::socket sock(io);
  sock.open(udp::v4());

  udp::endpoint server(asio::ip::make_address(argv[1]), (uint16_t)std::stoi(argv[2]));
  std::string uri = argv[3];
  uint32_t zone = (uint32_t)std::stoul(argv[4]);

  int run_s = std::stoi(argv[5]); // 0 => beskonačno

  const uint16_t lux_threshold = (argc >= 7) ? (uint16_t)std::stoul(argv[6]) : (uint16_t)25;
  const int16_t  temp_threshold_x10 = (argc >= 8) ? (int16_t)std::stoi(argv[7]) : (int16_t)10;
  const int heartbeat_s = (argc >= 9) ? std::stoi(argv[8]) : 30;

  const uint32_t fault_prob_per_mille = (argc >= 10) ? (uint32_t)std::stoul(argv[9]) : 0u;
  const int fault_reset_after_s = (argc >= 11) ? std::stoi(argv[10]) : 0;

  SensorFsm fsm;

  // ---------------- BOOT ----------------
  fsm.switch_to(SensorState::UNREGISTERED, "boot_done");

// ---------------- "REGISTER" (DEMO) ----------------
// UDP nema pravu registraciju, ali FSM-u treba tok:
// UNREGISTERED -> REGISTERING -> ACTIVE
if(fsm.switch_to(SensorState::REGISTERING, "send_uri/REGISTER_REQ(demo)")){
  // Timer mora živjeti dovoljno dugo -> shared_ptr
  auto ack_demo = std::make_shared<asio::steady_timer>(io);
  ack_demo->expires_after(std::chrono::milliseconds(200));
  ack_demo->async_wait([&, ack_demo](const boost::system::error_code& ec){
    if(ec) return;
    if(fsm.get() == SensorState::REGISTERING){
      fsm.switch_to(SensorState::ACTIVE, "udp_ready/REGISTER_ACK(demo)");
    }
  });
}


  asio::steady_timer tick_timer(io);
  asio::steady_timer stop_timer(io);
  asio::steady_timer fault_reset_timer(io);

  // Simulacija mjerenja
  uint16_t lux = 200, prev_lux = 200;
  uint8_t motion = 0, prev_motion = 0;
  int16_t temp_x10 = 235, prev_temp_x10 = 235;

  int seconds_elapsed = 0;
  int last_heartbeat = -999999;

  auto arm_fault_reset = [&](){
    if(fault_reset_after_s > 0){
      fault_reset_timer.expires_after(std::chrono::seconds(fault_reset_after_s));
      fault_reset_timer.async_wait([&](const boost::system::error_code& ec){
        if(ec) return;
        if(fsm.get() == SensorState::FAULT){
          fsm.switch_to(SensorState::IDLE, "reset");
          // nakon reset-a tipično ide ponovni boot:
          fsm.switch_to(SensorState::UNREGISTERED, "boot_done");
          fsm.switch_to(SensorState::REGISTERING, "send_uri/REGISTER_REQ(demo)");
          // i vrlo brzo ACTIVE
          fsm.switch_to(SensorState::ACTIVE, "udp_ready/REGISTER_ACK(demo)");
        }
      });
    }
  };

  std::function<void()> schedule_tick;
  schedule_tick = [&](){
    tick_timer.expires_after(std::chrono::seconds(1));
    tick_timer.async_wait([&](const boost::system::error_code& ec){
      if(ec) return;

      // --- simulacija ---
      lux = (uint16_t)(180 + (seconds_elapsed % 60));
      // motion simulacija: 1 jedan tick svakih ~7s, pa se vraća na 0
      motion = (seconds_elapsed % 7 == 0) ? 1 : 0;
      temp_x10 = (int16_t)(235 + ((seconds_elapsed/20) % 6));

      // event detekcija
      bool motion_rise = (prev_motion == 0 && motion == 1);
      bool motion_fall = (prev_motion == 1 && motion == 0); // <-- BITNO za OFF
      bool lux_changed  = (std::abs((int)lux - (int)prev_lux) >= (int)lux_threshold);
      bool temp_changed = (std::abs((int)temp_x10 - (int)prev_temp_x10) >= (int)temp_threshold_x10);
      bool heartbeat_due = (heartbeat_s > 0) && (seconds_elapsed - last_heartbeat >= heartbeat_s);

      // FAULT simulacija (random)
      if(fsm.get() == SensorState::ACTIVE && fault_prob_per_mille > 0){
        if(urand(1,1000) <= fault_prob_per_mille){
          if(fsm.switch_to(SensorState::FAULT, "fault_detected/FAULT_REPORT")){
            // šaljemo FAULT marker preko motion=254 (demo dogovor)
            send_telemetry(sock, server, uri, zone, 0, 254, 0);
            arm_fault_reset();
          }
        }
      }

      // normalan rad: samo u ACTIVE
      if(fsm.get() == SensorState::ACTIVE){
        if(motion_rise || motion_fall || lux_changed || temp_changed || heartbeat_due){
          fsm.switch_to(SensorState::ACTIVE, "event_report");

          // Šaljemo motion i kad je 0 (fall event) -> regional može poslati CMD OFF lampi
          send_telemetry(sock, server, uri, zone, lux, motion, temp_x10);

          if(heartbeat_due) last_heartbeat = seconds_elapsed;
        }
      }

      prev_motion = motion;
      prev_lux = lux;
      prev_temp_x10 = temp_x10;

      seconds_elapsed++;
      schedule_tick();
    });
  };

  schedule_tick();

  // Trajanje: run_s==0 -> beskonačno (Ctrl+C)
  if(run_s > 0){
    stop_timer.expires_after(std::chrono::seconds(run_s));
    stop_timer.async_wait([&](auto){ io.stop(); });
  }

  io.run();
  return 0;
}


