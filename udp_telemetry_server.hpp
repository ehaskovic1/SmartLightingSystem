
#pragma once
#include <boost/asio.hpp>
#include <iostream>
#include <array>
#include <cstring>
#include <functional>
#include <unordered_map>

#include "proto.hpp"
#include "registry.hpp"

namespace sls {
namespace asio = boost::asio;
using udp = asio::ip::udp;

// ============================================================
// Regionalni UDP server za telemetriju senzora
// - async_receive_from
// - update last_seen + upsert SENSOR u registry
// - detektuje motion edge:
//      rising  (0->1) => motion_on callback
//      falling (1->0) => motion_off callback
// - detektuje FAULT marker:
//      motion==254 => sensor_fault callback (i reg.fault=true)
// ============================================================

class UdpTelemetryServer {
public:
  using MotionCallback = std::function<void(uint32_t zone_id, const std::string& sensor_uri)>;
  using FaultCallback  = std::function<void(uint32_t zone_id, const std::string& sensor_uri, uint8_t code)>;

  UdpTelemetryServer(asio::io_context& io, uint16_t port, Registry& reg, uint32_t region_id)
    : sock_(io, udp::endpoint(udp::v4(), port)), reg_(reg), region_id_(region_id)
  {
    do_receive();
  }

  void set_motion_on_callback(MotionCallback cb){ motion_on_cb_ = std::move(cb); }
  void set_motion_off_callback(MotionCallback cb){ motion_off_cb_ = std::move(cb); }
  void set_sensor_fault_callback(FaultCallback cb){ sensor_fault_cb_ = std::move(cb); }

private:
  void do_receive(){
    sock_.async_receive_from(
      asio::buffer(buf_), sender_,
      [this](const boost::system::error_code& ec, std::size_t n){
        if(!ec && n >= sizeof(TelemetryUdp)){
          TelemetryUdp t{};
          std::memcpy(&t, buf_.data(), sizeof(TelemetryUdp));

          std::string uri(t.uri);
          uint32_t zone_id = from_be32(t.zone_id_be);
          uint16_t lux = from_be16(t.lux_be);

          uint16_t tmp_be{};
          std::memcpy(&tmp_be, &t.temp_c_x10_be, 2);
          int16_t temp10 = (int16_t)from_be16(tmp_be);

          // --- registry upsert/update kao SENSOR ---
          DeviceState st;
          if(reg_.get(uri, st)){
            st.type = DeviceType::SENSOR;
            st.zone_id = zone_id;
            st.last_seen_ms = now_ms();
            reg_.upsert(uri, st);
          } else {
            DeviceState ns;
            ns.type = DeviceType::SENSOR;
            ns.zone_id = zone_id;
            ns.on = 0; ns.intensity = 0; ns.power_mw = 0;
            ns.fault = false;
            ns.last_seen_ms = now_ms();
            reg_.upsert(uri, ns);
          }

          // --- FAULT marker (dogovor): motion == 254 ---
          if(t.motion == 254){
            DeviceState s2;
            if(reg_.get(uri, s2)){
              s2.fault = true;
              s2.last_seen_ms = now_ms();
              reg_.upsert(uri, s2);
            }
            std::cerr << "[REGION " << region_id_ << "] SENSOR_FAULT uri=" << uri
                      << " zone=" << zone_id << " (motion=254)\n";
            if(sensor_fault_cb_) sensor_fault_cb_(zone_id, uri, 1);
            // Napomena: i dalje možeš pustiti receive loop, bez return
          }

          // --- motion edge detekcija (0/1) ---
          if(t.motion == 0 || t.motion == 1){
            uint8_t prev = 0;
            auto it = last_motion_.find(uri);
            if(it != last_motion_.end()) prev = it->second;

            // rising edge
            if(prev == 0 && t.motion == 1){
              std::cout << "[REGION " << region_id_ << "] UDP motion ON uri=" << uri
                        << " zone=" << zone_id << " lux=" << lux
                        << " temp=" << (temp10/10.0) << "\n";
              if(motion_on_cb_) motion_on_cb_(zone_id, uri);
            }

            // falling edge
            if(prev == 1 && t.motion == 0){
              std::cout << "[REGION " << region_id_ << "] UDP motion OFF uri=" << uri
                        << " zone=" << zone_id << " lux=" << lux
                        << " temp=" << (temp10/10.0) << "\n";
              if(motion_off_cb_) motion_off_cb_(zone_id, uri);
            }

            last_motion_[uri] = t.motion;
          }
        }

        do_receive();
      });
  }

private:
  udp::socket sock_;
  udp::endpoint sender_;
  std::array<uint8_t, 1024> buf_{};
  Registry& reg_;
  uint32_t region_id_{0};

  // per-sensor last motion (za edge detekciju)
  std::unordered_map<std::string, uint8_t> last_motion_;

  MotionCallback motion_on_cb_{};
  MotionCallback motion_off_cb_{};
  FaultCallback  sensor_fault_cb_{};
};

} // namespace sls
