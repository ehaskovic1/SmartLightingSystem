#pragma once
#include <boost/asio.hpp>
#include <iostream>
#include <array>
#include <cstring>
#include <functional>
#include "proto.hpp"
#include "registry.hpp"

namespace sls {
namespace asio = boost::asio;
using udp = asio::ip::udp;

// ============================================================
// Regionalni UDP server za telemetriju senzora
// - async_receive_from kao u predavanju
// - update last_seen i log telemetriju
// - NEW: motion callback (motion==1) => regional može poslati CMD lampi
// ============================================================

class UdpTelemetryServer {
public:
  using MotionCallback = std::function<void(uint32_t zone_id, const std::string& sensor_uri)>;

  UdpTelemetryServer(asio::io_context& io, uint16_t port, Registry& reg, uint32_t region_id)
    : sock_(io, udp::endpoint(udp::v4(), port)), reg_(reg), region_id_(region_id)
  {
    do_receive();
  }

  void set_motion_callback(MotionCallback cb){
    motion_cb_ = std::move(cb);
  }

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

          // Update registry last_seen for sensors too (if exists)
          DeviceState st;
          if(reg_.get(uri, st)){
            st.zone_id = zone_id;
            st.last_seen_ms = now_ms();
            reg_.upsert(uri, st);
          } else {
            // ako nije u registry-ju, ipak možemo napraviti minimalni zapis kao SENSOR
            DeviceState ns;
            ns.type = DeviceType::SENSOR;
            ns.zone_id = zone_id;
            ns.on = 0; ns.intensity = 0; ns.power_mw = 0;
            ns.fault = false;
            ns.last_seen_ms = now_ms();
            reg_.upsert(uri, ns);
          }

          // Log (motion==1)
          if(t.motion == 1){
            std::cout<<"[REGION "<<region_id_<<"] UDP motion uri="<<uri
                     <<" zone="<<zone_id<<" lux="<<lux<<" temp="<<(temp10/10.0)<<"\n";
            if(motion_cb_) motion_cb_(zone_id, uri);
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

  MotionCallback motion_cb_{};
};

} // namespace sls
