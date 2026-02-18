// luminaire_client.cpp
// ============================================================
// Klijent - pametna svjetiljka (Luminaire)
//
// OČEKIVANO ponašanje za integraciju sa regionalnim serverom:
// - Registruje se na regionalni TLS server (REGISTER_REQ -> REGISTER_ACK).
// - Po prijemu CMD (SWITCH_ON / SWITCH_OFF / SET_INTENSITY) mijenja stanje
//   (OFF/ON) i šalje STATUS_REPORT.
// - Ova verzija JE "event-driven": STATUS_REPORT se šalje na komandu i
//   povremeno kao heartbeat (default 10s) da se izbjegne comm_lost.
//
// FSM (prema SRS-u):
//   IDLE -> UNREGISTERED (boot_done)
//   UNREGISTERED -> REGISTERING (REGISTER_REQ)
//   REGISTERING -> OFF (REGISTER_ACK) ili -> UNREGISTERED (NACK/timeout)
//   OFF <-> ON (CMD SWITCH_ON/OFF)
//   ON/OFF -> FAULT (fault_detected) => šalje FAULT_REPORT (opciono)
//   FAULT -> IDLE (manual_reset)
//
// Napomena:
// - fault simulacija je isključena po default-u (prob=0). Ako želite, dodajte arg.
// ============================================================

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <random>
#include <cstring>
#include <map>
#include <string>
#include <algorithm>

#include "proto.hpp"
#include "framed_tls.hpp"
#include "pqc_tls.hpp" //dodala 18feb
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// ===================== FSM definicije =====================

enum class LampState { IDLE, UNREGISTERED, REGISTERING, OFF, ON, FAULT };

static std::string to_string(LampState s){
  switch(s){
    case LampState::IDLE: return "IDLE";
    case LampState::UNREGISTERED: return "UNREGISTERED";
    case LampState::REGISTERING: return "REGISTERING";
    case LampState::OFF: return "OFF";
    case LampState::ON: return "ON";
    case LampState::FAULT: return "FAULT";
  }
  return "UNKNOWN";
}

using TransitionMatrix = std::multimap<std::string, std::string>;

static uint32_t urand(uint32_t a, uint32_t b){
  static std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<uint32_t> d(a,b);
  return d(rng);
}

// ============================================================

class LuminaireClient : public std::enable_shared_from_this<LuminaireClient> {
public:
  LuminaireClient(asio::io_context& io,
                  std::string host, uint16_t port,
                  std::string uri, uint32_t zone_id,
                  int run_seconds,
                  int reset_after_s,
                  int heartbeat_s,
                  int fault_prob_per_mille)
    : io_(io),
      host_(std::move(host)), port_(port),
      uri_(std::move(uri)), zone_(zone_id),
      run_s_(run_seconds),
      reset_after_s_(reset_after_s),
      heartbeat_s_(heartbeat_s),
      fault_prob_per_mille_(fault_prob_per_mille),
      ssl_ctx_(asio::ssl::context::tls_client),
      resolver_(io_),
      timer_stop_(io_),
      timer_reset_(io_),
      timer_register_timeout_(io_),
      timer_reconnect_(io_),
      timer_heartbeat_(io_)
  {
    ssl_ctx_.set_verify_mode(asio::ssl::verify_none); // lab/demo
      sls::apply_pqc_client(ssl_ctx_); //dodala 18.feb
      
    // ------------------- FSM tranzicijska matrica -------------------
    transitions_.insert({"IDLE", "UNREGISTERED"});           // boot_done
    transitions_.insert({"UNREGISTERED", "REGISTERING"});    // send_uri / REGISTER_REQ
    transitions_.insert({"REGISTERING", "OFF"});             // register_OK / REGISTER_ACK
    transitions_.insert({"REGISTERING", "UNREGISTERED"});    // register_fail / REGISTER_NACK ili timeout
    transitions_.insert({"OFF", "ON"});                      // CMD: SWITCH_ON / STATUS_REPORT
    transitions_.insert({"ON", "OFF"});                      // CMD: SWITCH_OFF / STATUS_REPORT
    transitions_.insert({"ON", "FAULT"});                    // fault_detected / FAULT_REPORT
    transitions_.insert({"OFF", "FAULT"});                   // fault_detected / FAULT_REPORT
    transitions_.insert({"FAULT", "IDLE"});                  // manual_reset
  }

