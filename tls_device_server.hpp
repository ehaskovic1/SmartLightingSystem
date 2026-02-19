#pragma once
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <array>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <ctime>
#include <cstring>
#include <deque>

#include "proto.hpp"
#include "framed_tls.hpp"
#include "registry.hpp"
#include "pqc_tls.hpp"
#include "db.hpp"

namespace sls {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using ssl_socket = asio::ssl::stream<tcp::socket>;

class DeviceSession : public std::enable_shared_from_this<DeviceSession> {
public:
  using SessionMap = std::unordered_map<std::string, std::weak_ptr<DeviceSession>>;
  using FaultCb    = std::function<void(const FaultReport&, DeviceType)>;

  DeviceSession(ssl_socket sock,
                Registry& reg,
                uint32_t region_id,
                SessionMap& sessions,
                std::mutex& sessions_mu,
                DbWriter& db,
                std::shared_ptr<FaultCb> fault_cb_shared)
    : sock_(std::move(sock)),
      reg_(reg),
      region_id_(region_id),
      sessions_(sessions),
      sessions_mu_(sessions_mu),
      db_(db),
      fault_cb_shared_(std::move(fault_cb_shared))
  {}

  void start(){
    auto self = shared_from_this();
    sock_.async_handshake(asio::ssl::stream_base::server,
      [self](const boost::system::error_code& ec){
        if(ec){
          std::cerr << "[REGION] TLS handshake fail: " << ec.message() << "\n";
          self->on_close();
          return;
        }
        self->read_header();
      });
  }

  void send_cmd(uint8_t cmd, uint8_t value){
    if(uri_.empty()) return;
    Command c{ uri_, cmd, value };
    queue_send(MsgType::CMD, encode_command(c));
  }

private:
  // WRITE QUEUE (SERIALIZE) 
  struct OutMsg { MsgType t; std::vector<uint8_t> p; };
  std::deque<OutMsg> out_q_;
  bool writing_{false};

  void queue_send(MsgType type, std::vector<uint8_t> payload){
    auto self = shared_from_this();
    asio::post(sock_.get_executor(), [self, type, payload=std::move(payload)]() mutable {
      self->out_q_.push_back(OutMsg{type, std::move(payload)});
      if(!self->writing_) self->do_write();
    });
  }

  void do_write(){
    if(out_q_.empty()){
      writing_ = false;
      return;
    }
    writing_ = true;

    auto self = shared_from_this();
    OutMsg msg = std::move(out_q_.front());
    out_q_.pop_front();

    async_write_frame(sock_, msg.t, msg.p,
      [self](const boost::system::error_code& ec, std::size_t){
        if(ec){
          self->on_close();
          boost::system::error_code e2;
          self->sock_.lowest_layer().close(e2);
          return;
        }
        self->do_write();
      });
  }

  // REGISTER 
  void handle_register(const RegisterReq& req){
    if(req.uri.empty() || req.zone_id == 0){
      queue_send(MsgType::REGISTER_NACK, {});
      return;
    }

    registered_   = true;
    session_type_ = req.type;
    uri_          = req.uri;

    DeviceState st;
    st.type = req.type;
    st.zone_id = req.zone_id;
    st.on = 0;
    st.intensity = 0;
    st.power_mw = 0;
    st.fault = false;
    st.last_seen_ms = now_ms();
    reg_.upsert(req.uri, st);

    if(req.type == DeviceType::LUMINAIRE){
      reg_.register_lamp(req.uri, req.zone_id);
    }

    {
      std::scoped_lock lk(sessions_mu_);
      sessions_[uri_] = shared_from_this();
    }

    RegisterAck ack{ static_cast<uint32_t>(std::time(nullptr)) };
    queue_send(MsgType::REGISTER_ACK, encode_register_ack(ack));
  }

  // STATUS 
  void handle_status(const StatusReport& s){
    DeviceState st;
    bool ok = reg_.get(s.uri, st);

    if(!ok){
      if(!registered_ || uri_.empty() || s.uri != uri_) return;
      st.type = session_type_;
      st.zone_id = s.zone_id;
      st.on = 0; st.intensity = 0; st.power_mw = 0;
      st.fault = false;
    }

    st.zone_id = s.zone_id;
    st.on = s.on;
    st.intensity = s.intensity;
    st.power_mw = s.power_mw;
    st.last_seen_ms = now_ms();
    reg_.upsert(s.uri, st);

    db_.upsert_device(
      s.uri,
      (st.type == DeviceType::LUMINAIRE) ? 1 : 2,
      region_id_,
      s.zone_id,
      st.fault,
      s.on,
      s.intensity,
      s.power_mw
    );

    if(st.type == DeviceType::LUMINAIRE){
      db_.log_lamp_status(s.uri, region_id_, s.zone_id, s.on, s.intensity, s.power_mw);
    }
  }

