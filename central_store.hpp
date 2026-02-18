/*
#pragma once
#include <unordered_map>
#include <mutex>
#include <vector>
#include <cstdint>

namespace sls {

// Centralni server drži agregirane podatke iz regiona:
// region_id -> { version, zone_power_sum }
struct RegionAggregate {
  uint32_t version{0};
  std::unordered_map<uint32_t,uint32_t> zone_power_mw; // zone_id -> sum power
};

class CentralStore {
public:
  void upsert_region(uint32_t region_id, uint32_t version,
                     const std::vector<std::pair<uint32_t,uint32_t>>& zone_power_sum)
  {
    std::scoped_lock lk(mu_);
    auto& agg = regions_[region_id];
    agg.version = version;
    agg.zone_power_mw.clear();
    for(auto& zp: zone_power_sum){
      agg.zone_power_mw[zp.first] = zp.second;
    }
  }

  std::unordered_map<uint32_t, RegionAggregate> snapshot() const {
    std::scoped_lock lk(mu_);
    return regions_;
  }

private:
  mutable std::mutex mu_;
  std::unordered_map<uint32_t, RegionAggregate> regions_;
};

} // namespace sls

*/


#pragma once
#include <unordered_map>
#include <mutex>
#include <vector>
#include <cstdint>

#include "proto.hpp"

namespace sls {

// Centralni server drži agregirane podatke iz regiona:
// region_id -> { version, zone_power_sum, zone_device_summary }
struct RegionAggregate {
  uint32_t version{0};

  // zone_id -> sum power
  std::unordered_map<uint32_t,uint32_t> zone_power_mw;

  // zone_id -> device summary
  std::unordered_map<uint32_t, ZoneDeviceSummary> zone_summary;
};

class CentralStore {
public:
  // NOVO: prima i power i summary
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

private:
  mutable std::mutex mu_;
  std::unordered_map<uint32_t, RegionAggregate> regions_;
};

} // namespace sls
