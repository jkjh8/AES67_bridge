#include "app/Controller.h"

#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>

#include <json.hpp>

#include "asio/AsioShared.h"
#include "common/Config.h"
#include "common/Log.h"
#include "media/AudioEngine.h"
#include "ptp/PtpClient.h"
#include "sap/SapListener.h"
#include "wasapi/DeviceEnum.h"

using nlohmann::json;

namespace aes67 {
namespace {
constexpr wchar_t kAsioRegKey[] = L"SOFTWARE\\ASIO\\AES67 Bridge";
constexpr wchar_t kAsioDll[] = L"AES67BridgeASIO.dll";

const char* PtpText(PtpLock l) {
  switch (l) {
    case PtpLock::Locked: return "locked";
    case PtpLock::Locking: return "locking";
    default: return "unlocked";
  }
}

std::string IpText(uint32_t ip) {
  char b[24];
  snprintf(b, sizeof(b), "%u.%u.%u.%u", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
           (ip >> 8) & 0xff, ip & 0xff);
  return b;
}

std::wstring ExeDir() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring p(path);
  return p.substr(0, p.find_last_of(L"\\/") + 1);
}

bool AsioRegistered() {
  HKEY k;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kAsioRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS)
    return false;
  RegCloseKey(k);
  return true;
}

bool RunRegsvr32(bool unregister, std::string* err) {
  const std::wstring dll = ExeDir() + kAsioDll;
  if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
    if (err) *err = "AES67BridgeASIO.dll not found next to the exe";
    return false;
  }
  std::wstring args = unregister ? L"/s /u \"" : L"/s \"";
  args += dll + L"\"";
  SHELLEXECUTEINFOW sei{sizeof(sei)};
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = L"runas";
  sei.lpFile = L"regsvr32.exe";
  sei.lpParameters = args.c_str();
  sei.nShow = SW_HIDE;
  if (!ShellExecuteExW(&sei)) {
    if (err) *err = "Administrator permission was denied";
    return false;
  }
  if (sei.hProcess) {
    WaitForSingleObject(sei.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code != 0) {
      if (err) *err = "regsvr32 failed (code " + std::to_string(code) + ")";
      return false;
    }
  }
  return true;
}

json TxToJson(const TxConfig& t) {
  return {{"id", t.id},           {"enabled", t.enabled},   {"name", t.name},
          {"channels", t.channels}, {"address", t.address}, {"port", t.rtp_port},
          {"pt", t.payload_type}, {"ttl", t.ttl}};
}
json RxToJson(const RxConfig& r) {
  return {{"id", r.id},           {"enabled", r.enabled},   {"name", r.name},
          {"channels", r.channels}, {"address", r.address}, {"port", r.rtp_port},
          {"pt", r.payload_type}};
}

std::string Dump(const json& j) {
  return j.dump(-1, ' ', false, json::error_handler_t::replace);
}
}

Controller::Controller(std::filesystem::path config_path, AppConfig cfg)
    : config_path_(std::move(config_path)), cfg_(std::move(cfg)) {
  cfg_.asio.preferred_buffer =
      (int)aes67asio::NormalizeBuffer((uint32_t)std::max(cfg_.asio.preferred_buffer, 1));
}

Controller::~Controller() { Stop(); }

