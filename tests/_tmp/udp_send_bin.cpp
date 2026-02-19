#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <string>
#include "proto.hpp"

int main(int argc, char** argv){
  if(argc < 6){
    std::cerr << "Usage: udp_send_bin <host> <port> <uri> <zone> <motion>\n";
    std::cerr << "  motion: 0/1 normal, 254 = fault marker\n";
    return 2;
  }
  std::string host = argv[1];
  int port = std::stoi(argv[2]);
  std::string uri = argv[3];
  uint32_t zone = (uint32_t)std::stoul(argv[4]);
  int motion = std::stoi(argv[5]);

  sls::TelemetryUdp t{};
  std::memset(&t, 0, sizeof(t));
  std::snprintf(t.uri, sizeof(t.uri), "%s", uri.c_str());
  t.zone_id_be = htonl(zone);
  t.lux_be = htons(180);
  // temp 23.5C -> 235 (x10)
  uint16_t temp10 = (uint16_t)235;
  t.temp_c_x10_be = htons(temp10);
  t.motion = (uint8_t)motion;

  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if(fd < 0){ perror("socket"); return 1; }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = inet_addr(host.c_str());

  ssize_t n = ::sendto(fd, &t, sizeof(t), 0, (sockaddr*)&addr, sizeof(addr));
  if(n < 0){ perror("sendto"); close(fd); return 1; }

  std::cout << "sent TelemetryUdp bytes=" << n << " uri=" << uri
            << " zone=" << zone << " motion=" << motion << "\n";
  close(fd);
  return 0;
}
