#pragma once
// ============================================================
// Framed TLS stream helper:
// TCP je byte-stream, zato radimo framing:
// [uint32_be length][uint8 type][payload...]
// length = 1 + payload_size (uključuje MsgType byte)
// ============================================================

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <cstdint>
#include <cstring>
#include <vector>
#include "proto.hpp"

namespace sls {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

template <typename TlsStream>
inline void async_write_frame(TlsStream& stream,
                              MsgType type,
                              const std::vector<uint8_t>& payload,
                              std::function<void(const boost::system::error_code&, std::size_t)> handler)
{
  auto frame = std::make_shared<std::vector<uint8_t>>();
  uint32_t len = static_cast<uint32_t>(1 + payload.size());
  uint32_t len_be = to_be32(len);
  frame->resize(4);
  std::memcpy(frame->data(), &len_be, 4);
  frame->push_back(static_cast<uint8_t>(type));
  frame->insert(frame->end(), payload.begin(), payload.end());

  asio::async_write(stream, asio::buffer(*frame),
    [frame, handler](const boost::system::error_code& ec, std::size_t n){
      handler(ec, n);
    });
}

template <typename TlsStream>
inline void write_frame(TlsStream& stream, MsgType type, const std::vector<uint8_t>& payload)
{
  std::vector<uint8_t> frame;
  uint32_t len = static_cast<uint32_t>(1 + payload.size());
  uint32_t len_be = to_be32(len);
  frame.resize(4);
  std::memcpy(frame.data(), &len_be, 4);
  frame.push_back(static_cast<uint8_t>(type));
  frame.insert(frame.end(), payload.begin(), payload.end());
  asio::write(stream, asio::buffer(frame));
}

template <typename TlsStream>
inline std::vector<uint8_t> read_frame(TlsStream& stream)
{
  uint32_t len_be{};
  asio::read(stream, asio::buffer(&len_be, 4));
  uint32_t len = from_be32(len_be);
  std::vector<uint8_t> body(len);
  asio::read(stream, asio::buffer(body));
  return body;
}

} // namespace sls