bool Controller::Start() {
  net_ = ResolveNetIf(cfg_.network.interface_id, cfg_.network.interface_ip, &net_auto_);
  LOGI("net: %s %s (%s)%s", net_.name.c_str(), net_.ip.c_str(), net_.mac_text.c_str(),
       net_auto_ ? " [auto]" : "");
  ptp_ = std::make_unique<PtpClient>();
  engine_ = std::make_unique<AudioEngine>();
  engine_->Init(ptp_.get(), (uint8_t)cfg_.ptp.domain, cfg_.sap, cfg_.asio, net_.ip_host);

  AudioEngine* eng = engine_.get();
  std::string err;
  if (!ptp_->Start(htonl(net_.ip_host), net_.has_mac ? net_.mac : nullptr,
                   (uint8_t)cfg_.ptp.domain, [eng] { eng->OnTic(); }, &err))
    LOGW("ptp: start failed: %s", err.c_str());

  engine_->ApplyAudio(cfg_.audio);
  for (const auto& t : cfg_.tx) engine_->ApplyTx(t, nullptr);
  engine_->SetRxDelay(cfg_.rx_delay_ms);
  for (const auto& r : cfg_.rx) engine_->ApplyRx(r, nullptr);
  engine_->SetRoutes(cfg_.routes);

  sap_listener_ = std::make_unique<SapListener>();
  if (!sap_listener_->Start(net_.ip_host, &err))
    LOGW("sap listener start failed: %s", err.c_str());
  return true;
}

void Controller::Stop() {
  if (sap_listener_) {
    sap_listener_->Stop();
    sap_listener_.reset();
  }
  if (ptp_) ptp_->Stop();
  if (engine_) {
    engine_->Shutdown();
    engine_.reset();
  }
  ptp_.reset();
}

void Controller::Tick() {
  if (!ptp_) return;
  PtpInfo info = ptp_->GetInfo();
  if ((int)info.lock != last_ptp_lock_) {
    last_ptp_lock_ = (int)info.lock;
    LOGI("ptp: lock -> %s gmid=%s", PtpText(info.lock),
         info.gmid.empty() ? "(none)" : info.gmid.c_str());
    if (info.lock == PtpLock::Locked && engine_) engine_->KickSap();
  }
  if (engine_ && ++tick_count_ % 10 == 0) {
    const auto st = engine_->GetStatus(true);
    LOGI("stat: ptp %s", ptp_->Diag().c_str());
    LOGI("stat: tic n=%llu burst>=2:%llu burst>=3:%llu max-lag=%.2fms", (unsigned long long)st.tics,
         (unsigned long long)st.tic_burst2, (unsigned long long)st.tic_burst3,
         st.max_lag / 48.0);
    if (st.in.running)
      LOGI("stat: wasapi-in asrc=%+.1f ppm fifo=%.1f ms rate=%d underruns=%llu",
           st.in.asrc_ppm, st.in.fifo_ms, st.in.rate, (unsigned long long)st.in.underruns);
    if (st.out.running)
      LOGI("stat: wasapi-out asrc=%+.1f ppm fifo=%.1f ms rate=%d", st.out.asrc_ppm,
           st.out.fifo_ms, st.out.rate);
    for (const auto& t : st.tx)
      LOGI("stat: tx#%d running=%d pkts=%llu send-errors=%llu", t.id, t.running,
           (unsigned long long)t.packets, (unsigned long long)t.send_errors);
    for (const auto& r : st.rx)
      LOGI("stat: rx#%d running=%d receiving=%d pkts=%llu other-group=%llu pt-mismatch=%llu %s",
           r.id, r.running, r.receiving, (unsigned long long)r.packets,
           (unsigned long long)r.filtered, (unsigned long long)r.pt_mismatch,
           r.diag.c_str());
  }
}

void Controller::Persist() {
  if (!SaveConfig(config_path_, cfg_)) LOGE("controller: failed to persist config");
}

void Controller::PruneRoutes() {
  auto src_ch = [&](const std::string& p) -> int {
    if (p == "wasapi" || p == "asio") return kAsioChannels;
    for (const auto& r : cfg_.rx)
      if (p == "rx" + std::to_string(r.id)) return r.channels;
    return 0;
  };
  auto dst_ch = [&](const std::string& p) -> int {
    if (p == "wasapi" || p == "asio") return kAsioChannels;
    for (const auto& t : cfg_.tx)
      if (p == "tx" + std::to_string(t.id)) return t.channels;
    return 0;
  };
  auto& R = cfg_.routes;
  R.erase(std::remove_if(R.begin(), R.end(),
                         [&](const Route& r) {
                           return r.src_ch >= src_ch(r.src) || r.dst_ch >= dst_ch(r.dst);
                         }),
          R.end());
}

