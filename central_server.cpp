// central_server.cpp
// ============================================================
// Central server:
// - Receives aggregations from 2 regional servers (REGION_SYNC_UP)
// - Sends back ACK (REGION_SYNC_ACK)

#include <boost/asio.hpp>
#include <thread>
#include <vector>
#include <iostream>
#include "central_tls_server.hpp"
#include "db.hpp" 

int main(int argc, char** argv){
  if(argc < 4){
    std::cerr<<"Usage: central_server <central_tls_port> <cert.pem> <key.pem>\n";
    return 1;
  }

  uint16_t port = (uint16_t)std::stoi(argv[1]);
  std::string cert = argv[2];
  std::string key  = argv[3];

  boost::asio::io_context io;
  sls::CentralStore store;
  
  sls::DbWriter db; 
  db.start("sls.db", "schema.sql"); 
  sls::CentralTlsServer srv(io, port, cert, key, store, db); 


  
  unsigned n = std::max(2u, std::thread::hardware_concurrency());
  std::vector<std::thread> threads;
  threads.reserve(n);
  for(unsigned i=0;i<n;i++){
    threads.emplace_back([&](){ io.run(); });
  }
  for(auto& t: threads) t.join();
  return 0;
}
