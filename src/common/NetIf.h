#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aes67 {

struct NetIf {
  std::string id;
  std::string name;
  std::string description;
  std::string ip;
  uint32_t ip_host = 0;
  uint8_t mac[6] = {};
  bool has_mac = false;
  std::string mac_text;
  uint64_t speed_bps = 0;
  bool wireless = false;
};

std::vector<NetIf> EnumNetIfs();

NetIf ResolveNetIf(const std::string& id, const std::string& ip, bool* is_auto);

}