std::string Controller::HandleCommand(const std::string& text, bool* send_devices) {
  json reply{{"type", "result"}, {"ok", true}};
  auto fail = [&](const std::string& e) {
    reply["ok"] = false;
    reply["error"] = e;
  };
  try {
    const json m = json::parse(text);
    reply["rid"] = m.value("rid", 0);
    const std::string cmd = m.value("cmd", "");

    if (cmd == "log") {
      LOGW("webui js: %s", m.value("text", "").c_str());
      return "";

    } else if (cmd == "hello" || cmd == "refresh_devices") {
      if (cmd == "hello") LOGI("webui: page connected");
      if (send_devices) *send_devices = true;

    } else if (cmd == "tx_save") {
      const json& s = m.at("stream");
      TxConfig t;
      t.id = s.value("id", 0);
      if (t.id != 0) {
        auto it = std::find_if(cfg_.tx.begin(), cfg_.tx.end(),
                               [&](const TxConfig& x) { return x.id == t.id; });
        if (it == cfg_.tx.end()) throw std::runtime_error("unknown TX stream");
        t = *it;
      }
      t.name = s.value("name", t.name);
      t.address = s.value("address", t.address);
      t.rtp_port = s.value("port", t.rtp_port);
      t.channels = s.value("channels", t.channels);
      t.payload_type = s.value("pt", t.payload_type);
      t.ttl = s.value("ttl", t.ttl);
      t.enabled = s.value("enabled", t.enabled);
      if (auto e = ValidateTx(t); !e.empty()) throw std::runtime_error(e);
      for (const auto& o : cfg_.tx)
        if (o.id != t.id && o.address == t.address && o.rtp_port == t.rtp_port)
          throw std::runtime_error("Address/port already used by '" + o.name + "'");
      if (t.id == 0) {
        int mx = 0;
        for (const auto& o : cfg_.tx) mx = std::max(mx, o.id);
        t.id = mx + 1;
        cfg_.tx.push_back(t);
      } else {
        for (auto& o : cfg_.tx)
          if (o.id == t.id) o = t;
      }
      PruneRoutes();
      std::string err;
      if (!engine_->ApplyTx(t, &err)) fail("Saved, but failed to start: " + err);
      engine_->SetRoutes(cfg_.routes);
      reply["id"] = t.id;
      LOGI("ui: TX save #%d '%s' %s:%d %dch", t.id, t.name.c_str(),
           t.address.c_str(), t.rtp_port, t.channels);
      Persist();

    } else if (cmd == "tx_enable") {
      const int id = m.value("id", 0);
      for (auto& t : cfg_.tx)
        if (t.id == id) {
          t.enabled = m.value("enabled", true);
          std::string err;
          if (!engine_->ApplyTx(t, &err)) fail(err);
          engine_->SetRoutes(cfg_.routes);
        }
      Persist();

    } else if (cmd == "tx_delete") {
      const int id = m.value("id", 0);
      cfg_.tx.erase(std::remove_if(cfg_.tx.begin(), cfg_.tx.end(),
                                   [&](const TxConfig& t) { return t.id == id; }),
                    cfg_.tx.end());
      engine_->RemoveTx(id);
      PruneRoutes();
      engine_->SetRoutes(cfg_.routes);
      Persist();

    } else if (cmd == "rx_save") {
      const json& s = m.at("stream");
      RxConfig r;
      r.id = s.value("id", 0);
      if (r.id != 0) {
        auto it = std::find_if(cfg_.rx.begin(), cfg_.rx.end(),
                               [&](const RxConfig& x) { return x.id == r.id; });
        if (it == cfg_.rx.end()) throw std::runtime_error("unknown RX stream");
        r = *it;
      }
      r.name = s.value("name", r.name);
      r.address = s.value("address", r.address);
      r.rtp_port = s.value("port", r.rtp_port);
      r.channels = s.value("channels", r.channels);
      r.payload_type = s.value("pt", r.payload_type);
      r.enabled = s.value("enabled", r.enabled);
      if (auto e = ValidateRx(r); !e.empty()) throw std::runtime_error(e);
      for (const auto& o : cfg_.rx)
        if (o.id != r.id && o.address == r.address && o.rtp_port == r.rtp_port)
          throw std::runtime_error("Already receiving this stream ('" + o.name + "')");
      if (r.id == 0) {
        int mx = 0;
        for (const auto& o : cfg_.rx) mx = std::max(mx, o.id);
        r.id = mx + 1;
        cfg_.rx.push_back(r);
      } else {
        for (auto& o : cfg_.rx)
          if (o.id == r.id) o = r;
      }
      PruneRoutes();
      std::string err;
      if (!engine_->ApplyRx(r, &err)) fail("Saved, but failed to start: " + err);
      engine_->SetRoutes(cfg_.routes);
      reply["id"] = r.id;
      LOGI("ui: RX save #%d '%s' %s:%d %dch PT%d", r.id, r.name.c_str(),
           r.address.c_str(), r.rtp_port, r.channels, r.payload_type);
      Persist();

    } else if (cmd == "rx_enable") {
      const int id = m.value("id", 0);
      for (auto& r : cfg_.rx)
        if (r.id == id) {
          r.enabled = m.value("enabled", true);
          std::string err;
          if (!engine_->ApplyRx(r, &err)) fail(err);
          engine_->SetRoutes(cfg_.routes);
        }
      Persist();

    } else if (cmd == "rx_delete") {
      const int id = m.value("id", 0);
      cfg_.rx.erase(std::remove_if(cfg_.rx.begin(), cfg_.rx.end(),
                                   [&](const RxConfig& r) { return r.id == id; }),
                    cfg_.rx.end());
      engine_->RemoveRx(id);
      PruneRoutes();
      engine_->SetRoutes(cfg_.routes);
      Persist();

    } else if (cmd == "route_set") {
      Route r{m.value("src", ""), m.value("sc", 0), m.value("dst", ""), m.value("dc", 0)};
      const bool on = m.value("on", true);
      auto& R = cfg_.routes;
      auto it = std::find(R.begin(), R.end(), r);
      if (on && it == R.end()) R.push_back(r);
      if (!on && it != R.end()) R.erase(it);
      LOGI("ui: route %s %s.%d -> %s.%d (total %zu)", on ? "ON " : "OFF", r.src.c_str(),
           r.src_ch + 1, r.dst.c_str(), r.dst_ch + 1, R.size());
      engine_->SetRoutes(R);
      Persist();

    } else if (cmd == "routes_clear") {
      const std::string dst = m.value("dst", "");
      auto& R = cfg_.routes;
      R.erase(std::remove_if(R.begin(), R.end(),
                             [&](const Route& r) { return dst.empty() || r.dst == dst; }),
              R.end());
      engine_->SetRoutes(R);
      Persist();

    } else if (cmd == "audio_save") {
      AudioDeviceConfig& a = cfg_.audio;
      a.input_enabled = m.value("input_enabled", a.input_enabled);
      a.input_device = m.value("input_device", a.input_device);
      a.input_loopback = m.value("input_loopback", a.input_loopback);
      a.output_enabled = m.value("output_enabled", a.output_enabled);
      a.output_device = m.value("output_device", a.output_device);
      LOGI("ui: audio in=%d '%s'%s out=%d '%s'", a.input_enabled,
           a.input_device.c_str(), a.input_loopback ? " (loopback)" : "",
           a.output_enabled, a.output_device.c_str());
      engine_->ApplyAudio(a);
      engine_->SetRoutes(cfg_.routes);
      Persist();

    } else if (cmd == "network_save") {
      const std::string id = m.value("interface_id", "");
      std::string ip;
      for (const auto& n : EnumNetIfs())
        if (n.id == id) ip = n.ip;
      if (!id.empty() && ip.empty()) throw std::runtime_error("Network interface not found");
      cfg_.network.interface_id = id;
      cfg_.network.interface_ip = ip;
      Persist();
      LOGI("ui: network interface -> %s", id.empty() ? "auto" : ip.c_str());
      Stop();
      Start();

    } else if (cmd == "rx_delay_save") {
      const int d = m.value("delay_ms", cfg_.rx_delay_ms);
      if (d < kMinRxDelayMs || d > kMaxRxDelayMs) throw std::runtime_error("delay must be 4..10 ms");
      cfg_.rx_delay_ms = d;
      engine_->SetRxDelay(d);
      for (const auto& r : cfg_.rx) engine_->ApplyRx(r, nullptr);
      engine_->SetRoutes(cfg_.routes);
      LOGI("ui: RX delay -> %d ms", d);
      Persist();

    } else if (cmd == "asio_save") {
      const int b = m.value("preferred_buffer", cfg_.asio.preferred_buffer);
      cfg_.asio.preferred_buffer = (int)aes67asio::NormalizeBuffer((uint32_t)std::max(b, 1));
      engine_->ApplyAsio(cfg_.asio);
      Persist();

    } else if (cmd == "asio_register" || cmd == "asio_unregister") {
      std::string err;
      if (!RunRegsvr32(cmd == "asio_unregister", &err)) fail(err);

    } else {
      fail("unknown command: " + cmd);
    }
  } catch (const std::exception& e) {
    fail(e.what());
  }
  return Dump(reply);
}

