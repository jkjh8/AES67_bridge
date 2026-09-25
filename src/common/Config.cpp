#include "common/Config.h"

#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#include <json.hpp>

#include "common/Log.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace aes67 {

namespace {

NodeConfig NodeFrom(const json& j) {
  NodeConfig c;
  c.name = j.value("name", c.name);
  return c;
}
NetworkConfig NetworkFrom(const json& j) {
  NetworkConfig c;
  c.interface_id = j.value("interface_id", c.interface_id);
  c.interface_ip = j.value("interface_ip", c.interface_ip);
  c.ip_ttl = j.value("ip_ttl", c.ip_ttl);
  return c;
}
PtpConfig PtpFrom(const json& j) {
  PtpConfig c;
  c.domain = j.value("domain", c.domain);
  c.dscp = j.value("dscp", c.dscp);
  return c;
}
TxConfig TxFrom(const json& j) {
  TxConfig c;
  c.id = j.value("id", c.id);
  c.enabled = j.value("enabled", c.enabled);
  c.name = j.value("name", c.name);
  c.channels = j.value("channels", c.channels);
  c.address = j.value("address", c.address);
  c.rtp_port = j.value("rtp_port", c.rtp_port);
  c.payload_type = j.value("payload_type", c.payload_type);
  c.ttl = j.value("ttl", c.ttl);
  c.dscp = j.value("dscp", c.dscp);
  return c;
}
RxConfig RxFrom(const json& j) {
  RxConfig c;
  c.id = j.value("id", c.id);
  c.enabled = j.value("enabled", c.enabled);
  c.name = j.value("name", c.name);
  c.channels = j.value("channels", c.channels);
  c.address = j.value("address", c.address);
  c.rtp_port = j.value("rtp_port", c.rtp_port);
  c.payload_type = j.value("payload_type", c.payload_type);
  return c;
}
AudioDeviceConfig AudioFrom(const json& j) {
  AudioDeviceConfig c;
  c.input_enabled = j.value("input_enabled", c.input_enabled);
  c.input_device = j.value("input_device", c.input_device);
  c.input_loopback = j.value("input_loopback", c.input_loopback);
  c.output_enabled = j.value("output_enabled", c.output_enabled);
  c.output_device = j.value("output_device", c.output_device);
  return c;
}
SapConfig SapFrom(const json& j) {
  SapConfig c;
  c.enabled = j.value("enabled", c.enabled);
  c.interval_s = j.value("interval_s", c.interval_s);
  return c;
}

json TxJson(const TxConfig& t) {
  return json{{"id", t.id},           {"enabled", t.enabled},
              {"name", t.name},       {"channels", t.channels},
              {"address", t.address}, {"rtp_port", t.rtp_port},
              {"payload_type", t.payload_type},
              {"ttl", t.ttl},         {"dscp", t.dscp}};
}
json RxJson(const RxConfig& r) {
  return json{{"id", r.id},           {"enabled", r.enabled},
              {"name", r.name},       {"channels", r.channels},
              {"address", r.address}, {"rtp_port", r.rtp_port},
              {"payload_type", r.payload_type}};
}

json ToJson(const AppConfig& c) {
  json tx = json::array(), rx = json::array(), routes = json::array();
  for (const auto& t : c.tx) tx.push_back(TxJson(t));
  for (const auto& r : c.rx) rx.push_back(RxJson(r));
  for (const auto& r : c.routes)
    routes.push_back({{"src", r.src}, {"sc", r.src_ch}, {"dst", r.dst}, {"dc", r.dst_ch}});
  return json{
      {"version", c.version},
      {"node", {{"name", c.node.name}}},
      {"network",
       {{"interface_id", c.network.interface_id},
        {"interface_ip", c.network.interface_ip},
        {"ip_ttl", c.network.ip_ttl}}},
      {"ptp", {{"domain", c.ptp.domain}, {"dscp", c.ptp.dscp}}},
      {"tx_streams", tx},
      {"rx_streams", rx},
      {"audio",
       {{"input_enabled", c.audio.input_enabled},
        {"input_device", c.audio.input_device},
        {"input_loopback", c.audio.input_loopback},
        {"output_enabled", c.audio.output_enabled},
        {"output_device", c.audio.output_device}}},
      {"asio", {{"preferred_buffer", c.asio.preferred_buffer}}},
      {"routes", routes},
      {"sap", {{"enabled", c.sap.enabled}, {"interval_s", c.sap.interval_s}}},
      {"rx_delay_ms", c.rx_delay_ms},
      {"autostart", c.autostart},
      {"log_severity", c.log_severity},
  };
}

AppConfig FromJson(const json& j) {
  AppConfig c;
  const int ver = j.value("version", 1);
  if (j.contains("node")) c.node = NodeFrom(j["node"]);
  if (j.contains("network")) c.network = NetworkFrom(j["network"]);
  if (j.contains("ptp")) c.ptp = PtpFrom(j["ptp"]);
  if (j.contains("sap")) c.sap = SapFrom(j["sap"]);
  c.rx_delay_ms = std::clamp(j.value("rx_delay_ms", c.rx_delay_ms), kMinRxDelayMs, kMaxRxDelayMs);
  if (j.contains("audio")) c.audio = AudioFrom(j["audio"]);
  if (j.contains("asio"))
    c.asio.preferred_buffer = j["asio"].value("preferred_buffer", c.asio.preferred_buffer);
  c.autostart = j.value("autostart", c.autostart);
  c.log_severity = j.value("log_severity", c.log_severity);

  if (ver >= 2) {
    if (j.contains("tx_streams"))
      for (const auto& t : j["tx_streams"]) c.tx.push_back(TxFrom(t));
    if (j.contains("rx_streams"))
      for (const auto& r : j["rx_streams"]) c.rx.push_back(RxFrom(r));
    if (j.contains("routes"))
      for (const auto& r : j["routes"])
        c.routes.push_back({r.value("src", ""), r.value("sc", 0),
                            r.value("dst", ""), r.value("dc", 0)});
  } else {
    if (j.contains("tx")) {
      TxConfig t = TxFrom(j["tx"]);
      t.id = 1;
      c.tx.push_back(t);
      for (int ch = 0; ch < 2 && ch < t.channels; ++ch)
        c.routes.push_back({"wasapi", ch, "tx1", ch});
    }
    if (j.contains("rx")) {
      RxConfig r = RxFrom(j["rx"]);
      r.id = 1;
      c.rx.push_back(r);
      for (int ch = 0; ch < 2 && ch < r.channels; ++ch)
        c.routes.push_back({"rx1", ch, "wasapi", ch});
    }
    c.version = 2;
  }
  return c;
}

}

