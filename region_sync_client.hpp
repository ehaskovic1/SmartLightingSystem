/*
#pragma once
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <array>
#include <cstring>

#include "proto.hpp"
#include "registry.hpp"
#include "framed_tls.hpp"

namespace sls {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// ============================================================
// Regional -> Central: periodični SYNC (async)
// - Na svakih N sekundi uzme agregaciju iz Registry (zone_power_sum)
// - Uspostavi TLS konekciju prema centralnom, pošalje REGION_SYNC_UP, primi ACK
// - Ovo vam je "server-server" komunikacija, ali preko centralnog (hijerarhija):
//   uređaji -> regionalni -> centralni
// ============================================================
*/

#pragma once
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <array>
#include <cstring>
#include <vector>
#include <string>

#include "proto.hpp"
#include "registry.hpp"
#include "framed_tls.hpp"
#include "pqc_tls.hpp" //dodala 18feb

namespace sls {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// ============================================================
// Regional -> Central: periodični SYNC (async)
// - Na svakih N sekundi uzme agregaciju iz Registry:
//     * zone_power_sum (već postojalo)
//     * zone_device_summary (NOVO)
// - Uspostavi TLS konekciju prema centralnom, pošalje REGION_SYNC_UP, primi ACK
// ============================================================

class RegionSyncClient : public std::enable_shared_from_this<RegionSyncClient> {
public:
  RegionSyncClient(asio::io_context& io,
                   Registry& reg,
                   uint32_t region_id,
                   std::string central_host,
                   uint16_t central_port,
                   int interval_seconds)
    : io_(io),
      reg_(reg),
      region_id_(region_id),
      central_host_(std::move(central_host)),
      central_port_(central_port),
      interval_s_(interval_seconds),
      ssl_ctx_(asio::ssl::context::tls_client),
      resolver_(io_),
      timer_(io_)
  {
    ssl_ctx_.set_verify_mode(asio::ssl::verify_none); // lab/demo
          sls::apply_pqc_client(ssl_ctx_); //dodala 18.feb
  }


  void start(bool do_first_sync_immediately = true){
    if(do_first_sync_immediately){
      do_sync_once();
    } else {
      schedule_tick();
    }
  }

void send_alarm_now(const AlarmUp& alarm){
    // šalje odmah, ne dira periodicni tick
    post_alarm(alarm);
  }
  
private:
  void schedule_tick(){
    timer_.expires_after(std::chrono::seconds(interval_s_));
    auto self = shared_from_this();
    timer_.async_wait([self](const boost::system::error_code& ec){
      if(ec) return;
      self->do_sync_once();
    });
  }

  void do_sync_once(){
    // pripremi payload iz registra
    RegionSyncUp up;
    up.region_id = region_id_;
    up.version   = ++version_;

    // postojeće: potrošnja po zonama
    up.zone_power_sum = reg_.zone_power_sum();

    // NOVO: summary uređaja po zonama
    // (Ovo zahtijeva da u Registry implementiraš: zone_device_summary())
    up.zone_device_summary = reg_.zone_device_summary();

    // novi socket po tick-u (jednostavnije i robustnije)
    sock_ = std::make_unique<asio::ssl::stream<tcp::socket>>(io_, ssl_ctx_);

    auto self = shared_from_this();
    resolver_.async_resolve(central_host_, std::to_string(central_port_),
      [self, up](const boost::system::error_code& ec, tcp::resolver::results_type eps) mutable {
        if(ec){
          std::cerr<<"[REGION "<<self->region_id_<<"] resolve fail: "<<ec.message()<<"\n";
          self->schedule_tick();
          return;
        }
        asio::async_connect(self->sock_->next_layer(), eps,
          [self, up](const boost::system::error_code& ec2, const tcp::endpoint&) mutable {
            if(ec2){
              std::cerr<<"[REGION "<<self->region_id_<<"] connect fail: "<<ec2.message()<<"\n";
              self->schedule_tick();
              return;
            }
            self->sock_->async_handshake(asio::ssl::stream_base::client,
              [self, up](const boost::system::error_code& ec3) mutable {
                if(ec3){
                  std::cerr<<"[REGION "<<self->region_id_<<"] handshake fail: "<<ec3.message()<<"\n";
                  self->schedule_tick();
                  return;
                }
                self->send_sync(std::move(up));
              });
          });
      });
  }

  void send_sync(RegionSyncUp up){
    auto payload = encode_region_sync_up(up);

    auto self = shared_from_this();
    async_write_frame(*sock_, MsgType::REGION_SYNC_UP, payload,
      [self](const boost::system::error_code& ec, std::size_t){
        if(ec){
          std::cerr<<"[REGION "<<self->region_id_<<"] write fail: "<<ec.message()<<"\n";
          self->schedule_tick();
          return;
        }
        self->read_ack();
      });
  }