  void start(){
    state_ = LampState::IDLE;
    SwitchToState(LampState::UNREGISTERED, "boot_done");

    do_connect();

    // global stop
    if(run_s_>0){
    timer_stop_.expires_after(std::chrono::seconds(run_s_));
    timer_stop_.async_wait([self=shared_from_this()](auto){ self->io_.stop(); });
    }
    // simulirani manual reset (ako je zadano)
    if(reset_after_s_ > 0){
      timer_reset_.expires_after(std::chrono::seconds(reset_after_s_));
      timer_reset_.async_wait([self=shared_from_this()](auto){ self->manual_reset(); });
    }
  }

private:
  // -------------- FSM helper --------------
  bool SwitchToState(LampState next, const std::string& event_label){
    const std::string oldS = to_string(state_);
    const std::string newS = to_string(next);

    bool allowed = false;
    auto range = transitions_.equal_range(oldS);
    for(auto it = range.first; it != range.second; ++it){
      if(it->second == newS){ allowed = true; break; }
    }

    if(!allowed){
      std::cerr << "[LAMP-FSM] INVALID transition: " << oldS << " -> " << newS
                << " (event=" << event_label << ")\n";
      return false;
    }

    state_ = next;
    std::cout << "[LAMP-FSM] " << oldS << " -> " << newS
              << " (event=" << event_label << ")\n";
    return true;
  }

  // -------------- Connection management --------------
  using ssl_socket = asio::ssl::stream<tcp::socket>;

  void make_socket(){
    sock_.reset(new ssl_socket(io_, ssl_ctx_));
  }

  void close_socket(){
    if(!sock_) return;
    boost::system::error_code ec;
    sock_->lowest_layer().shutdown(tcp::socket::shutdown_both, ec);
    sock_->lowest_layer().close(ec);
  }

  void schedule_reconnect(){
    close_socket();
    timer_register_timeout_.cancel();
    timer_heartbeat_.cancel();

    if(state_ != LampState::UNREGISTERED){
      // reset state machine to allow REGISTER again
      state_ = LampState::UNREGISTERED;
      std::cout << "[LAMP] reconnect -> UNREGISTERED\n";
    }

    timer_reconnect_.expires_after(std::chrono::seconds(2));
    auto self = shared_from_this();
    timer_reconnect_.async_wait([self](const boost::system::error_code& ec){
      if(ec) return;
      self->do_connect();
    });
  }

  void do_connect(){
    make_socket();

    auto self = shared_from_this();
    resolver_.async_resolve(host_, std::to_string(port_),
      [self](const boost::system::error_code& ec, tcp::resolver::results_type eps){
        if(ec){
          std::cerr<<"[LAMP] resolve fail: "<<ec.message()<<"\n";
          self->schedule_reconnect();
          return;
        }

        asio::async_connect(self->sock_->next_layer(), eps,
          [self](const boost::system::error_code& ec2, const tcp::endpoint&){
            if(ec2){
              std::cerr<<"[LAMP] connect fail: "<<ec2.message()<<"\n";
              self->schedule_reconnect();
              return;
            }

            self->sock_->async_handshake(asio::ssl::stream_base::client,
              [self](const boost::system::error_code& ec3){
                if(ec3){
                  std::cerr<<"[LAMP] handshake fail: "<<ec3.message()<<"\n";
                  self->schedule_reconnect();
                  return;
                }

                self->send_register();
                self->read_loop();
              });
          });
      });
  }

