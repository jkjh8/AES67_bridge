#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aes67 {

inline constexpr int kSampleRate = 48000;
inline constexpr int kBytesPerSample = 3;
inline constexpr double kPtimeMs = 1.0;
inline constexpr int kSamplesPerPacket = 48;
inline constexpr int kMaxStreamChannels = 8;
inline constexpr int kAsioChannels = 32;

struct NodeConfig {
  std::string name = "AES67-Bridge";
};

struct NetworkConfig {
  std::string interface_id;
  std::string interface_ip;
  int ip_ttl = 15;
};

struct PtpConfig {
  int domain = 0;
  int dscp = 46;
};

struct TxConfig {
  int id = 0;
  bool enabled = true;
  std::string name = "AES67-Bridge TX";
  int channels = 2;
  std::string address = "239.69.0.1";
  int rtp_port = 5004;
  int payload_type = 98;
  int ttl = 15;
  int dscp = 34;
};

struct RxConfig {
  int id = 0;
  bool enabled = true;
  std::string name = "AES67 RX";
  int channels = 2;
  std::string address = "239.69.0.18";
  int rtp_port = 5004;
  int payload_type = 98;
  int delay_ms = 4;
};

struct AudioDeviceConfig {
  bool input_enabled = true;
  std::string input_device;
  bool input_loopback = true;
  bool output_enabled = true;
  std::string output_device;
};

struct AsioConfig {
  int preferred_buffer = 256;
};

struct Route {
  std::string src;
  int src_ch = 0;
  std::string dst;
  int dst_ch = 0;
  bool operator==(const Route&) const = default;
};

struct SapConfig {
  bool enabled = true;
  int interval_s = 30;
};

struct AppConfig {
  int version = 2;
  NodeConfig node;
  NetworkConfig network;
  PtpConfig ptp;
  std::vector<TxConfig> tx;
  std::vector<RxConfig> rx;
  AudioDeviceConfig audio;
  AsioConfig asio;
  std::vector<Route> routes;
  SapConfig sap;
  bool autostart = false;
  std::string log_severity = "info";
};

}
