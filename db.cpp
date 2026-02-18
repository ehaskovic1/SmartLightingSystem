#include "db.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace sls {

static void check_sqlite(int rc, sqlite3* db, const char* what) {
  if (rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW) return;
  std::string msg = what;
  msg += ": ";
  msg += (db ? sqlite3_errmsg(db) : "unknown");
  throw std::runtime_error(msg);
}

DbWriter::DbWriter() = default;

DbWriter::~DbWriter() {
  stop();
}

void DbWriter::start(const std::string& db_path, const std::string& schema_sql_file) {
  if (running_) return;

  int rc = sqlite3_open(db_path.c_str(), &db_);
  if (rc != SQLITE_OK) {
    std::string msg = "sqlite3_open failed: ";
    msg += sqlite3_errmsg(db_);
    sqlite3_close(db_);
    db_ = nullptr;
    throw std::runtime_error(msg);
  }

  // recommended pragmas
  exec_sql("PRAGMA foreign_keys = ON;");
  exec_sql("PRAGMA journal_mode = WAL;");
  exec_sql("PRAGMA synchronous = NORMAL;");

  if (!schema_sql_file.empty()) {
    apply_schema_from_file(schema_sql_file);
  }

  prepare_statements();

  stop_requested_ = false;
  running_ = true;
  th_ = std::thread([this]{ worker_main(); });
}

void DbWriter::stop() {
  if (!running_) return;

  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_requested_ = true;
  }
  cv_.notify_all();

  if (th_.joinable()) th_.join();
  running_ = false;

  finalize_statements();

  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }

  // clear queue
  {
    std::lock_guard<std::mutex> lk(mu_);
    q_.clear();
  }
}

void DbWriter::enqueue(Event ev) {
  if (!running_) return;
  {
    std::lock_guard<std::mutex> lk(mu_);
    q_.push_back(std::move(ev));
  }
  cv_.notify_one();
}

void DbWriter::log_region_sync(uint32_t region_id, uint32_t version,
                               std::vector<std::pair<uint32_t,uint32_t>> zone_power_sum) {
  RegionSyncEvent e;
  e.ts_ms = now_ms_utc();
  e.region_id = region_id;
  e.version = version;
  e.zone_power_sum = std::move(zone_power_sum);
  enqueue(std::move(e));
}

void DbWriter::upsert_device(std::string uri, uint8_t type, uint32_t region_id, uint32_t zone_id,
                             bool fault, uint8_t on_state, uint8_t intensity, uint32_t power_mw) {
  DeviceUpsertEvent e;
  e.ts_ms = now_ms_utc();
  e.uri = std::move(uri);
  e.type = type;
  e.region_id = region_id;
  e.zone_id = zone_id;
  e.fault = fault;
  e.on_state = on_state;
  e.intensity = intensity;
  e.power_mw = power_mw;
  enqueue(std::move(e));
}

void DbWriter::log_lamp_status(std::string uri, uint32_t region_id, uint32_t zone_id,
                               uint8_t on_state, uint8_t intensity, uint32_t power_mw) {
  LampStatusEvent e{ now_ms_utc(), std::move(uri), region_id, zone_id, on_state, intensity, power_mw };
  enqueue(std::move(e));
}

void DbWriter::log_fault(std::string uri, uint32_t region_id, uint32_t zone_id, uint8_t code, std::string text) {
  FaultEvent e{ now_ms_utc(), std::move(uri), region_id, zone_id, code, std::move(text) };
  enqueue(std::move(e));
}

void DbWriter::log_command(std::string uri, uint32_t region_id, uint8_t cmd, uint8_t value, std::string source) {
  CommandEvent e{ now_ms_utc(), std::move(uri), region_id, cmd, value, std::move(source) };
  enqueue(std::move(e));
}

void DbWriter::apply_schema_from_file(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open schema file: " + path);
  std::stringstream ss;
  ss << f.rdbuf();
  exec_sql(ss.str());
}