  // -------------- Register + timeout --------------
  void send_register(){
    if(state_ != LampState::UNREGISTERED && state_ != LampState::REGISTERING){
      // force into UNREGISTERED if caller is reconnect/reset
      state_ = LampState::UNREGISTERED;
    }

    if(!SwitchToState(LampState::REGISTERING, "send_uri/REGISTER_REQ")){
      return;
    }

    sls::RegisterReq r;
    r.type = sls::DeviceType::LUMINAIRE;
    r.uri = uri_;
    r.zone_id = zone_;

    sls::async_write_frame(*sock_, sls::MsgType::REGISTER_REQ, sls::encode_register_req(r),
      [](const boost::system::error_code&, std::size_t){});

    timer_register_timeout_.expires_after(std::chrono::seconds(5));
    auto self = shared_from_this();
    timer_register_timeout_.async_wait([self](const boost::system::error_code& ec){
      if(ec) return;
      if(self->state_ == LampState::REGISTERING){
        std::cerr << "[LAMP] register timeout -> UNREGISTERED\n";
        self->SwitchToState(LampState::UNREGISTERED, "timeout");
        self->schedule_reconnect(); // reconnect is simplest
      }
    });
  }

  // -------------- Heartbeat/status (for comm_lost) --------------
  void schedule_heartbeat(){
    if(heartbeat_s_ <= 0) return;
    timer_heartbeat_.expires_after(std::chrono::seconds(heartbeat_s_));
    auto self = shared_from_this();
    timer_heartbeat_.async_wait([self](const boost::system::error_code& ec){
      if(ec) return;
      if(self->state_ == LampState::ON || self->state_ == LampState::OFF){
        self->send_status();
      }
      self->schedule_heartbeat();
    });
  }

  void send_status(){
    sls::StatusReport s;
    s.uri = uri_;
    s.zone_id = zone_;
    s.on = (state_==LampState::ON) ? 1 : 0;
    s.intensity = intensity_;
    s.power_mw = s.on ? (70000u + 800u*intensity_) : 0u;

    sls::async_write_frame(*sock_, sls::MsgType::STATUS_REPORT, sls::encode_status_report(s),
      [](const boost::system::error_code&, std::size_t){});
  }

  void send_fault(uint8_t code, const std::string& text){
    sls::FaultReport f;
    f.uri = uri_;
    f.zone_id = zone_;
    f.code = code;
    f.text = text;

    sls::async_write_frame(*sock_, sls::MsgType::FAULT_REPORT, sls::encode_fault_report(f),
      [](const boost::system::error_code&, std::size_t){});
  }

  void maybe_simulate_fault(){
    if(fault_prob_per_mille_ <= 0) return;
    if(state_!=LampState::ON && state_!=LampState::OFF) return;

    // npr. svaka komanda/heartbeat može okinuti slučajno
    if((int)urand(0,999) < fault_prob_per_mille_){
      if(SwitchToState(LampState::FAULT, "fault_detected/FAULT_REPORT")){
        send_fault(1, "fault_detected");
      }
    }
  }

  void manual_reset(){
    if(state_ == LampState::FAULT){
      if(SwitchToState(LampState::IDLE, "manual_reset")){
        SwitchToState(LampState::UNREGISTERED, "boot_done");
        schedule_reconnect();
      }
    }
  }

  // -------------- Read loop / message handling --------------
  void read_loop(){
    auto self = shared_from_this();
    asio::async_read(*sock_, asio::buffer(hdr_),
      [self](const boost::system::error_code& ec, std::size_t){
        if(ec){
          self->schedule_reconnect();
          return;
        }
        uint32_t len_be{};
        std::memcpy(&len_be, self->hdr_.data(), 4);
        self->body_len_ = sls::from_be32(len_be);
        if(self->body_len_ < 1 || self->body_len_ > 1024*1024){
          self->schedule_reconnect();
          return;
        }
        self->body_.resize(self->body_len_);
        asio::async_read(*self->sock_, asio::buffer(self->body_),
          [self](const boost::system::error_code& ec2, std::size_t){
            if(ec2){
              self->schedule_reconnect();
              return;
            }
            self->on_message();
            self->read_loop();
          });
      });
  }

