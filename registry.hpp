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
  std::unordered_map<uint32_t, std::vector<std::string>> zone_to_lamps_;
};

inline uint64_t now_ms(){
  using namespace std::chrono;
  return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace sls