void DbWriter::exec_sql(const std::string& sql) {
  char* err = nullptr;
  int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::string msg = "sqlite3_exec failed: ";
    msg += (err ? err : "unknown");
    if (err) sqlite3_free(err);
    throw std::runtime_error(msg);
  }
}

void DbWriter::prepare_statements() {
  // region_sync
  check_sqlite(sqlite3_prepare_v2(db_,
    "INSERT OR IGNORE INTO region_sync(ts_ms, region_id, version) VALUES(?,?,?);",
    -1, &st_region_sync_, nullptr), db_, "prepare region_sync");

  check_sqlite(sqlite3_prepare_v2(db_,
    "INSERT INTO region_zone_power(ts_ms, region_id, version, zone_id, power_sum_mw) VALUES(?,?,?,?,?);",
    -1, &st_region_zone_power_, nullptr), db_, "prepare region_zone_power");

  // device upsert
  check_sqlite(sqlite3_prepare_v2(db_,
    "INSERT INTO devices(uri,type,region_id,zone_id,last_seen_ms,fault,on_state,intensity,power_mw) "
    "VALUES(?,?,?,?,?,?,?,?,?) "
    "ON CONFLICT(uri) DO UPDATE SET "
    "type=excluded.type, region_id=excluded.region_id, zone_id=excluded.zone_id, "
    "last_seen_ms=excluded.last_seen_ms, fault=excluded.fault, on_state=excluded.on_state, "
    "intensity=excluded.intensity, power_mw=excluded.power_mw;",
    -1, &st_device_upsert_, nullptr), db_, "prepare devices upsert");

  // lamp_status
  check_sqlite(sqlite3_prepare_v2(db_,
    "INSERT INTO lamp_status(ts_ms, uri, region_id, zone_id, on_state, intensity, power_mw) "
    "VALUES(?,?,?,?,?,?,?);",
    -1, &st_lamp_status_, nullptr), db_, "prepare lamp_status");

  // faults
  check_sqlite(sqlite3_prepare_v2(db_,
    "INSERT INTO faults(ts_ms, uri, region_id, zone_id, code, text) VALUES(?,?,?,?,?,?);",
    -1, &st_fault_, nullptr), db_, "prepare faults");

  // commands
  check_sqlite(sqlite3_prepare_v2(db_,
    "INSERT INTO commands(ts_ms, uri, region_id, cmd, value, source) VALUES(?,?,?,?,?,?);",
    -1, &st_command_, nullptr), db_, "prepare commands");
}

void DbWriter::finalize_statements() {
  auto fin = [](sqlite3_stmt*& st){ if(st){ sqlite3_finalize(st); st=nullptr; } };
  fin(st_region_sync_);
  fin(st_region_zone_power_);
  fin(st_device_upsert_);
  fin(st_lamp_status_);
  fin(st_fault_);
  fin(st_command_);
}

void DbWriter::worker_main() {
  while (true) {
    Event ev;

    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&]{ return stop_requested_ || !q_.empty(); });

      if (stop_requested_ && q_.empty())
        break;

      ev = std::move(q_.front());
      q_.pop_front();
    }

    try {
      std::visit([this](auto&& x){ handle(x); }, ev);
    } catch (const std::exception& e) {
      // Ne ruši server zbog DB greške; samo ignoriši/loguj
      // (Ako želiš, ovdje možeš std::cerr << ...)
      (void)e;
    }
  }
}

static void reset_and_clear(sqlite3_stmt* st) {
  sqlite3_reset(st);
  sqlite3_clear_bindings(st);
}

