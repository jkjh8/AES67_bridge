#include "media/TxStream.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/Log.h"
#include "ptp/PtpClient.h"

namespace aes67 {
namespace {
constexpr uint32_t kFrameSize = 48;
constexpr size_t kRtpHeader = 12;

uint32_t LocalIpv4Host() {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return 0;
  sockaddr_in probe{};
  probe.sin_family = AF_INET;
  probe.sin_addr.s_addr = inet_addr("8.8.8.8");
  probe.sin_port = htons(53);
  uint32_t ip = 0;
  if (connect(s, (sockaddr*)&probe, sizeof(probe)) == 0) {
    sockaddr_in local{};
    int len = sizeof(local);
    if (getsockname(s, (sockaddr*)&local, &len) == 0) ip = ntohl(local.sin_addr.s_addr);
  }
  closesocket(s);
  return ip;
}

inline void PutL24(uint8_t* d, float v) {
  if (v > 1.0f) v = 1.0f;
  if (v < -1.0f) v = -1.0f;
  int32_t x = (int32_t)(v * 8388607.0f);
  d[0] = (uint8_t)(x >> 16);
  d[1] = (uint8_t)(x >> 8);
  d[2] = (uint8_t)x;
}
}

struct TxStream::Impl {
  PtpClient* ptp = nullptr;
  TxConfig cfg{};
  int channels = 2;
  uint32_t iface_ip_host = 0;
  uint8_t ptp_domain = 0;

  SOCKET sock = INVALID_SOCKET;
  sockaddr_in dst{};
  uint16_t seq = 0;
  uint32_t ssrc = 0;
  std::vector<uint8_t> pkt;

