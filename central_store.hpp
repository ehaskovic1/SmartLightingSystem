#pragma once
#include <unordered_map>
#include <mutex>
#include <vector>
#include <cstdint>
#include <string>
#include "proto.hpp"

namespace sls {

// The central server maintains aggregated data received from regions:
// region_id -> { version, zone_power_sum, zone_device_summary }

struct RegionAggregate {
  uint32_t version{0};

  // zone_id -> sum power
  std::unordered_map<uint32_t,uint32_t> zone_power_mw;

  // zone_id -> device summary
  std::unordered_map<uint32_t, ZoneDeviceSummary> zone_summary;
};

// Alarm records are stored in CentralStore (global list, last N entries if desired)
struct AlarmRecord {
  uint32_t region_id{0};
  uint32_t ts_unix{0};
  uint8_t  device_type{0}; // 1=LUMINAIRE, 2=SENSOR
  uint32_t zone_id{0};
  uint8_t  code{0};
  std::string uri;
  std::string text;
};

class CentralStore {
public:
  // Receives both power and summary
  void upsert_region(uint32_t region_id,
                     uint32_t version,
                     const std::vector<std::pair<uint32_t,uint32_t>>& zone_power_sum,
                     const std::vector<ZoneDeviceSummary>& zone_device_summary)
  {
    std::scoped_lock lk(mu_);
    auto& agg = regions_[region_id];
    agg.version = version;

    agg.zone_power_mw.clear();
    for(const auto& zp: zone_power_sum){
      agg.zone_power_mw[zp.first] = zp.second;
    }

    agg.zone_summary.clear();
    for(const auto& zs : zone_device_summary){
      agg.zone_summary[zs.zone_id] = zs;
    }
  }

  std::unordered_map<uint32_t, RegionAggregate> snapshot() const {
    std::scoped_lock lk(mu_);
    return regions_;
  }

  // --- ALARMS ---
  void push_alarm(const AlarmRecord& a){
    std::scoped_lock lk(mu_);
    alarms_.push_back(a);

    // Optional: limit the list to prevent unbounded growth
    constexpr size_t MAX_ALARMS = 2000;
    if(alarms_.size() > MAX_ALARMS){
      alarms_.erase(alarms_.begin(), alarms_.begin() + (alarms_.size() - MAX_ALARMS));
    }
  }

  std::vector<AlarmRecord> alarms_snapshot() const{
    std::scoped_lock lk(mu_);
    return alarms_;
  }

private:
  mutable std::mutex mu_;
  std::unordered_map<uint32_t, RegionAggregate> regions_;
  std::vector<AlarmRecord> alarms_;   // <-- samo JEDNOM, bez duplikata
};

} // namespace sls
