#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <variant>
#include <atomic>

#include <sqlite3.h>

namespace sls {

inline int64_t now_ms_utc() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

class DbWriter {
public:
  struct RegionSyncEvent {
    int64_t ts_ms;
    uint32_t region_id;
    uint32_t version;
    std::vector<std::pair<uint32_t,uint32_t>> zone_power_sum; // (zone_id, power_sum_mw)
  };

  struct DeviceUpsertEvent {
    int64_t ts_ms;
    std::string uri;
    uint8_t type;           // 1 luminaire, 2 sensor
    uint32_t region_id;
    uint32_t zone_id;
    bool fault;
    uint8_t on_state;
    uint8_t intensity;
    uint32_t power_mw;
  };

  struct LampStatusEvent {
    int64_t ts_ms;
    std::string uri;
    uint32_t region_id;
    uint32_t zone_id;
    uint8_t on_state;
    uint8_t intensity;
    uint32_t power_mw;
  };

  struct FaultEvent {
    int64_t ts_ms;
    std::string uri;
    uint32_t region_id;
    uint32_t zone_id;
    uint8_t code;
    std::string text;
  };

  struct CommandEvent {
    int64_t ts_ms;
    std::string uri;
    uint32_t region_id;
    uint8_t cmd;
    uint8_t value;
    std::string source;
  };

  using Event = std::variant<RegionSyncEvent, DeviceUpsertEvent, LampStatusEvent, FaultEvent, CommandEvent>;

  DbWriter();
  ~DbWriter();

  DbWriter(const DbWriter&) = delete;
  DbWriter& operator=(const DbWriter&) = delete;

  // Open DB + apply schema (schema_sql_file is optional; can be "")
  void start(const std::string& db_path, const std::string& schema_sql_file = "");
  void stop();

  // enqueue events (non-blocking)
  void enqueue(Event ev);

  // convenience wrappers
  void log_region_sync(uint32_t region_id, uint32_t version,
                       std::vector<std::pair<uint32_t,uint32_t>> zone_power_sum);

  void upsert_device(std::string uri, uint8_t type, uint32_t region_id, uint32_t zone_id,
                     bool fault, uint8_t on_state, uint8_t intensity, uint32_t power_mw);

  void log_lamp_status(std::string uri, uint32_t region_id, uint32_t zone_id,
                       uint8_t on_state, uint8_t intensity, uint32_t power_mw);

  void log_fault(std::string uri, uint32_t region_id, uint32_t zone_id, uint8_t code, std::string text);

  void log_command(std::string uri, uint32_t region_id, uint8_t cmd, uint8_t value, std::string source);

private:
  void worker_main();
  void apply_schema_from_file(const std::string& path);
  void exec_sql(const std::string& sql);
  void prepare_statements();
  void finalize_statements();

  // handlers
  void handle(const RegionSyncEvent& e);
  void handle(const DeviceUpsertEvent& e);
  void handle(const LampStatusEvent& e);
  void handle(const FaultEvent& e);
  void handle(const CommandEvent& e);

  // sqlite
  sqlite3* db_{nullptr};

  sqlite3_stmt* st_region_sync_{nullptr};
  sqlite3_stmt* st_region_zone_power_{nullptr};

  sqlite3_stmt* st_device_upsert_{nullptr};
  sqlite3_stmt* st_lamp_status_{nullptr};
  sqlite3_stmt* st_fault_{nullptr};
  sqlite3_stmt* st_command_{nullptr};

  // thread + queue
  std::thread th_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Event> q_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
};

} // namespace sls
