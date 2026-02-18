/*
#pragma once
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

#include "proto.hpp"

namespace sls {

// Stanje uređaja koje regionalni server drži u registru
struct DeviceState {
  DeviceType type{DeviceType::LUMINAIRE};
  uint32_t zone_id{};
  uint8_t  on{};
  uint8_t  intensity{};
  uint32_t power_mw{};
  bool     fault{};
  uint64_t last_seen_ms{};
};

class Registry {
public:
  void upsert(const std::string& uri, const DeviceState& st){
    std::scoped_lock lk(mu_);
    dev_[uri] = st;

    // Održavaj mapu zone->lamps za automatske komande (motion -> SWITCH_ON)
    if(st.type == DeviceType::LUMINAIRE && st.zone_id != 0){
      auto& v = zone_to_lamps_[st.zone_id];
      if(std::find(v.begin(), v.end(), uri) == v.end())
        v.push_back(uri);
    }
  }

  bool get(const std::string& uri, DeviceState& out) const {
    std::scoped_lock lk(mu_);
    auto it = dev_.find(uri);
    if(it==dev_.end()) return false;
    out = it->second;
    return true;
  }

  // Agregacija potrošnje po zoni (potrebno za slanje ka centralnom serveru)
  std::vector<std::pair<uint32_t,uint32_t>> zone_power_sum() const {
    std::scoped_lock lk(mu_);
    std::unordered_map<uint32_t,uint32_t> sums;
    for(const auto& kv : dev_){
      const auto& st = kv.second;
      sums[st.zone_id] += st.power_mw;
    }
    std::vector<std::pair<uint32_t,uint32_t>> out;
    out.reserve(sums.size());
    for(auto& kv : sums) out.push_back(kv);
    return out;
  }

  std::unordered_map<std::string,DeviceState> snapshot() const {
    std::scoped_lock lk(mu_);
    return dev_;
  }

  // detekcija "comm_lost": ako dugo nema status/telemetry
  std::vector<std::string> detect_comm_lost(uint64_t now_ms, uint64_t timeout_ms){
    std::vector<std::string> lost;
    std::scoped_lock lk(mu_);
    for(auto& [uri, st] : dev_){
      if(!st.fault && st.last_seen_ms>0 && (now_ms - st.last_seen_ms) > timeout_ms){
        st.fault = true; // izolacija/oznaka
        lost.push_back(uri);
      }
    }
    return lost;
  }

  // --- NEW: registruj lampu i vrati lampe u zoni (za motion-trigger) ---
  void register_lamp(const std::string& uri, uint32_t zone_id){
    std::scoped_lock lk(mu_);
    auto& v = zone_to_lamps_[zone_id];
    if(std::find(v.begin(), v.end(), uri) == v.end())
      v.push_back(uri);
  }

  std::vector<std::string> get_lamps_in_zone(uint32_t zone_id) const{
    std::scoped_lock lk(mu_);
    auto it = zone_to_lamps_.find(zone_id);
    if(it == zone_to_lamps_.end()) return {};
    return it->second;
  }

private:
  mutable std::mutex mu_;
  std::unordered_map<std::string,DeviceState> dev_;

  // zone_id -> list of lamp URIs
  std::unordered_map<uint32_t, std::vector<std::string>> zone_to_lamps_;s
};

inline uint64_t now_ms(){
  using namespace std::chrono;
  return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace sls

*/
/*
#pragma once
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <array>
#include <cstring>
#include <vector>
#include <string>

#include "proto.hpp"
#include "registry.hpp"
#include "framed_tls.hpp"

namespace sls {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// ============================================================
// Regional -> Central: periodični SYNC (async)
// - Na svakih N sekundi uzme agregaciju iz Registry:
//     * zone_power_sum (već postojalo)
//     * zone_device_summary (NOVO)
// - Uspostavi TLS konekciju prema centralnom, pošalje REGION_SYNC_UP, primi ACK
// ============================================================

class RegionSyncClient : public std::enable_shared_from_this<RegionSyncClient> {
public:
  RegionSyncClient(asio::io_context& io,
                   Registry& reg,
                   uint32_t region_id,
                   std::string central_host,
                   uint16_t central_port,
                   int interval_seconds)
    : io_(io),
      reg_(reg),
      region_id_(region_id),
      central_host_(std::move(central_host)),
      central_port_(central_port),
      interval_s_(interval_seconds),
      ssl_ctx_(asio::ssl::context::tls_client),
      resolver_(io_),
      timer_(io_)
  {
    ssl_ctx_.set_verify_mode(asio::ssl::verify_none); // lab/demo
  }

  void start(bool do_first_sync_immediately = true){
    if(do_first_sync_immediately){
      do_sync_once();
    } else {
      schedule_tick();
    }
  }

private:
  void schedule_tick(){
    timer_.expires_after(std::chrono::seconds(interval_s_));
    auto self = shared_from_this();
    timer_.async_wait([self](const boost::system::error_code& ec){
      if(ec) return;
      self->do_sync_once();
    });
  }

  void do_sync_once(){
    // pripremi payload iz registra
    RegionSyncUp up;
    up.region_id = region_id_;
    up.version   = ++version_;

    // postojeće: potrošnja po zonama
    up.zone_power_sum = reg_.zone_power_sum();

    // NOVO: summary uređaja po zonama
    // (Ovo zahtijeva da u Registry implementiraš: zone_device_summary())
    up.zone_device_summary = reg_.zone_device_summary();

    // novi socket po tick-u (jednostavnije i robustnije)
    sock_ = std::make_unique<asio::ssl::stream<tcp::socket>>(io_, ssl_ctx_);

    auto self = shared_from_this();
    resolver_.async_resolve(central_host_, std::to_string(central_port_),
      [self, up](const boost::system::error_code& ec, tcp::resolver::results_type eps) mutable {
        if(ec){
          std::cerr<<"[REGION "<<self->region_id_<<"] resolve fail: "<<ec.message()<<"\n";
          self->schedule_tick();
          return;
        }
        asio::async_connect(self->sock_->next_layer(), eps,
          [self, up](const boost::system::error_code& ec2, const tcp::endpoint&) mutable {
            if(ec2){
              std::cerr<<"[REGION "<<self->region_id_<<"] connect fail: "<<ec2.message()<<"\n";
              self->schedule_tick();
              return;
            }
            self->sock_->async_handshake(asio::ssl::stream_base::client,
              [self, up](const boost::system::error_code& ec3) mutable {
                if(ec3){
                  std::cerr<<"[REGION "<<self->region_id_<<"] handshake fail: "<<ec3.message()<<"\n";
                  self->schedule_tick();
                  return;
                }
                self->send_sync(std::move(up));
              });
          });
      });
  }

  void send_sync(RegionSyncUp up){
    auto payload = encode_region_sync_up(up);

    auto self = shared_from_this();
    async_write_frame(*sock_, MsgType::REGION_SYNC_UP, payload,
      [self](const boost::system::error_code& ec, std::size_t){
        if(ec){
          std::cerr<<"[REGION "<<self->region_id_<<"] write fail: "<<ec.message()<<"\n";
          self->schedule_tick();
          return;
        }
        self->read_ack();
      });
  }

  void read_ack(){
    auto self = shared_from_this();

    asio::async_read(*sock_, asio::buffer(hdr_),
      [self](const boost::system::error_code& ec, std::size_t){
        if(ec){
          std::cerr<<"[REGION "<<self->region_id_<<"] read hdr fail: "<<ec.message()<<"\n";
          self->schedule_tick();
          return;
        }

        uint32_t len_be{};
        std::memcpy(&len_be, self->hdr_.data(), 4);
        self->body_len_ = from_be32(len_be);

        self->body_.assign(self->body_len_, 0);

        asio::async_read(*self->sock_, asio::buffer(self->body_),
          [self](const boost::system::error_code& ec2, std::size_t){
            if(ec2){
              std::cerr<<"[REGION "<<self->region_id_<<"] read body fail: "<<ec2.message()<<"\n";
              self->schedule_tick();
              return;
            }
            self->on_ack();
            self->schedule_tick();
          });
      });
  }

  void on_ack(){
    if(body_.empty()) return;

    MsgType t = (MsgType)body_[0];
    if(t != MsgType::REGION_SYNC_ACK) return;

    const uint8_t* p = body_.data()+1;
    size_t n = body_.size()-1;

    try{
      auto ack = decode_region_sync_ack(p, n);
      std::cout<<"[REGION "<<region_id_<<"] SYNC_ACK ok="<<(int)ack.ok
               <<" version="<<ack.version<<"\n";
    } catch(const std::exception& ex){
      std::cerr<<"[REGION "<<region_id_<<"] decode ACK fail: "<<ex.what()<<"\n";
    }
  }

private:
  asio::io_context& io_;
  Registry& reg_;

  uint32_t region_id_{0};
  std::string central_host_;
  uint16_t central_port_{0};
  int interval_s_{5};

  asio::ssl::context ssl_ctx_;
  tcp::resolver resolver_;
  asio::steady_timer timer_;

  std::unique_ptr<asio::ssl::stream<tcp::socket>> sock_;

  uint32_t version_{0};
  std::array<uint8_t,4> hdr_{};
  uint32_t body_len_{0};
  std::vector<uint8_t> body_;
};

} // namespace sls
*/