std::string Controller::StateJson() const {
  json j{{"type", "state"}};
  const AudioEngine::Status st = engine_ ? engine_->GetStatus() : AudioEngine::Status{};

  j["node"] = {{"name", cfg_.node.name}, {"ip", IpText(st.local_ip)}};
  j["network"] = {{"interface_id", cfg_.network.interface_id},
                  {"auto", net_auto_},
                  {"name", net_.name},
                  {"ip", net_.ip},
                  {"mac", net_.mac_text},
                  {"speed", net_.speed_bps},
                  {"clock_id", ptp_ ? ptp_->ClockId() : ""}};
  if (ptp_) {
    PtpInfo info = ptp_->GetInfo();
    j["ptp"] = {{"lock", PtpText(info.lock)}, {"gmid", info.gmid},
                {"domain", cfg_.ptp.domain}};
  }

  json tx = json::array();
  for (const auto& t : cfg_.tx) {
    json o = TxToJson(t);
    for (const auto& s : st.tx)
      if (s.id == t.id) {
        o["running"] = s.running;
        o["packets"] = s.packets;
        o["error"] = s.error;
        o["sdp"] = s.sdp;
      }
    tx.push_back(o);
  }
  j["tx"] = tx;

  json rx = json::array();
  for (const auto& r : cfg_.rx) {
    json o = RxToJson(r);
    for (const auto& s : st.rx)
      if (s.id == r.id) {
        o["running"] = s.running;
        o["receiving"] = s.receiving;
        o["packets"] = s.packets;
        o["error"] = s.error;
      }
    rx.push_back(o);
  }
  j["rx"] = rx;

  json sap = json::array();
  if (sap_listener_)
    for (const auto& s : sap_listener_->Sources()) {
      bool local = false;
      for (const auto& t : cfg_.tx)
        if (t.address == s.address && t.rtp_port == s.port) local = true;
      sap.push_back({{"name", s.name}, {"address", s.address}, {"port", s.port},
                     {"channels", s.channels}, {"pt", s.payload_type},
                     {"local", local}});
    }
  j["sap"] = sap;

  auto dev = [](const AudioEngine::DevStatus& d) {
    return json{{"running", d.running}, {"channels", d.channels}, {"rate", d.rate},
                {"name", d.name},       {"error", d.error}};
  };
  const AudioDeviceConfig& a = cfg_.audio;
  j["audio"] = {{"input_enabled", a.input_enabled},
                {"input_device", a.input_device},
                {"input_loopback", a.input_loopback},
                {"output_enabled", a.output_enabled},
                {"output_device", a.output_device},
                {"input", dev(st.in)},
                {"output", dev(st.out)}};

  j["rx_delay_ms"] = cfg_.rx_delay_ms;
  j["asio"] = {{"registered", AsioRegistered()},
               {"preferred", cfg_.asio.preferred_buffer}};

  json src = json::array(), dst = json::array();
  src.push_back({{"id", "wasapi"}, {"group", "wasapi"}, {"name", "WASAPI In"},
                 {"channels", st.in.running ? st.in.channels : 2},
                 {"active", st.in.running}});
  src.push_back({{"id", "asio"}, {"group", "asio"}, {"name", "ASIO Out (from DAW)"},
                 {"channels", kAsioChannels}, {"active", true}});
  for (const auto& r : cfg_.rx) {
    bool on = false;
    for (const auto& s : st.rx)
      if (s.id == r.id) on = s.running;
    src.push_back({{"id", "rx" + std::to_string(r.id)}, {"group", "aes"},
                   {"name", r.name}, {"channels", r.channels}, {"active", on}});
  }
  for (const auto& t : cfg_.tx) {
    bool on = false;
    for (const auto& s : st.tx)
      if (s.id == t.id) on = s.running;
    dst.push_back({{"id", "tx" + std::to_string(t.id)}, {"group", "aes"},
                   {"name", t.name}, {"channels", t.channels}, {"active", on}});
  }
  dst.push_back({{"id", "wasapi"}, {"group", "wasapi"}, {"name", "WASAPI Out"},
                 {"channels", st.out.running ? st.out.channels : 2},
                 {"active", st.out.running}});
  dst.push_back({{"id", "asio"}, {"group", "asio"}, {"name", "ASIO In (to DAW)"},
                 {"channels", kAsioChannels}, {"active", true}});
  j["ports"] = {{"src", src}, {"dst", dst}};

  json routes = json::array();
  for (const auto& r : cfg_.routes)
    routes.push_back({{"src", r.src}, {"sc", r.src_ch}, {"dst", r.dst}, {"dc", r.dst_ch}});
  j["routes"] = routes;
  return Dump(j);
}

