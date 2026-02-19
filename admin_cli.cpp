// admin_cli.cpp
// ============================================================
// Admin/Dispatcher CLI: requests a snapshot from the CENTRAL server (synchronously)
// ============================================================

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <cstring>
#include <vector>
#include <string>
#include <cstdint>

#include "proto.hpp"
#include "framed_tls.hpp"
#include "pqc_tls.hpp"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

static void print_snapshot(const std::vector<uint8_t>& payload){
  const uint8_t* p = payload.data();
  const uint8_t* e = p + payload.size();

  try{
    uint16_t regions = sls::get_u16(p, e);
    std::cout << "=== CENTRAL SNAPSHOT ===\n";

    for(uint16_t i = 0; i < regions; i++){
      uint32_t rid = sls::get_u32(p, e);
      uint32_t ver = sls::get_u32(p, e);
      uint16_t zones = sls::get_u16(p, e);

      std::cout << "Region " << rid << " (version " << ver << ") zones=" << zones << "\n";

      for(uint16_t z = 0; z < zones; z++){
        uint32_t zid = sls::get_u32(p, e);
        uint32_t pwr = sls::get_u32(p, e);

        uint16_t lamp_total = sls::get_u16(p, e);
        uint16_t lamp_on    = sls::get_u16(p, e);
        uint16_t lamp_fault = sls::get_u16(p, e);
        uint16_t sens_total = sls::get_u16(p, e);
        uint16_t sens_fault = sls::get_u16(p, e);

        std::cout << "  zone " << zid
                  << " sum_power_mW=" << pwr
                  << " | lamps total=" << lamp_total << " on=" << lamp_on << " fault=" << lamp_fault
                  << " | sensors total=" << sens_total << " fault=" << sens_fault
                  << "\n";
      }
    }

    // Alarms block
    if(p < e){
      uint16_t acnt = sls::get_u16(p, e);
      std::cout << "--- ALARMS (" << acnt << ") ---\n";

      for(uint16_t i = 0; i < acnt; i++){
        uint32_t rid  = sls::get_u32(p, e);
        uint32_t ts   = sls::get_u32(p, e);
        uint8_t  dt   = sls::get_u8(p, e);
        uint32_t zid  = sls::get_u32(p, e);
        uint8_t  code = sls::get_u8(p, e);
        std::string uri  = sls::get_str(p, e);
        std::string text = sls::get_str(p, e);

        std::cout << "  rid=" << rid
                  << " ts=" << ts
                  << " devType=" << (int)dt
                  << " zone=" << zid
                  << " code=" << (int)code
                  << " uri=" << uri
                  << " text=" << text << "\n";
      }
    }

  } catch(const std::exception& ex){
    std::cerr << "decode snapshot fail: " << ex.what() << "\n";
  }
}

int main(int argc, char** argv){
  if(argc < 3){
    std::cerr << "Usage: admin_cli <central_host> <central_tls_port>\n";
    return 1;
  }

  std::string host = argv[1];
  std::string port = argv[2];

  asio::io_context io;
  asio::ssl::context ctx(asio::ssl::context::tls_client);
  ctx.set_verify_mode(asio::ssl::verify_none);

  sls::apply_pqc_client(ctx);

  asio::ssl::stream<tcp::socket> sock(io, ctx);

  tcp::resolver res(io);
  auto eps = res.resolve(host, port);
  asio::connect(sock.next_layer(), eps);
  sock.handshake(asio::ssl::stream_base::client);

  // zahtjev
  sls::write_frame(sock, sls::MsgType::ADMIN_SNAPSHOT_REQ, {});

  // odgovor
  auto body = sls::read_frame(sock);
  if(body.empty()){
    std::cerr << "empty\n";
    return 1;
  }

  auto t = (sls::MsgType)body[0];
  if(t != sls::MsgType::ADMIN_SNAPSHOT_ACK){
    std::cerr << "unexpected msg\n";
    return 1;
  }

  std::vector<uint8_t> payload(body.begin() + 1, body.end());
  print_snapshot(payload);

  return 0;
  }