  // FAULT 
  void handle_fault(const FaultReport& f){
    db_.log_fault(f.uri, region_id_, f.zone_id, f.code, f.text);

    DeviceState st;
    bool ok = reg_.get(f.uri, st);

    DeviceType dtype = DeviceType::LUMINAIRE;

    if(ok){
      dtype = st.type;
    } else if(registered_ && !uri_.empty() && f.uri == uri_){
      dtype = session_type_;
      st.type = dtype;
      st.zone_id = f.zone_id;
      st.on = 0; st.intensity = 0; st.power_mw = 0;
      st.fault = false;
    } else {
      dtype = DeviceType::LUMINAIRE;
      st.type = dtype;
      st.zone_id = f.zone_id;
      st.on = 0; st.intensity = 0; st.power_mw = 0;
      st.fault = false;
    }

    st.zone_id = f.zone_id;
    st.fault = true;
    st.last_seen_ms = now_ms();
    reg_.upsert(f.uri, st);

    std::cerr << "[REGION " << region_id_
              << "] FAULT uri=" << f.uri
              << " zone=" << f.zone_id
              << " code=" << int(f.code)
              << " text=" << f.text << "\n";

    if(fault_cb_shared_ && *fault_cb_shared_){
      (*fault_cb_shared_)(f, dtype);
    }

    queue_send(MsgType::FAULT_ACK, {});
  }

  // DISPATCH 
  void on_message(){
    if(body_.empty()) return;

    MsgType t = static_cast<MsgType>(body_[0]);
    const uint8_t* p = body_.data() + 1;
    size_t n = body_.size() - 1;

    try{
      switch(t){
        case MsgType::REGISTER_REQ:  handle_register(decode_register_req(p,n)); break;
        case MsgType::STATUS_REPORT: handle_status(decode_status_report(p,n)); break;
        case MsgType::FAULT_REPORT:  handle_fault(decode_fault_report(p,n)); break;
        default: break;
      }
    } catch(const std::exception& e){
      std::cerr << "[REGION] Decode error: " << e.what() << "\n";
    }
  }

  void read_header(){
    auto self = shared_from_this();
    asio::async_read(sock_, asio::buffer(hdr_),
      [self](const boost::system::error_code& ec, std::size_t){
        if(ec){
          self->on_close();
          boost::system::error_code e2;
          self->sock_.lowest_layer().close(e2);
          return;
        }

        uint32_t len_be{};
        std::memcpy(&len_be, self->hdr_.data(), 4);
        self->body_len_ = from_be32(len_be);

        if(self->body_len_ < 1 || self->body_len_ > (1024*1024)){
          self->on_close();
          boost::system::error_code e2;
          self->sock_.lowest_layer().close(e2);
          return;
        }

        self->body_.assign(self->body_len_, 0);
        self->read_body();
      });
  }

  void read_body(){
    auto self = shared_from_this();
    asio::async_read(sock_, asio::buffer(body_),
      [self](const boost::system::error_code& ec, std::size_t){
        if(ec){
          self->on_close();
          boost::system::error_code e2;
          self->sock_.lowest_layer().close(e2);
          return;
        }
        self->on_message();
        self->read_header();
      });
  }

  void on_close(){
    if(!uri_.empty()){
      std::scoped_lock lk(sessions_mu_);
      auto it = sessions_.find(uri_);
      if(it != sessions_.end()){
        if(auto sp = it->second.lock()){
          if(sp.get() == this) sessions_.erase(it);
        } else {
          sessions_.erase(it);
        }
      }
    }
  }

private:
  ssl_socket sock_;
  Registry& reg_;
  uint32_t region_id_;

  SessionMap& sessions_;
  std::mutex& sessions_mu_;
  DbWriter& db_;

  std::shared_ptr<FaultCb> fault_cb_shared_;

  std::array<uint8_t,4> hdr_{};
  uint32_t body_len_{0};
  std::vector<uint8_t> body_;
  std::string uri_;

  bool registered_{false};
  DeviceType session_type_{DeviceType::LUMINAIRE};
};

class RegionalDeviceServer {
public:
  using FaultCb = DeviceSession::FaultCb;

  RegionalDeviceServer(asio::io_context& io,
                       uint16_t tls_port,
                       const std::string& cert_file,
                       const std::string& key_file,
                       Registry& reg,
                       uint32_t region_id,
                       DbWriter& db)
    : io_(io),
      acceptor_(io, tcp::endpoint(tcp::v4(), tls_port)),
      ssl_ctx_(asio::ssl::context::tls_server),
      reg_(reg),
      region_id_(region_id),
      db_(db),
      fault_cb_shared_(std::make_shared<FaultCb>())
  {
    ssl_ctx_.set_options(
      asio::ssl::context::default_workarounds |
      asio::ssl::context::no_sslv2 |
      asio::ssl::context::no_sslv3
    );

    sls::apply_pqc_server(ssl_ctx_, cert_file, key_file);
    do_accept();
  }

  void set_fault_callback(FaultCb cb){
    *fault_cb_shared_ = std::move(cb);
  }

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
            std::move(ss),
            reg_,
            region_id_,
            sessions_,
            sessions_mu_,
            db_,
            fault_cb_shared_
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
  DbWriter& db_;

  std::shared_ptr<FaultCb> fault_cb_shared_;

  DeviceSession::SessionMap sessions_;
  std::mutex sessions_mu_;
};

} // namespace sls
