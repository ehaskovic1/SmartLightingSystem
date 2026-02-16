// sensor_udp.cpp
// ============================================================
// Senzor (UDP, EVENT-DRIVEN)
// - Šalje telemetriju samo kada se desi EVENT:
//     1) Motion rising edge (0 -> 1)
//     2) Promjena lux-a >= lux_threshold
//     3) Promjena temperature >= temp_threshold_x10
// - Opcionalno: rijedak heartbeat (default 30s)
//
// NOTE:
// - UDP paket je TelemetryUdp (proto.hpp).
// - Regionalni server tretira motion==1 kao "motion event" i tada šalje
//   CMD lampi u istoj zoni (SWITCH_ON + SET_INTENSITY).
//
// FSM (demo):
//   IDLE -> UNREGISTERED (boot_done)
//   UNREGISTERED -> ACTIVE (udp_ready)   // nema "prave" UDP registracije
//   ACTIVE -> ACTIVE (event_report)
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

// ===================== FSM DEFINICIJA =====================
enum class SensorState { IDLE, UNREGISTERED, ACTIVE };

static std::string to_string(SensorState s){
  switch(s){
    case SensorState::IDLE: return "IDLE";
    case SensorState::UNREGISTERED: return "UNREGISTERED";
    case SensorState::ACTIVE: return "ACTIVE";
  }
  return "UNKNOWN";
}

using TransitionMatrix = std::multimap<std::string, std::string>;

class SensorFsm {
public:
  SensorFsm(){
    transitions_.insert({"IDLE","UNREGISTERED"});
    transitions_.insert({"UNREGISTERED","ACTIVE"});
    transitions_.insert({"ACTIVE","ACTIVE"});
  }
  SensorState get() const { return st_; }
  bool switch_to(SensorState next, const std::string& ev){
    std::string a = to_string(st_), b = to_string(next);
    bool ok=false;
    auto r=transitions_.equal_range(a);
    for(auto it=r.first; it!=r.second; ++it) if(it->second==b){ ok=true; break; }
    if(!ok){
      std::cerr<<"[SENSOR-FSM] INVALID "<<a<<" -> "<<b<<" (event="<<ev<<")\n";
      return false;
    }
    st_=next;
    std::cout<<"[SENSOR-FSM] "<<a<<" -> "<<b<<" (event="<<ev<<")\n";
    return true;
  }
private:
  SensorState st_{SensorState::IDLE};
  TransitionMatrix transitions_;
};

// ===================== UDP SEND HELPERS =====================
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
      << "Usage: sensor_udp <server_ip> <udp_port> <uri> <zone_id> <run_seconds> [lux_threshold] [temp_threshold_x10] [heartbeat_seconds]\n"
      << "Example: sensor_udp 127.0.0.1 7777 sls://sensor-001 1 120 25 10 30\n";
    return 1;
  }

  asio::io_context io;
  udp::socket sock(io);
  sock.open(udp::v4());

  udp::endpoint server(asio::ip::make_address(argv[1]), (uint16_t)std::stoi(argv[2]));
  std::string uri = argv[3];
  uint32_t zone = (uint32_t)std::stoul(argv[4]);
  int run_s = std::stoi(argv[5]); 

  const uint16_t lux_threshold = (argc >= 7) ? (uint16_t)std::stoul(argv[6]) : (uint16_t)25;
  const int16_t  temp_threshold_x10 = (argc >= 8) ? (int16_t)std::stoi(argv[7]) : (int16_t)10;
  const int heartbeat_s = (argc >= 9) ? std::stoi(argv[8]) : 30;

  SensorFsm fsm;
  fsm.switch_to(SensorState::UNREGISTERED, "boot_done");
  fsm.switch_to(SensorState::ACTIVE, "udp_ready");

  asio::steady_timer tick_timer(io);
  asio::steady_timer stop_timer(io);

  // Simulacija mjerenja
  uint16_t lux = 200, prev_lux = 200;
  uint8_t motion = 0, prev_motion = 0;
  int16_t temp_x10 = 235, prev_temp_x10 = 235;

  int seconds_elapsed = 0;
  int last_heartbeat = -999999;

  std::function<void()> schedule_tick;
  schedule_tick = [&](){
    tick_timer.expires_after(std::chrono::seconds(1));
    tick_timer.async_wait([&](const boost::system::error_code& ec){
      if(ec) return;

      // --- simulacija ---
      lux = (uint16_t)(180 + (seconds_elapsed % 60));
      motion = (seconds_elapsed % 7 == 0) ? 1 : 0; // rising edge svako ~7s
      temp_x10 = (int16_t)(235 + ((seconds_elapsed/20) % 6));

      bool motion_rise = (prev_motion == 0 && motion == 1);
      bool lux_changed = (std::abs((int)lux - (int)prev_lux) >= (int)lux_threshold);
      bool temp_changed = (std::abs((int)temp_x10 - (int)prev_temp_x10) >= (int)temp_threshold_x10);
      bool heartbeat_due = (heartbeat_s > 0) && (seconds_elapsed - last_heartbeat >= heartbeat_s);

      if(fsm.get() == SensorState::ACTIVE && (motion_rise || lux_changed || temp_changed || heartbeat_due)){
        fsm.switch_to(SensorState::ACTIVE, "event_report");
        send_telemetry(sock, server, uri, zone, lux, motion, temp_x10);
        if(heartbeat_due) last_heartbeat = seconds_elapsed;
      }

      prev_motion = motion;
      prev_lux = lux;
      prev_temp_x10 = temp_x10;

      seconds_elapsed++;
      schedule_tick();
    });
  };

  schedule_tick();
  if(run_s>0){
  stop_timer.expires_after(std::chrono::seconds(run_s));
  stop_timer.async_wait([&](auto){ io.stop(); });
}
  io.run();
  return 0;
}
