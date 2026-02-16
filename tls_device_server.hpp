#pragma once
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <array>
#include <unordered_map>
#include <mutex>

#include "proto.hpp"
#include "framed_tls.hpp"
#include "registry.hpp"

namespace sls {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using ssl_socket = asio::ssl::stream<tcp::socket>;

// ============================================================
// Regionalni TLS server za uređaje (svjetiljke/senzori)
// - Async accept + async_read (header/body)
// - REGISTER_REQ -> ACK/NACK, upis u Registry
// - STATUS_REPORT -> update Registry
// - FAULT_REPORT  -> izolacija + FAULT_ACK
//
// NEW:
// - RegionalDeviceServer pamti mapu uri -> session
// - send_cmd_to_uri(uri, cmd, value) omogućava regionalnom serveru
//   da na događaj senzora (motion) pošalje komandu lampi.
// ============================================================

class DeviceSession : public std::enable_shared_from_this<DeviceSession> {
public:
  using SessionMap = std::unordered_map<std::string, std::weak_ptr<DeviceSession>>;

  DeviceSession(ssl_socket sock, Registry& reg, uint32_t region_id,
                SessionMap& sessions, std::mutex& sessions_mu)
    : sock_(std::move(sock)),
      reg_(reg),
      region_id_(region_id),
      sessions_(sessions),
      sessions_mu_(sessions_mu)
  {}

  void start(){
    auto self = shared_from_this();
    sock_.async_handshake(asio::ssl::stream_base::server,
      [self](const boost::system::error_code& ec){
        if(ec){ std::cerr<<"[REGION] TLS handshake fail: "<<ec.message()<<"\n"; return; }
        self->read_header();
      });
  }

  void send_cmd(uint8_t cmd, uint8_t value){
    if(uri_.empty()) return;
    Command c{ uri_, cmd, value };
    async_send(MsgType::CMD, encode_command(c));
  }

  std::string uri() const { return uri_; }

private:
  void read_header(){
    auto self = shared_from_this();
    asio::async_read(sock_, asio::buffer(hdr_),
      [self](const boost::system::error_code& ec, std::size_t){
        if(ec){
          self->on_close();
          return;
        }
        uint32_t len_be{};
        std::memcpy(&len_be, self->hdr_.data(), 4);
        self->body_len_ = from_be32(len_be);
        if(self->body_len_ < 1 || self->body_len_ > (1024*1024)){
          std::cerr<<"[REGION] Bad frame length\n";
          self->on_close();
          return;
        }
        self->body_.resize(self->body_len_);
        self->read_body();
      });
  }

  void read_body(){
    auto self = shared_from_this();
    asio::async_read(sock_, asio::buffer(body_),
      [self](const boost::system::error_code& ec, std::size_t){
        if(ec){
          self->on_close();
          return;
        }
        self->on_message();
        self->read_header();
      });
  }

  void async_send(MsgType type, const std::vector<uint8_t>& payload){
    auto self = shared_from_this();
    async_write_frame(sock_, type, payload,
      [self](const boost::system::error_code& ec, std::size_t){
        if(ec){
          self->on_close();
        }
      });
  }

  void on_close(){
    // izbaci session iz mape ako smo bili registrovani
    if(!uri_.empty()){
      std::scoped_lock lk(sessions_mu_);
      auto it = sessions_.find(uri_);
      if(it != sessions_.end()){
        // briši samo ako pokazuje na ovu sesiju (best-effort)
        if(auto sp = it->second.lock()){
          if(sp.get() == this) sessions_.erase(it);
        } else {
          sessions_.erase(it);
        }
      }
    }
  }