#pragma once
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

#include "proto.hpp"

namespace sls {

// vrijeme (ms) - koristi steady_clock (ok za timeout-e)
inline uint64_t now_ms(){
  using namespace std::chrono;
  return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Stanje uređaja koje regionalni server drži u registru
struct DeviceState {
  DeviceType type{DeviceType::LUMINAIRE};
  uint32_t zone_id{};
  uint8_t  on{};
  uint8_t  intensity{};
  uint32_t power_mw{};
  bool     fault{};
  uint64_t last_seen_ms{};
};

class Registry {
public:
  void upsert(const std::string& uri, const DeviceState& st){
    std::scoped_lock lk(mu_);
    dev_[uri] = st;

    // Održavaj mapu zone->lamps za automatske komande (motion -> SWITCH_ON)
    if(st.type == DeviceType::LUMINAIRE && st.zone_id != 0){
      auto& v = zone_to_lamps_[st.zone_id];
      if(std::find(v.begin(), v.end(), uri) == v.end())
        v.push_back(uri);
    }
  }

  bool get(const std::string& uri, DeviceState& out) const {
    std::scoped_lock lk(mu_);
    auto it = dev_.find(uri);
    if(it==dev_.end()) return false;
    out = it->second;
    return true;
  }

  // Agregacija potrošnje po zoni (preporuka: samo LUMINAIRE utiče na power)
  std::vector<std::pair<uint32_t,uint32_t>> zone_power_sum() const {
    std::scoped_lock lk(mu_);
    std::unordered_map<uint32_t,uint32_t> sums;

    for(const auto& kv : dev_){
      const auto& st = kv.second;
      if(st.zone_id == 0) continue;

      // samo lampe zbrajamo u potrošnju
      if(st.type == DeviceType::LUMINAIRE){
        sums[st.zone_id] += st.power_mw;
      }
    }

    std::vector<std::pair<uint32_t,uint32_t>> out;
    out.reserve(sums.size());
    for(auto& kv : sums) out.push_back(kv);
    return out;
  }

  // NOVO: summary uređaja po zoni (region -> central)
  std::vector<ZoneDeviceSummary> zone_device_summary() const {
    std::scoped_lock lk(mu_);

    struct Acc {
      uint16_t lamp_total = 0;
      uint16_t lamp_on = 0;
      uint16_t lamp_fault = 0;
      uint16_t sensor_total = 0;
      uint16_t sensor_fault = 0;
    };

    std::unordered_map<uint32_t, Acc> acc;

    for(const auto& kv : dev_){
      const auto& st = kv.second;
      if(st.zone_id == 0) continue;

      auto& a = acc[st.zone_id];

      if(st.type == DeviceType::LUMINAIRE){
        a.lamp_total++;
        if(st.on) a.lamp_on++;
        if(st.fault) a.lamp_fault++;
      } else { // SENSOR
        a.sensor_total++;
        if(st.fault) a.sensor_fault++;
      }
    }

    std::vector<ZoneDeviceSummary> out;
    out.reserve(acc.size());
    for(const auto& kv : acc){
      ZoneDeviceSummary z{};
      z.zone_id      = kv.first;
      z.lamp_total   = kv.second.lamp_total;
      z.lamp_on      = kv.second.lamp_on;
      z.lamp_fault   = kv.second.lamp_fault;
      z.sensor_total = kv.second.sensor_total;
      z.sensor_fault = kv.second.sensor_fault;
      out.push_back(z);
    }

    // čisto radi stabilnog ispisa/snapshot-a
    std::sort(out.begin(), out.end(), [](const ZoneDeviceSummary& a, const ZoneDeviceSummary& b){
      return a.zone_id < b.zone_id;
    });

    return out;
  }

  std::unordered_map<std::string,DeviceState> snapshot() const {
    std::scoped_lock lk(mu_);
    return dev_;
  }

  // detekcija "comm_lost": ako dugo nema status/telemetry
  std::vector<std::string> detect_comm_lost(uint64_t now, uint64_t timeout_ms){
    std::vector<std::string> lost;
    std::scoped_lock lk(mu_);

    for(auto& [uri, st] : dev_){
      if(!st.fault && st.last_seen_ms > 0 && (now - st.last_seen_ms) > timeout_ms){
        st.fault = true; // izolacija/oznaka
        lost.push_back(uri);
      }
    }
    return lost;
  }

  // --- Lampe po zoni (motion-trigger) ---
  void register_lamp(const std::string& uri, uint32_t zone_id){
    std::scoped_lock lk(mu_);
    auto& v = zone_to_lamps_[zone_id];
    if(std::find(v.begin(), v.end(), uri) == v.end())
      v.push_back(uri);
  }

  std::vector<std::string> get_lamps_in_zone(uint32_t zone_id) const{
    std::scoped_lock lk(mu_);
    auto it = zone_to_lamps_.find(zone_id);
    if(it == zone_to_lamps_.end()) return {};
    return it->second;
  }

private:
  mutable std::mutex mu_;
  std::unordered_map<std::string,DeviceState> dev_;
  std::unordered_map<uint32_t, std::vector<std::string>> zone_to_lamps_;
};

} // namespace sls

