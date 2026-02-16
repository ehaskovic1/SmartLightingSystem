#pragma once
// ============================================================
// Smart Lighting System (SLS) - zajednički protokol (binarni)
// - TCP/TLS: "data-stream" -> framing (len + type + payload)
// - UDP:     "byte-stream" -> fiksna packed struktura TelemetryUdp
//
// Namjena:
//  - Uređaji (svjetiljke/senzori) <-> Regionalni server
//  - Regionalni server <-> Centralni server (agregacija / sync)
// ============================================================

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>

namespace sls {

// -------------------- Tipovi poruka (TCP/TLS) --------------------
enum class MsgType : uint8_t {
  // Device <-> Regional
  REGISTER_REQ   = 1,
  REGISTER_ACK   = 2,
  REGISTER_NACK  = 3,
  STATUS_REPORT  = 4,   // status svjetiljke (on/off, intenzitet, potrošnja)
  CMD            = 5,   // komanda servera prema svjetiljci
  FAULT_REPORT   = 6,   // uređaj prijavljuje kvar (FAULT)
  FAULT_ACK      = 7,   // server potvrdi prijem FAULT_REPORT

  // Regional <-> Central
  REGION_SYNC_UP   = 20, // regional -> central: agregirani podaci (potrošnja po zoni)
  REGION_SYNC_ACK  = 21, // central -> regional: ACK verzije