  void on_message(){
    if(body_.empty()) return;

    auto t = (sls::MsgType)body_[0];
    const uint8_t* p = body_.data()+1;
    size_t n = body_.size()-1;

    if(t == sls::MsgType::REGISTER_ACK){
      timer_register_timeout_.cancel();
      if(SwitchToState(LampState::OFF, "register_OK/REGISTER_ACK")){
        intensity_ = 0;
        send_status();        // inicijalni status da reg zna da smo OFF
        schedule_heartbeat(); // periodic status za comm_lost
      }
      return;
    }

    if(t == sls::MsgType::REGISTER_NACK){
      timer_register_timeout_.cancel();
      SwitchToState(LampState::UNREGISTERED, "register_fail/REGISTER_NACK");
      schedule_reconnect();
      return;
    }

    if(t == sls::MsgType::CMD){
      try{
        auto c = sls::decode_command(p,n);

        // cmd=1 SWITCH_ON, cmd=2 SWITCH_OFF, cmd=3 SET_INTENSITY
        if(c.cmd==1){ // SWITCH_ON
          if(state_ == LampState::OFF){
            if(SwitchToState(LampState::ON, "CMD:SWITCH_ON/STATUS_REPORT")){
              intensity_ = std::max<uint8_t>(intensity_, 30);
              send_status();
              maybe_simulate_fault();
            }
          }
        } else if(c.cmd==2){ // SWITCH_OFF
          if(state_ == LampState::ON){
            if(SwitchToState(LampState::OFF, "CMD:SWITCH_OFF/STATUS_REPORT")){
              intensity_ = 0;
              send_status();
            }
          }
        } else if(c.cmd==3){ // SET_INTENSITY
          intensity_ = c.value;
          if(intensity_==0 && state_==LampState::ON){
            SwitchToState(LampState::OFF, "CMD:SET_INTENSITY->OFF/STATUS_REPORT");
          } else if(intensity_>0 && state_==LampState::OFF){
            SwitchToState(LampState::ON, "CMD:SET_INTENSITY->ON/STATUS_REPORT");
          }
          if(state_==LampState::ON || state_==LampState::OFF){
            send_status();
            maybe_simulate_fault();
          }
        }
      } catch(...) {}
      return;
    }

    if(t == sls::MsgType::FAULT_ACK){
      std::cout<<"[LAMP] FAULT_ACK received\n";
      return;
    }
  }

private:
  asio::io_context& io_;
  std::string host_;
  uint16_t port_;
  std::string uri_;
  uint32_t zone_;
  int run_s_{0}; // bilo 60
  int reset_after_s_{0};
  int heartbeat_s_{10};
  int fault_prob_per_mille_{0};

  asio::ssl::context ssl_ctx_;
  std::unique_ptr<ssl_socket> sock_;
  tcp::resolver resolver_;

  LampState state_{LampState::IDLE};
  TransitionMatrix transitions_;
  uint8_t intensity_{0};

  asio::steady_timer timer_stop_;
  asio::steady_timer timer_reset_;
  asio::steady_timer timer_register_timeout_;
  asio::steady_timer timer_reconnect_;
  asio::steady_timer timer_heartbeat_;

  std::array<uint8_t,4> hdr_{};
  uint32_t body_len_{0};
  std::vector<uint8_t> body_;
};

int main(int argc, char** argv){
  if(argc < 6){
    std::cerr<<"Usage: luminaire_client <regional_host> <regional_tls_port> <uri> <zone_id> <run_seconds> [reset_after_s] [heartbeat_s] [fault_prob_per_mille]\n";
    return 1;
  }

  asio::io_context io;
  std::string host = argv[1];
  uint16_t port = (uint16_t)std::stoi(argv[2]);
  std::string uri = argv[3];
  uint32_t zone = (uint32_t)std::stoul(argv[4]);
  int run_s = std::stoi(argv[5]);
  int reset_after = (argc>=7) ? std::stoi(argv[6]) : 0;
  int heartbeat_s = (argc>=8) ? std::stoi(argv[7]) : 10;
  int fault_ppm = (argc>=9) ? std::stoi(argv[8]) : 0;
  

  auto c = std::make_shared<LuminaireClient>(io, host, port, uri, zone, run_s, reset_after, heartbeat_s, fault_ppm);
  c->start();
  io.run();
  return 0;
}