fs::path DefaultConfigPath() {
  const char* pd = std::getenv("ProgramData");
  fs::path base = pd ? fs::path(pd) : fs::path("C:/ProgramData");
  return base / "AES67Bridge" / "config.json";
}

AppConfig LoadConfig(const fs::path& path, bool* used_defaults) {
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    if (used_defaults) *used_defaults = true;
    return AppConfig{};
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    LOGW("config: cannot open %ls, using defaults", path.c_str());
    if (used_defaults) *used_defaults = true;
    return AppConfig{};
  }
  std::stringstream ss;
  ss << in.rdbuf();
  try {
    json j = json::parse(ss.str());
    if (used_defaults) *used_defaults = false;
    return FromJson(j);
  } catch (const std::exception& e) {
    LOGE("config: parse error (%s); using defaults", e.what());
    if (used_defaults) *used_defaults = true;
    return AppConfig{};
  }
}

bool SaveConfig(const fs::path& path, const AppConfig& cfg) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);

  const std::string text = ToJson(cfg).dump(2);
  fs::path tmp = path;
  tmp += L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      LOGE("config: cannot write temp %ls", tmp.c_str());
      return false;
    }
    out << text;
    if (!out) return false;
  }
  if (!MoveFileExW(tmp.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    LOGE("config: MoveFileEx failed (%lu)", GetLastError());
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

std::string ValidateTx(const TxConfig& t) {
  if (t.channels < 1 || t.channels > kMaxStreamChannels)
    return "channels must be 1..8";
  const unsigned long a = ntohl(inet_addr(t.address.c_str()));
  if (t.address.empty() || a == ntohl(INADDR_NONE) || (a >> 28) != 0xE)
    return "address must be an IPv4 multicast group (224.0.0.0 - 239.255.255.255)";
  if (t.rtp_port <= 0 || t.rtp_port > 65535) return "invalid RTP port";
  if (t.payload_type < 96 || t.payload_type > 127) return "payload type must be 96..127";
  if (t.ttl < 1 || t.ttl > 255) return "TTL must be 1..255";
  return "";
}

std::string ValidateRx(const RxConfig& r) {
  if (r.channels < 1 || r.channels > kMaxStreamChannels)
    return "channels must be 1..8";
  const unsigned long a = ntohl(inet_addr(r.address.c_str()));
  if (r.address.empty() || a == ntohl(INADDR_NONE) || (a >> 28) != 0xE)
    return "address must be an IPv4 multicast group";
  if (r.rtp_port <= 0 || r.rtp_port > 65535) return "invalid RTP port";
  return "";
}

std::string ValidateConfig(const AppConfig& cfg) {
  for (const auto& t : cfg.tx)
    if (auto e = ValidateTx(t); !e.empty()) return "tx '" + t.name + "': " + e;
  for (const auto& r : cfg.rx)
    if (auto e = ValidateRx(r); !e.empty()) return "rx '" + r.name + "': " + e;
  if (cfg.ptp.domain < 0 || cfg.ptp.domain > 127)
    return "ptp.domain out of range";
  return "";
}

}
