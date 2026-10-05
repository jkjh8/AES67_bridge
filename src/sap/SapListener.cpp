#include "sap/SapListener.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include "common/Log.h"

namespace aes67 {
namespace {
constexpr char kSapMcast[] = "239.255.255.255";
constexpr uint16_t kSapPort = 9875;
constexpr uint64_t kExpiryMs = 90000;

std::string LineValue(const std::string& sdp, const char* prefix) {
  std::istringstream ss(sdp);
  std::string line;
  const size_t plen = strlen(prefix);
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.compare(0, plen, prefix) == 0) return line.substr(plen);
  }
  return "";
}

bool ParseSdp(const std::string& sdp, SapSource* out) {
  out->name = LineValue(sdp, "s=");
  std::string c = LineValue(sdp, "c=IN IP4 ");
  if (!c.empty()) {
    size_t slash = c.find('/');
    out->address = (slash == std::string::npos) ? c : c.substr(0, slash);
  }
  std::string m = LineValue(sdp, "m=audio ");
  if (!m.empty()) {
    std::istringstream ms(m);
    std::string proto;
    ms >> out->port >> proto >> out->payload_type;
  }
  std::string rtpmap = LineValue(sdp, "a=rtpmap:");
  if (!rtpmap.empty()) {
    size_t slash2 = rtpmap.rfind('/');
    if (slash2 != std::string::npos) out->channels = atoi(rtpmap.c_str() + slash2 + 1);
  }
  return !out->address.empty() && out->port > 0;
}
}

struct SapListener::Impl {
  SOCKET sock = INVALID_SOCKET;
  std::thread thread;
  std::atomic<bool> running{false};
  mutable std::mutex mtx;
  std::map<std::string, SapSource> sources;

  void RecvLoop();
};

void SapListener::Impl::RecvLoop() {
  std::vector<char> buf(4096);
  int diag = 0;
  while (running.load()) {
    sockaddr_in from{};
    int fromlen = sizeof(from);
    int n = recvfrom(sock, buf.data(), (int)buf.size() - 1, 0, (sockaddr*)&from,
                     &fromlen);
    if (diag < 2) {
      LOGI("sap-listen: recvfrom n=%d err=%d", n, n < 0 ? WSAGetLastError() : 0);
      ++diag;
    }
    if (n <= 0) continue;
    buf[n] = 0;
    std::string pkt(buf.data(), n);
    if (!pkt.empty() && (pkt[0] & 0x04)) continue;
    size_t v = pkt.find("v=0");
    if (v == std::string::npos) continue;
    SapSource s;
    if (!ParseSdp(pkt.substr(v), &s)) continue;
    s.last_seen_ms = GetTickCount64();
    char org[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &from.sin_addr, org, sizeof(org));
    s.origin = org;
    const std::string key = s.origin + " " + s.address + ":" + std::to_string(s.port);
    std::lock_guard<std::mutex> lk(mtx);
    const bool isNew = sources.find(key) == sources.end();
    sources[key] = s;
    if (isNew)
      LOGI("sap-listen: source '%s' %s:%d %dch PT%d (total=%zu)", s.name.c_str(),
           s.address.c_str(), s.port, s.channels, s.payload_type, sources.size());
  }
}

SapListener::SapListener() : impl_(std::make_unique<Impl>()) {}
SapListener::~SapListener() { Stop(); }

bool SapListener::Start(uint32_t ifaceIpHost, std::string* err) {
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  impl_->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (impl_->sock == INVALID_SOCKET) {
    if (err) *err = "SAP listen socket failed";
    return false;
  }
  BOOL reuse = TRUE;
  setsockopt(impl_->sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(kSapPort);
  if (bind(impl_->sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
    if (err) *err = "SAP bind failed";
    closesocket(impl_->sock);
    impl_->sock = INVALID_SOCKET;
    return false;
  }
  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = inet_addr(kSapMcast);
  mreq.imr_interface.s_addr = htonl(ifaceIpHost);
  if (setsockopt(impl_->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq,
                 sizeof(mreq)) == SOCKET_ERROR) {
    if (err) *err = "SAP join failed";
    closesocket(impl_->sock);
    impl_->sock = INVALID_SOCKET;
    return false;
  }
  impl_->running = true;
  impl_->thread = std::thread([this] { impl_->RecvLoop(); });
  LOGI("sap: listening for sources on %s:%d", kSapMcast, kSapPort);
  return true;
}

void SapListener::Stop() {
  if (!impl_->running.exchange(false)) return;
  if (impl_->sock != INVALID_SOCKET) {
    closesocket(impl_->sock);
    impl_->sock = INVALID_SOCKET;
  }
  if (impl_->thread.joinable()) impl_->thread.join();
}

std::vector<SapSource> SapListener::Sources() const {
  const uint64_t now = GetTickCount64();
  std::vector<SapSource> out;
  std::lock_guard<std::mutex> lk(impl_->mtx);
  for (auto it = impl_->sources.begin(); it != impl_->sources.end();) {
    if (now - it->second.last_seen_ms > kExpiryMs) {
      it = impl_->sources.erase(it);
    } else {
      out.push_back(it->second);
      ++it;
    }
  }
  return out;
}

}