  std::atomic<bool> running{false};
  std::atomic<uint64_t> packets{0};
  std::atomic<uint64_t> send_errors{0};
  int last_error = 0;
  std::mutex send_mutex;
};

TxStream::TxStream() : impl_(std::make_unique<Impl>()) {}
TxStream::~TxStream() { Stop(); }

bool TxStream::Start(PtpClient* ptp, const TxConfig& cfg, uint32_t ifaceIpHost,
                     uint8_t ptpDomain, std::string* err) {
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  Impl* im = impl_.get();
  if (ifaceIpHost == 0) ifaceIpHost = LocalIpv4Host();
  im->ptp = ptp;
  im->cfg = cfg;
  im->iface_ip_host = ifaceIpHost;
  im->ptp_domain = ptpDomain;
  im->channels = (cfg.channels >= 1 && cfg.channels <= 8) ? cfg.channels : 2;
  im->packets = 0;
  im->ssrc = 0x11223344u + (uint32_t)(cfg.id > 0 ? cfg.id - 1 : 0);
  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  im->seq = (uint16_t)(qpc.QuadPart * 2654435761u >> 7);
  im->pkt.assign(kRtpHeader + (size_t)kFrameSize * im->channels * 3, 0);

  im->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (im->sock == INVALID_SOCKET) {
    if (err) *err = "tx socket() failed";
    return false;
  }
  in_addr ifa;
  ifa.s_addr = htonl(ifaceIpHost);
  setsockopt(im->sock, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ifa, sizeof(ifa));
  DWORD ttl = (DWORD)(cfg.ttl > 0 && cfg.ttl < 256 ? cfg.ttl : 15);
  setsockopt(im->sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
  BOOL loop = TRUE;
  setsockopt(im->sock, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));
  DWORD tos = (DWORD)(cfg.dscp & 0x3F) << 2;
  setsockopt(im->sock, IPPROTO_IP, IP_TOS, (const char*)&tos, sizeof(tos));
  sockaddr_in src{};
  src.sin_family = AF_INET;
  src.sin_addr.s_addr = htonl(ifaceIpHost);
  src.sin_port = htons((unsigned short)cfg.rtp_port);
  BOOL reuse = TRUE;
  setsockopt(im->sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
  if (bind(im->sock, (sockaddr*)&src, sizeof(src)) == SOCKET_ERROR) {
    src.sin_port = 0;
    bind(im->sock, (sockaddr*)&src, sizeof(src));
  }

  im->dst = {};
  im->dst.sin_family = AF_INET;
  im->dst.sin_addr.s_addr = inet_addr(cfg.address.c_str());
  im->dst.sin_port = htons((unsigned short)cfg.rtp_port);

  uint8_t* h = im->pkt.data();
  h[0] = 0x80;
  h[1] = (uint8_t)(cfg.payload_type & 0x7F);
  h[8] = (uint8_t)(im->ssrc >> 24);
  h[9] = (uint8_t)(im->ssrc >> 16);
  h[10] = (uint8_t)(im->ssrc >> 8);
  h[11] = (uint8_t)im->ssrc;

  im->running = true;
  LOGI("tx: started %s:%d L24/%dch pt=%d", cfg.address.c_str(), cfg.rtp_port,
       im->channels, cfg.payload_type);
  return true;
}

void TxStream::Stop() {
  if (!impl_->running.exchange(false)) return;
  std::lock_guard<std::mutex> lk(impl_->send_mutex);
  if (impl_->sock != INVALID_SOCKET) closesocket(impl_->sock);
  impl_->sock = INVALID_SOCKET;
  LOGI("tx: stopped");
}

void TxStream::SendBlock(uint64_t block_sac, const float* planar) {
  Impl* im = impl_.get();
  if (!im->running.load()) return;
  std::lock_guard<std::mutex> lk(im->send_mutex);
  uint8_t* h = im->pkt.data();
  const uint16_t seq = im->seq++;
  const uint32_t ts = (uint32_t)block_sac;
  h[2] = (uint8_t)(seq >> 8);
  h[3] = (uint8_t)seq;
  h[4] = (uint8_t)(ts >> 24);
  h[5] = (uint8_t)(ts >> 16);
  h[6] = (uint8_t)(ts >> 8);
  h[7] = (uint8_t)ts;
  uint8_t* d = h + kRtpHeader;
  const int nch = im->channels;
  for (uint32_t i = 0; i < kFrameSize; ++i)
    for (int c = 0; c < nch; ++c, d += 3) PutL24(d, planar[(size_t)c * kFrameSize + i]);
  if (sendto(im->sock, (const char*)h, (int)im->pkt.size(), 0, (sockaddr*)&im->dst,
             sizeof(im->dst)) > 0) {
    im->packets.fetch_add(1, std::memory_order_relaxed);
  } else {
    im->send_errors.fetch_add(1, std::memory_order_relaxed);
    const int e = WSAGetLastError();
    if (e != im->last_error) {
      im->last_error = e;
      LOGW("tx: sendto failed on %s (%d)", im->cfg.address.c_str(), e);
    }
  }
}

int TxStream::channels() const { return impl_->channels; }
uint64_t TxStream::packets() const { return impl_->packets.load(); }
uint64_t TxStream::send_errors() const { return impl_->send_errors.load(); }
bool TxStream::running() const { return impl_->running.load(); }
uint32_t TxStream::LocalIpHost() const { return impl_->iface_ip_host; }

std::string TxStream::Sdp() const {
  if (!impl_->running.load()) return "";
  const uint32_t ip = impl_->iface_ip_host;
  char origin[24];
  snprintf(origin, sizeof(origin), "%u.%u.%u.%u", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
           (ip >> 8) & 0xff, ip & 0xff);
  const int domain = impl_->ptp_domain;
  char buf[1200];
  const int n = snprintf(
      buf, sizeof(buf),
      "v=0\r\n"
      "o=- %u 1 IN IP4 %s\r\n"
      "s=%s\r\n"
      "c=IN IP4 %s/32\r\n"
      "t=0 0\r\n"
      "m=audio %d RTP/AVP %d\r\n"
      "c=IN IP4 %s/32\r\n"
      "a=rtpmap:%d L24/48000/%d\r\n"
      "a=sendonly\r\n"
      "a=sync-time:0\r\n"
      "a=framecount:48\r\n"
      "a=ptime:1\r\n"
      "a=mediaclk:direct=0\r\n"
      "a=clock-domain:PTPv2 %d\r\n",
      1000u + (unsigned)impl_->cfg.id, origin, impl_->cfg.name.c_str(),
      impl_->cfg.address.c_str(), impl_->cfg.rtp_port, impl_->cfg.payload_type,
      impl_->cfg.address.c_str(), impl_->cfg.payload_type, impl_->channels, domain);
  std::string sdp(buf, n > 0 ? (size_t)n : 0);
  const std::string gmid = impl_->ptp ? impl_->ptp->GetInfo().gmid : "";
  if (!gmid.empty())
    sdp += "a=ts-refclk:ptp=IEEE1588-2008:" + gmid + ":" + std::to_string(domain) + "\r\n";
  return sdp;
}

}