  void read_ack(){
    auto self = shared_from_this();

    asio::async_read(*sock_, asio::buffer(hdr_),
      [self](const boost::system::error_code& ec, std::size_t){
        if(ec){
          std::cerr<<"[REGION "<<self->region_id_<<"] read hdr fail: "<<ec.message()<<"\n";
          self->schedule_tick();
          return;
        }

        uint32_t len_be{};
        std::memcpy(&len_be, self->hdr_.data(), 4);
        self->body_len_ = from_be32(len_be);

        self->body_.assign(self->body_len_, 0);

        asio::async_read(*self->sock_, asio::buffer(self->body_),
          [self](const boost::system::error_code& ec2, std::size_t){
            if(ec2){
              std::cerr<<"[REGION "<<self->region_id_<<"] read body fail: "<<ec2.message()<<"\n";
              self->schedule_tick();
              return;
            }
            self->on_ack();
            self->schedule_tick();
          });
      });
  }

  void on_ack(){
    if(body_.empty()) return;

    MsgType t = (MsgType)body_[0];
    if(t != MsgType::REGION_SYNC_ACK) return;

    const uint8_t* p = body_.data()+1;
    size_t n = body_.size()-1;

    try{
      auto ack = decode_region_sync_ack(p, n);
      std::cout<<"[REGION "<<region_id_<<"] SYNC_ACK ok="<<(int)ack.ok
               <<" version="<<ack.version<<"\n";
    } catch(const std::exception& ex){
      std::cerr<<"[REGION "<<region_id_<<"] decode ACK fail: "<<ex.what()<<"\n";
    }
  }
  
  void post_alarm(AlarmUp alarm){
    // novi socket za alarm (isto kao sync), robustno
    alarm_sock_ = std::make_unique<asio::ssl::stream<tcp::socket>>(io_, ssl_ctx_);

    auto self = shared_from_this();
    resolver_.async_resolve(central_host_, std::to_string(central_port_),
      [self, alarm](const boost::system::error_code& ec, tcp::resolver::results_type eps) mutable {
        if(ec){
          std::cerr<<"[REGION "<<self->region_id_<<"] alarm resolve fail: "<<ec.message()<<"\n";
          return;
        }
        asio::async_connect(self->alarm_sock_->next_layer(), eps,
          [self, alarm](const boost::system::error_code& ec2, const tcp::endpoint&) mutable {
            if(ec2){
              std::cerr<<"[REGION "<<self->region_id_<<"] alarm connect fail: "<<ec2.message()<<"\n";
              return;
            }
            self->alarm_sock_->async_handshake(asio::ssl::stream_base::client,
              [self, alarm](const boost::system::error_code& ec3) mutable {
                if(ec3){
                  std::cerr<<"[REGION "<<self->region_id_<<"] alarm handshake fail: "<<ec3.message()<<"\n";
                  return;
                }
                auto payload = encode_alarm_up(alarm);
                async_write_frame(*self->alarm_sock_, MsgType::ALARM_UP, payload,
                  [self](const boost::system::error_code& ecw, std::size_t){
                    if(ecw){
                      std::cerr<<"[REGION "<<self->region_id_<<"] alarm write fail: "<<ecw.message()<<"\n";
                      return;
                    }
                    self->read_alarm_ack();
                  });
              });
          });
      });
  }

  void read_alarm_ack(){
    auto self = shared_from_this();
    asio::async_read(*alarm_sock_, asio::buffer(alarm_hdr_),
      [self](const boost::system::error_code& ec, std::size_t){
        if(ec){
          std::cerr<<"[REGION "<<self->region_id_<<"] alarm read hdr fail: "<<ec.message()<<"\n";
          return;
        }
        uint32_t len_be{};
        std::memcpy(&len_be, self->alarm_hdr_.data(), 4);
        self->alarm_body_len_ = from_be32(len_be);
        self->alarm_body_.assign(self->alarm_body_len_, 0);

        asio::async_read(*self->alarm_sock_, asio::buffer(self->alarm_body_),
          [self](const boost::system::error_code& ec2, std::size_t){
            if(ec2){
              std::cerr<<"[REGION "<<self->region_id_<<"] alarm read body fail: "<<ec2.message()<<"\n";
              return;
            }
            if(self->alarm_body_.empty()) return;
            auto mt = (MsgType)self->alarm_body_[0];
            if(mt == MsgType::ALARM_ACK){
              const uint8_t* p = self->alarm_body_.data()+1;
              size_t n = self->alarm_body_.size()-1;
              try{
                auto ack = decode_alarm_ack(p,n);
                std::cout<<"[REGION "<<self->region_id_<<"] ALARM_ACK ok="<<(int)ack.ok
                         <<" ts="<<ack.ts_unix<<"\n";
              } catch(...) {}
            }
          });
      });
  }

private:
  asio::io_context& io_;
  Registry& reg_;

  uint32_t region_id_{0};
  std::string central_host_;
  uint16_t central_port_{0};
  int interval_s_{5};

  asio::ssl::context ssl_ctx_;
  tcp::resolver resolver_;
  asio::steady_timer timer_;

  std::unique_ptr<asio::ssl::stream<tcp::socket>> sock_;

  uint32_t version_{0};
  std::array<uint8_t,4> hdr_{};
  uint32_t body_len_{0};
  std::vector<uint8_t> body_;
  
  std::unique_ptr<asio::ssl::stream<tcp::socket>> alarm_sock_;
  std::array<uint8_t,4> alarm_hdr_{};
  uint32_t alarm_body_len_{0};
  std::vector<uint8_t> alarm_body_;
  
  
};

} // namespace sls