  // Admin (CLI) <-> Central (snapshot/report)
  ADMIN_SNAPSHOT_REQ = 40,
  ADMIN_SNAPSHOT_ACK = 41
};

enum class DeviceType : uint8_t { LUMINAIRE = 1, SENSOR = 2 };

// -------------------- Endian helperi --------------------
inline uint32_t to_be32(uint32_t x) {
  return ((x & 0x000000FFu) << 24) |
         ((x & 0x0000FF00u) << 8)  |
         ((x & 0x00FF0000u) >> 8)  |
         ((x & 0xFF000000u) >> 24);
}
inline uint32_t from_be32(uint32_t x) { return to_be32(x); }

inline uint16_t to_be16(uint16_t x) { return (uint16_t)((x>>8) | (x<<8)); }
inline uint16_t from_be16(uint16_t x) { return to_be16(x); }

// -------------------- Minimalna serializacija --------------------
inline void put_u8(std::vector<uint8_t>& b, uint8_t v){ b.push_back(v); }
inline void put_u16(std::vector<uint8_t>& b, uint16_t v){
  b.push_back(uint8_t((v>>8)&0xFF)); b.push_back(uint8_t(v&0xFF));
}
inline void put_u32(std::vector<uint8_t>& b, uint32_t v){
  b.push_back(uint8_t((v>>24)&0xFF)); b.push_back(uint8_t((v>>16)&0xFF));
  b.push_back(uint8_t((v>>8)&0xFF));  b.push_back(uint8_t(v&0xFF));
}
inline uint8_t get_u8 (const uint8_t*& p, const uint8_t* e){
  if (p+1>e) throw std::runtime_error("underflow u8"); return *p++;
}
inline uint16_t get_u16(const uint8_t*& p, const uint8_t* e){
  if (p+2>e) throw std::runtime_error("underflow u16");
  uint16_t v = (uint16_t(p[0])<<8) | uint16_t(p[1]); p+=2; return v;
}
inline uint32_t get_u32(const uint8_t*& p, const uint8_t* e){
  if (p+4>e) throw std::runtime_error("underflow u32");
  uint32_t v = (uint32_t(p[0])<<24) | (uint32_t(p[1])<<16) | (uint32_t(p[2])<<8) | uint32_t(p[3]);
  p+=4; return v;
}
inline void put_str(std::vector<uint8_t>& b, const std::string& s){
  if (s.size() > 65535) throw std::runtime_error("string too long");
  put_u16(b, static_cast<uint16_t>(s.size()));
  b.insert(b.end(), s.begin(), s.end());
}
inline std::string get_str(const uint8_t*& p, const uint8_t* e){
  uint16_t n = get_u16(p,e);
  if (p+n>e) throw std::runtime_error("underflow str");
  std::string s(reinterpret_cast<const char*>(p), reinterpret_cast<const char*>(p+n));
  p += n; return s;
}

// -------------------- Strukture poruka (TCP/TLS payload) --------------------
struct RegisterReq {
  DeviceType type{};
  std::string uri;   // jedinstveni identifikator (SRS zahtjev)
  uint32_t zone_id{};
};

struct RegisterAck { uint32_t server_time_unix{}; };

struct StatusReport {
  std::string uri;
  uint32_t zone_id{};
  uint8_t  on{};        // 0/1
  uint8_t  intensity{}; // 0..100
  uint32_t power_mw{};  // potrošnja u mW (demo)
};

struct Command {
  std::string uri;
  uint8_t cmd{};   // 1=SWITCH_ON, 2=SWITCH_OFF, 3=SET_INTENSITY
  uint8_t value{}; // npr intensity (0..100)
};

struct FaultReport {
  std::string uri;
  uint32_t zone_id{};
  uint8_t code{};      // 1=fault_detected, 2=powerloss, 3=comm_lost...
  std::string text;    // opis
};

// Regional -> Central (agregacija)
struct RegionSyncUp {
  uint32_t region_id{};
  uint32_t version{};
  // parovi (zone_id, ukupna_potrošnja_mW) za tu regiju
  std::vector<std::pair<uint32_t,uint32_t>> zone_power_sum;
};
struct RegionSyncAck {
  uint32_t region_id{};
  uint32_t version{};
  uint8_t  ok{};
};

// Admin snapshot (central izvještaj)
struct AdminSnapshotAck {
  // Minimalno: per region: broj zona i suma potrošnje
  // kodiramo kao: regions_count, pa niz: region_id, zones_count, (zone_id,power)...
  // (encode/decode su dole)
  std::vector<uint8_t> raw;
};

// -------------------- UDP telemetry (byte-stream) --------------------
#pragma pack(push,1)
struct TelemetryUdp {
  char     uri[48];        // nul-terminated ili padded nulama
  uint32_t zone_id_be;
  uint16_t lux_be;
  uint8_t  motion;         // 0/1
  int16_t  temp_c_x10_be;  // npr 235 = 23.5C
};
#pragma pack(pop)

// -------------------- Encode/Decode helpers --------------------
inline std::vector<uint8_t> encode_register_req(const RegisterReq& r){
  std::vector<uint8_t> b;
  put_u8(b, static_cast<uint8_t>(r.type));
  put_str(b, r.uri);
  put_u32(b, r.zone_id);
  return b;
}
inline RegisterReq decode_register_req(const uint8_t* p, size_t n){
  const uint8_t* e = p+n;
  RegisterReq r;
  r.type = static_cast<DeviceType>(get_u8(p,e));
  r.uri  = get_str(p,e);
  r.zone_id = get_u32(p,e);
  return r;
}

inline std::vector<uint8_t> encode_register_ack(const RegisterAck& a){
  std::vector<uint8_t> b; put_u32(b, a.server_time_unix); return b;
}
inline RegisterAck decode_register_ack(const uint8_t* p, size_t n){
  const uint8_t* e = p+n;
  RegisterAck a{ get_u32(p,e) };
  return a;
}

inline std::vector<uint8_t> encode_status_report(const StatusReport& s){
  std::vector<uint8_t> b;
  put_str(b, s.uri);
  put_u32(b, s.zone_id);
  put_u8(b, s.on);
  put_u8(b, s.intensity);
  put_u32(b, s.power_mw);
  return b;
}
inline StatusReport decode_status_report(const uint8_t* p, size_t n){
  const uint8_t* e = p+n;
  StatusReport s;
  s.uri = get_str(p,e);
  s.zone_id = get_u32(p,e);
  s.on = get_u8(p,e);
  s.intensity = get_u8(p,e);
  s.power_mw = get_u32(p,e);
  return s;
}

inline std::vector<uint8_t> encode_command(const Command& c){
  std::vector<uint8_t> b;
  put_str(b,c.uri); put_u8(b,c.cmd); put_u8(b,c.value);
  return b;
}
inline Command decode_command(const uint8_t* p, size_t n){
  const uint8_t* e = p+n;
  Command c;
  c.uri = get_str(p,e);
  c.cmd = get_u8(p,e);
  c.value = get_u8(p,e);
  return c;
}

inline std::vector<uint8_t> encode_fault_report(const FaultReport& f){
  std::vector<uint8_t> b;
  put_str(b, f.uri);
  put_u32(b, f.zone_id);
  put_u8(b, f.code);
  put_str(b, f.text);
  return b;
}
inline FaultReport decode_fault_report(const uint8_t* p, size_t n){
  const uint8_t* e = p+n;
  FaultReport f;
  f.uri = get_str(p,e);
  f.zone_id = get_u32(p,e);
  f.code = get_u8(p,e);
  f.text = get_str(p,e);
  return f;
}

inline std::vector<uint8_t> encode_region_sync_up(const RegionSyncUp& s){
  std::vector<uint8_t> b;
  put_u32(b, s.region_id);
  put_u32(b, s.version);
  put_u16(b, static_cast<uint16_t>(s.zone_power_sum.size()));
  for (auto& zp : s.zone_power_sum){
    put_u32(b, zp.first);
    put_u32(b, zp.second);
  }
  return b;
}
inline RegionSyncUp decode_region_sync_up(const uint8_t* p, size_t n){
  const uint8_t* e = p+n;
  RegionSyncUp s;
  s.region_id = get_u32(p,e);
  s.version   = get_u32(p,e);
  uint16_t cnt = get_u16(p,e);
  for(uint16_t i=0;i<cnt;i++){
    uint32_t z = get_u32(p,e);
    uint32_t pw = get_u32(p,e);
    s.zone_power_sum.push_back({z,pw});
  }
  return s;
}
inline std::vector<uint8_t> encode_region_sync_ack(const RegionSyncAck& a){
  std::vector<uint8_t> b;
  put_u32(b, a.region_id);
  put_u32(b, a.version);
  put_u8(b, a.ok);
  return b;
}
inline RegionSyncAck decode_region_sync_ack(const uint8_t* p, size_t n){
  const uint8_t* e = p+n;
  RegionSyncAck a;
  a.region_id = get_u32(p,e);
  a.version = get_u32(p,e);
  a.ok = get_u8(p,e);
  return a;
}

} // namespace sls
