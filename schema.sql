PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;

-- Uređaji (zadnje poznato stanje po URI)
CREATE TABLE IF NOT EXISTS devices (
  uri           TEXT PRIMARY KEY,
  type          INTEGER NOT NULL,          -- 1 luminaire, 2 sensor
  region_id     INTEGER NOT NULL,
  zone_id       INTEGER NOT NULL,
  last_seen_ms  INTEGER NOT NULL,
  fault         INTEGER NOT NULL DEFAULT 0,
  on_state      INTEGER NOT NULL DEFAULT 0,
  intensity     INTEGER NOT NULL DEFAULT 0,
  power_mw      INTEGER NOT NULL DEFAULT 0
);

-- Svaki STATUS_REPORT (historija)
CREATE TABLE IF NOT EXISTS lamp_status (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  ts_ms         INTEGER NOT NULL,
  uri           TEXT NOT NULL,
  region_id     INTEGER NOT NULL,
  zone_id       INTEGER NOT NULL,
  on_state      INTEGER NOT NULL,
  intensity     INTEGER NOT NULL,
  power_mw      INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_lamp_status_uri_ts ON lamp_status(uri, ts_ms);

-- FAULT report (historija)
CREATE TABLE IF NOT EXISTS faults (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  ts_ms         INTEGER NOT NULL,
  uri           TEXT NOT NULL,
  region_id     INTEGER NOT NULL,
  zone_id       INTEGER NOT NULL,
  code          INTEGER NOT NULL,
  text          TEXT
);
CREATE INDEX IF NOT EXISTS idx_faults_uri_ts ON faults(uri, ts_ms);

-- Agregacija regional->central (historija po verziji)
CREATE TABLE IF NOT EXISTS region_sync (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  ts_ms         INTEGER NOT NULL,
  region_id     INTEGER NOT NULL,
  version       INTEGER NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_region_sync_r_v ON region_sync(region_id, version);

CREATE TABLE IF NOT EXISTS region_zone_power (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  ts_ms         INTEGER NOT NULL,
  region_id     INTEGER NOT NULL,
  version       INTEGER NOT NULL,
  zone_id       INTEGER NOT NULL,
  power_sum_mw  INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_region_zone_power_rz ON region_zone_power(region_id, zone_id);

-- Komande koje su poslane lampama (opciono)
CREATE TABLE IF NOT EXISTS commands (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  ts_ms         INTEGER NOT NULL,
  uri           TEXT NOT NULL,
  region_id     INTEGER NOT NULL,
  cmd           INTEGER NOT NULL,
  value         INTEGER NOT NULL,
  source        TEXT
);

CREATE TABLE IF NOT EXISTS alarm (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    region_id INTEGER,
    ts_unix INTEGER,
    device_type INTEGER,
    zone_id INTEGER,
    code INTEGER,
    uri TEXT,
    text TEXT
);

CREATE INDEX IF NOT EXISTS idx_commands_uri_ts ON commands(uri, ts_ms);
