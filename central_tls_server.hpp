#pragma once
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <array>
#include <cstring>

#include "proto.hpp"
#include "framed_tls.hpp"
#include "central_store.hpp"

namespace sls {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using ssl_socket = asio::ssl::stream<tcp::socket>;

// ============================================================
// Centralni TLS server (agregacija + admin snapshot)
// - Prima REGION_SYNC_UP od regionalnih servera
// - Vraća REGION_SYNC_ACK
// - Prima ADMIN_SNAPSHOT_REQ (CLI) i vraća agregatno stanje
// ============================================================

class CentralSession : public std::enable_shared_from_this<CentralSession> {
public:
  CentralSession(ssl_socket sock, CentralStore& store)
    : sock_(std::move(sock)), store_(store) {}

  void start(){
    auto self = shared_from_this();
    sock_.async_handshake(asio::ssl::stream_base::server,
      [self](const boost::system::error_code& ec){
        if(ec){ std::cerr<<"[CENTRAL] TLS handshake fail: "<<ec.message()<<"\n"; return; }
        self->read_header();
      });
  }

private:
  void read_header(){
    auto self = shared_from_this();
    asio::async_read(sock_, asio::buffer(hdr_),
      [self](const boost::system::error_code& ec, std::size_t){
        if(ec) return;
        uint32_t len_be{};
        std::memcpy(&len_be, self->hdr_.data(), 4);
        self->body_len_ = from_be32(len_be);
        if(self->body_len_ < 1 || self->body_len_ > (1024*1024)){
          std::cerr<<"[CENTRAL] Bad frame length\n"; return;
        }
        self->body_.resize(self->body_len_);
        self->read_body();
      });
  }

  void read_body(){
    auto self = shared_from_this();
    asio::async_read(sock_, asio::buffer(body_),
      [self](const boost::system::error_code& ec, std::size_t){
        if(ec) return;
        self->on_message();
        self->read_header();
      });
  }

  void async_send(MsgType type, const std::vector<uint8_t>& payload){
    auto self = shared_from_this();
    async_write_frame(sock_, type, payload,
      [self](const boost::system::error_code& ec, std::size_t){
        if(ec){ /* ignore */ }
      });
  }

  std::vector<uint8_t> encode_admin_snapshot(){
    // Format:
    // u16 regions_count
    // for each region:
    //   u32 region_id
    //   u32 version
    //   u16 zones_count
    //   repeated: u32 zone_id, u32 power_mw
    auto snap = store_.snapshot();
    std::vector<uint8_t> out;
    put_u16(out, static_cast<uint16_t>(snap.size()));
    for(const auto& [rid, agg] : snap){
      put_u32(out, rid);
      put_u32(out, agg.version);
      put_u16(out, static_cast<uint16_t>(agg.zone_power_mw.size()));
      for(const auto& [zid, pwr] : agg.zone_power_mw){
        put_u32(out, zid);
        put_u32(out, pwr);
      }
    }
    return out;
  }

  void on_message(){
    MsgType t = static_cast<MsgType>(body_[0]);
    const uint8_t* p = body_.data() + 1;
    size_t n = body_.size() - 1;

    try{
      switch(t){
        case MsgType::REGION_SYNC_UP: {
          auto up = decode_region_sync_up(p,n);
          store_.upsert_region(up.region_id, up.version, up.zone_power_sum);

          RegionSyncAck ack{ up.region_id, up.version, 1 };
          async_send(MsgType::REGION_SYNC_ACK, encode_region_sync_ack(ack));
          break;
        }
        case MsgType::ADMIN_SNAPSHOT_REQ: {
          auto payload = encode_admin_snapshot();
          async_send(MsgType::ADMIN_SNAPSHOT_ACK, payload);
          break;
        }
        default:
          break;
      }
    } catch(const std::exception& e){
      std::cerr<<"[CENTRAL] Decode error: "<<e.what()<<"\n";
    }
  }

private:
  ssl_socket sock_;
  CentralStore& store_;

  std::array<uint8_t,4> hdr_{};
  uint32_t body_len_{0};
  std::vector<uint8_t> body_;
};

class CentralTlsServer {
public:
  CentralTlsServer(asio::io_context& io, uint16_t port,
                   const std::string& cert_file,
                   const std::string& key_file,
                   CentralStore& store)
    : io_(io),
      acceptor_(io, tcp::endpoint(tcp::v4(), port)),
      ssl_ctx_(asio::ssl::context::tls_server),
      store_(store)
  {
    ssl_ctx_.set_options(
      asio::ssl::context::default_workarounds |
      asio::ssl::context::no_sslv2 |
      asio::ssl::context::no_sslv3
    );
    ssl_ctx_.use_certificate_chain_file(cert_file);
    ssl_ctx_.use_private_key_file(key_file, asio::ssl::context::pem);

    do_accept();
  }

private:
  void do_accept(){
    acceptor_.async_accept(
      [this](const boost::system::error_code& ec, tcp::socket sock){
        if(!ec){
          ssl_socket ss(std::move(sock), ssl_ctx_);
          std::make_shared<CentralSession>(std::move(ss), store_)->start();
        }
        do_accept();
      });
  }

private:
  asio::io_context& io_;
  tcp::acceptor acceptor_;
  asio::ssl::context ssl_ctx_;
  CentralStore& store_;
};

} // namespace sls