std::string Controller::DevicesJson() const {
  auto list = [](bool render) {
    json a = json::array();
    for (const auto& d : EnumAudioDevices(render))
      a.push_back({{"id", d.id}, {"name", d.name}, {"default", d.is_default},
                   {"channels", d.channels}, {"rate", d.rate}});
    return a;
  };
  json nics = json::array();
  for (const auto& n : EnumNetIfs())
    nics.push_back({{"id", n.id}, {"name", n.name}, {"description", n.description},
                    {"ip", n.ip}, {"mac", n.mac_text}, {"speed", n.speed_bps},
                    {"wireless", n.wireless}});
  return Dump(json{{"type", "devices"}, {"render", list(true)}, {"capture", list(false)},
                   {"nics", nics}});
}

std::wstring Controller::Tooltip() const {
  std::wstring tip = L"AES67 Bridge";
  if (ptp_) {
    PtpInfo info = ptp_->GetInfo();
    tip += L"\nPTP: ";
    const char* t = PtpText(info.lock);
    tip.append(t, t + strlen(t));
  }
  if (engine_) {
    const auto st = engine_->GetStatus();
    int txr = 0, rxr = 0;
    for (const auto& t : st.tx) txr += t.running;
    for (const auto& r : st.rx) rxr += r.receiving;
    tip += L"\nTX " + std::to_wstring(txr) + L" / RX " + std::to_wstring(rxr);
    if (st.asio.client_active) tip += L" / ASIO";
  }
  return tip;
}

}