void DbWriter::handle(const RegionSyncEvent& e) {
  // insert region_sync
  reset_and_clear(st_region_sync_);
  sqlite3_bind_int64(st_region_sync_, 1, e.ts_ms);
  sqlite3_bind_int(st_region_sync_, 2, (int)e.region_id);
  sqlite3_bind_int(st_region_sync_, 3, (int)e.version);
  check_sqlite(sqlite3_step(st_region_sync_), db_, "step region_sync");

  // insert each zone power
  for (auto& zp : e.zone_power_sum) {
    reset_and_clear(st_region_zone_power_);
    sqlite3_bind_int64(st_region_zone_power_, 1, e.ts_ms);
    sqlite3_bind_int(st_region_zone_power_, 2, (int)e.region_id);
    sqlite3_bind_int(st_region_zone_power_, 3, (int)e.version);
    sqlite3_bind_int(st_region_zone_power_, 4, (int)zp.first);
    sqlite3_bind_int(st_region_zone_power_, 5, (int)zp.second);
    check_sqlite(sqlite3_step(st_region_zone_power_), db_, "step region_zone_power");
  }
}

void DbWriter::handle(const DeviceUpsertEvent& e) {
  reset_and_clear(st_device_upsert_);
  sqlite3_bind_text(st_device_upsert_, 1, e.uri.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st_device_upsert_, 2, (int)e.type);
  sqlite3_bind_int(st_device_upsert_, 3, (int)e.region_id);
  sqlite3_bind_int(st_device_upsert_, 4, (int)e.zone_id);
  sqlite3_bind_int64(st_device_upsert_, 5, e.ts_ms);
  sqlite3_bind_int(st_device_upsert_, 6, e.fault ? 1 : 0);
  sqlite3_bind_int(st_device_upsert_, 7, (int)e.on_state);
  sqlite3_bind_int(st_device_upsert_, 8, (int)e.intensity);
  sqlite3_bind_int(st_device_upsert_, 9, (int)e.power_mw);
  check_sqlite(sqlite3_step(st_device_upsert_), db_, "step device upsert");
}

void DbWriter::handle(const LampStatusEvent& e) {
  reset_and_clear(st_lamp_status_);
  sqlite3_bind_int64(st_lamp_status_, 1, e.ts_ms);
  sqlite3_bind_text(st_lamp_status_, 2, e.uri.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st_lamp_status_, 3, (int)e.region_id);
  sqlite3_bind_int(st_lamp_status_, 4, (int)e.zone_id);
  sqlite3_bind_int(st_lamp_status_, 5, (int)e.on_state);
  sqlite3_bind_int(st_lamp_status_, 6, (int)e.intensity);
  sqlite3_bind_int(st_lamp_status_, 7, (int)e.power_mw);
  check_sqlite(sqlite3_step(st_lamp_status_), db_, "step lamp_status");
}

void DbWriter::handle(const FaultEvent& e) {
  reset_and_clear(st_fault_);
  sqlite3_bind_int64(st_fault_, 1, e.ts_ms);
  sqlite3_bind_text(st_fault_, 2, e.uri.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st_fault_, 3, (int)e.region_id);
  sqlite3_bind_int(st_fault_, 4, (int)e.zone_id);
  sqlite3_bind_int(st_fault_, 5, (int)e.code);
  sqlite3_bind_text(st_fault_, 6, e.text.c_str(), -1, SQLITE_TRANSIENT);
  check_sqlite(sqlite3_step(st_fault_), db_, "step fault");
}

void DbWriter::handle(const CommandEvent& e) {
  reset_and_clear(st_command_);
  sqlite3_bind_int64(st_command_, 1, e.ts_ms);
  sqlite3_bind_text(st_command_, 2, e.uri.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st_command_, 3, (int)e.region_id);
  sqlite3_bind_int(st_command_, 4, (int)e.cmd);
  sqlite3_bind_int(st_command_, 5, (int)e.value);
  sqlite3_bind_text(st_command_, 6, e.source.c_str(), -1, SQLITE_TRANSIENT);
  check_sqlite(sqlite3_step(st_command_), db_, "step command");
}

} // namespace sls