  void on_message(){
    MsgType t = static_cast<MsgType>(body_[0]);
    const uint8_t* p = body_.data() + 1;
    size_t n = body_.size() - 1;

    try {
      switch(t){
        case MsgType::REGISTER_REQ: {
          auto req = decode_register_req(p,n);

          // Validacija URI i zone_id
          if(req.uri.empty() || req.zone_id==0){
            async_send(MsgType::REGISTER_NACK, {});
            break;
          }

          // upis u registar
          DeviceState st;
          st.type = req.type;
          st.zone_id = req.zone_id;
          st.on = 0; st.intensity = 0; st.power_mw = 0;
          st.fault = false;
          st.last_seen_ms = now_ms();
          reg_.upsert(req.uri, st);

          // zapamti uri session-a
          uri_ = req.uri;

          // registruj lampu u zone mapu (za motion->lamp)
          if(req.type == DeviceType::LUMINAIRE){
            reg_.register_lamp(req.uri, req.zone_id);
          }

          // ubaci u sessions mapu da bi server mogao slati CMD
          {
            std::scoped_lock lk(sessions_mu_);
            sessions_[uri_] = shared_from_this();
          }

          RegisterAck ack{ static_cast<uint32_t>(std::time(nullptr)) };
          async_send(MsgType::REGISTER_ACK, encode_register_ack(ack));
          break;
        }

        case MsgType::STATUS_REPORT: {
          auto s = decode_status_report(p,n);
          DeviceState st;
          if(!reg_.get(s.uri, st)){
            break; // nije registrovan
          }
          st.zone_id = s.zone_id;
          st.on = s.on;
          st.intensity = s.intensity;
          st.power_mw = s.power_mw;
          st.last_seen_ms = now_ms();
          reg_.upsert(s.uri, st);
          break;
        }

        case MsgType::FAULT_REPORT: {
          auto f = decode_fault_report(p,n);
          DeviceState st;
          if(reg_.get(f.uri, st)){
            st.fault = true;
            st.last_seen_ms = now_ms();
            reg_.upsert(f.uri, st);
          }
          std::cerr<<"[REGION "<<region_id_<<"] FAULT uri="<<f.uri<<" zone="<<f.zone_id
                   <<" code="<<(int)f.code<<" text="<<f.text<<"\n";
          async_send(MsgType::FAULT_ACK, {});
          break;
        }

        default:
          break;
      }
    } catch(const std::exception& e){
      std::cerr<<"[REGION] Decode error: "<<e.what()<<"\n";
    }
  }

private:
  ssl_socket sock_;
  Registry& reg_;
  uint32_t region_id_{0};

  SessionMap& sessions_;
  std::mutex& sessions_mu_;

  std::array<uint8_t,4> hdr_{};
  uint32_t body_len_{0};
  std::vector<uint8_t> body_;

  std::string uri_;
};

class RegionalDeviceServer {
public:
  RegionalDeviceServer(asio::io_context& io,
                       uint16_t tls_port,
                       const std::string& cert_file,
                       const std::string& key_file,
                       Registry& reg,
                       uint32_t region_id)
    : io_(io),
      acceptor_(io, tcp::endpoint(tcp::v4(), tls_port)),
      ssl_ctx_(asio::ssl::context::tls_server),
      reg_(reg),
      region_id_(region_id)
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

  // NEW: regional_server može pozvati ovo kada dobije motion event
  bool send_cmd_to_uri(const std::string& uri, uint8_t cmd, uint8_t value){
    std::shared_ptr<DeviceSession> s;
    {
      std::scoped_lock lk(sessions_mu_);
      auto it = sessions_.find(uri);
      if(it == sessions_.end()) return false;
      s = it->second.lock();
      if(!s){
        sessions_.erase(it);
        return false;
      }
    }
    s->send_cmd(cmd, value);
    return true;
  }

private:
  void do_accept(){
    acceptor_.async_accept(
      [this](const boost::system::error_code& ec, tcp::socket sock){
        if(!ec){
          ssl_socket ss(std::move(sock), ssl_ctx_);
          std::make_shared<DeviceSession>(
            std::move(ss), reg_, region_id_, sessions_, sessions_mu_
          )->start();
        }
        do_accept();
      });
  }

private:
  asio::io_context& io_;
  tcp::acceptor acceptor_;
  asio::ssl::context ssl_ctx_;
  Registry& reg_;
  uint32_t region_id_{0};

  DeviceSession::SessionMap sessions_;
  std::mutex sessions_mu_;
};

} // namespace sls
