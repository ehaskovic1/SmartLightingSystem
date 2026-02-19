#pragma once
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

#include "proto.hpp"

namespace sls {

// Time (ms) - uses steady_clock (suitable for timeouts)
inline uint64_t now_ms(){
  using namespace std::chrono;
  return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Device state maintained by the regional server in the registry
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

    // Maintain a zone->lamps map for automatic commands (motion -> SWITCH_ON)
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

  std::vector<std::pair<uint32_t,uint32_t>> zone_power_sum() const {
    std::scoped_lock lk(mu_);
    std::unordered_map<uint32_t,uint32_t> sums;

    for(const auto& kv : dev_){
      const auto& st = kv.second;
      if(st.zone_id == 0) continue;

      if(st.type == DeviceType::LUMINAIRE){
        sums[st.zone_id] += st.power_mw;
      }
    }

    std::vector<std::pair<uint32_t,uint32_t>> out;
    out.reserve(sums.size());
    for(auto& kv : sums) out.push_back(kv);
    return out;
  }

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

    std::sort(out.begin(), out.end(), [](const ZoneDeviceSummary& a, const ZoneDeviceSummary& b){
      return a.zone_id < b.zone_id;
    });

    return out;
  }

  std::unordered_map<std::string,DeviceState> snapshot() const {
    std::scoped_lock lk(mu_);
    return dev_;
  }

  // "comm_lost" detection: if no status/telemetry is received for an extended period
  std::vector<std::string> detect_comm_lost(uint64_t now, uint64_t timeout_ms){
    std::vector<std::string> lost;
    std::scoped_lock lk(mu_);

    for(auto& [uri, st] : dev_){
      if(!st.fault && st.last_seen_ms > 0 && (now - st.last_seen_ms) > timeout_ms){
        st.fault = true; 
        lost.push_back(uri);
      }
    }
    return lost;
  }

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

