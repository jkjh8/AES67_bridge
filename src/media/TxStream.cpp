#include "media/TxStream.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

#include "common/Log.h"
#include "common/RtCpu.h"
#include "ptp/PtpClient.h"

namespace aes67 {
namespace {
constexpr uint32_t kFrameSize = 48;
constexpr size_t kRtpHeader = 12;
constexpr uint32_t kQueueSlots = 64;

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
  size_t pkt_len = 0;
  std::vector<uint8_t> slots;
  uint64_t due[kQueueSlots] = {};
  int64_t enq_qpc[kQueueSlots] = {};
  std::atomic<uint32_t> q_head{0}, q_tail{0};
  HANDLE wake = nullptr;
  std::thread thread;

  std::atomic<bool> running{false};
  std::atomic<uint64_t> packets{0};
  std::atomic<uint64_t> send_errors{0};
  std::atomic<uint64_t> q_drops{0};
  std::atomic<uint32_t> q_max{0};
  std::atomic<int64_t> send_max_us{0};
  std::atomic<int64_t> wake_max_us{0};
  std::atomic<int64_t> out_late_max_us{0};
  std::atomic<uint64_t> out_late_hist[5]{};
  std::atomic<uint64_t> inline_pkts{0}, queued_pkts{0};
  int64_t qpc_freq = 1;
  int last_error = 0;

  std::mutex sdp_mtx;
  std::string sdp_gmid;
  uint64_t sdp_version = 0;

  uint8_t* Slot(uint32_t i) { return slots.data() + (size_t)(i % kQueueSlots) * pkt_len; }
  int SendPacket(const uint8_t* p);
  void RecordLate(uint64_t due_sac);
  void SendLoop();
};

int TxStream::Impl::SendPacket(const uint8_t* p) {
  LARGE_INTEGER q0, q1;
  QueryPerformanceCounter(&q0);
  const int r = sendto(sock, (const char*)p, (int)pkt_len, 0, (sockaddr*)&dst, sizeof(dst));
  const int e = r > 0 ? 0 : WSAGetLastError();
  QueryPerformanceCounter(&q1);
  const int64_t us = (q1.QuadPart - q0.QuadPart) * 1000000 / qpc_freq;
  if (us > send_max_us.load(std::memory_order_relaxed)) send_max_us.store(us, std::memory_order_relaxed);
  if (r > 0) {
    packets.fetch_add(1, std::memory_order_relaxed);
    return 1;
  }
  if (e == WSAEWOULDBLOCK) return 0;
  send_errors.fetch_add(1, std::memory_order_relaxed);
  if (e != last_error) {
    last_error = e;
    LOGW("tx: sendto failed on %s (%d)", cfg.address.c_str(), e);
  }
  return -1;
}

void TxStream::Impl::RecordLate(uint64_t d) {
  if (!ptp) return;
  const int64_t due_ns = (int64_t)((d / 48000) * 1000000000ull + (d % 48000) * 1000000000ull / 48000);
  const int64_t late_us = ((int64_t)ptp->GlobalTime() - due_ns) / 1000;
  if (late_us > out_late_max_us.load(std::memory_order_relaxed))
    out_late_max_us.store(late_us, std::memory_order_relaxed);
  out_late_hist[late_us < 0 ? 0 : (late_us >= 1000 ? 4 : late_us / 250)].fetch_add(1, std::memory_order_relaxed);
}

void TxStream::Impl::SendLoop() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  PinCurrentThreadToRealtimeCores();
  while (running.load(std::memory_order_acquire)) {
    WaitForSingleObject(wake, 5);
    uint32_t h = q_head.load(std::memory_order_relaxed);
    const uint32_t t = q_tail.load(std::memory_order_acquire);
    bool waited = false;
    while (h != t && running.load(std::memory_order_acquire)) {
      const uint32_t si = h % kQueueSlots;
      if (!waited) {
        LARGE_INTEGER q0;
        QueryPerformanceCounter(&q0);
        const int64_t wu = (q0.QuadPart - enq_qpc[si]) * 1000000 / qpc_freq;
        if (wu > wake_max_us.load(std::memory_order_relaxed)) wake_max_us.store(wu, std::memory_order_relaxed);
      }
      if (SendPacket(Slot(h)) == 0) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(sock, &w);
        timeval tv{0, 2000};
        select(0, nullptr, &w, nullptr, &tv);
        waited = true;
        continue;
      }
      waited = false;
      queued_pkts.fetch_add(1, std::memory_order_relaxed);
      RecordLate(due[si]);
      ++h;
      q_head.store(h, std::memory_order_release);
    }
  }
}

TxStream::TxStream() : impl_(std::make_unique<Impl>()) {}
TxStream::~TxStream() {
  Stop();
  if (impl_->wake) CloseHandle(impl_->wake);
}

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
  LARGE_INTEGER qpc, qf;
  QueryPerformanceFrequency(&qf);
  im->qpc_freq = qf.QuadPart;
  QueryPerformanceCounter(&qpc);
  im->seq = (uint16_t)(qpc.QuadPart * 2654435761u >> 7);
  im->pkt_len = kRtpHeader + (size_t)kFrameSize * im->channels * 3;
  im->slots.assign(im->pkt_len * kQueueSlots, 0);
  im->q_head = 0;
  im->q_tail = 0;

  im->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (im->sock == INVALID_SOCKET) {
    if (err) *err = "tx socket() failed";
    return false;
  }
  u_long nonblock = 1;
  ioctlsocket(im->sock, FIONBIO, &nonblock);
  in_addr ifa;
  ifa.s_addr = htonl(ifaceIpHost);
  setsockopt(im->sock, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ifa, sizeof(ifa));
  DWORD ttl = (DWORD)(cfg.ttl > 0 && cfg.ttl < 256 ? cfg.ttl : 15);
  setsockopt(im->sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
  BOOL loop = TRUE;
  setsockopt(im->sock, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));
  int sndbuf = 1 << 20;
  setsockopt(im->sock, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));
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

  for (uint32_t i = 0; i < kQueueSlots; ++i) {
    uint8_t* h = im->Slot(i);
    h[0] = 0x80;
    h[1] = (uint8_t)(cfg.payload_type & 0x7F);
    h[8] = (uint8_t)(im->ssrc >> 24);
    h[9] = (uint8_t)(im->ssrc >> 16);
    h[10] = (uint8_t)(im->ssrc >> 8);
    h[11] = (uint8_t)im->ssrc;
  }

  if (!im->wake) im->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  im->running = true;
  im->thread = std::thread([im] { im->SendLoop(); });
  LOGI("tx: started %s:%d L24/%dch pt=%d", cfg.address.c_str(), cfg.rtp_port,
       im->channels, cfg.payload_type);
  return true;
}

void TxStream::Stop() {
  if (!impl_->running.exchange(false)) return;
  if (impl_->wake) SetEvent(impl_->wake);
  if (impl_->thread.joinable()) impl_->thread.join();
  if (impl_->sock != INVALID_SOCKET) closesocket(impl_->sock);
  impl_->sock = INVALID_SOCKET;
  LOGI("tx: stopped");
}

void TxStream::SendBlock(uint64_t block_sac, uint64_t due_sac, const float* planar) {
  Impl* im = impl_.get();
  if (!im->running.load()) return;
  const uint32_t tail = im->q_tail.load(std::memory_order_relaxed);
  const uint32_t depth = tail - im->q_head.load(std::memory_order_acquire);
  if (depth >= kQueueSlots) {
    im->q_drops.fetch_add(1, std::memory_order_relaxed);
    ++im->seq;
    return;
  }
  uint8_t* h = im->Slot(tail);
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
  const uint32_t si = tail % kQueueSlots;
  im->due[si] = due_sac;
  if (depth == 0) {
    const int r = im->SendPacket(h);
    if (r != 0) {
      im->inline_pkts.fetch_add(1, std::memory_order_relaxed);
      im->RecordLate(due_sac);
      return;
    }
  }
  if (depth + 1 > im->q_max.load(std::memory_order_relaxed))
    im->q_max.store(depth + 1, std::memory_order_relaxed);
  LARGE_INTEGER qn;
  QueryPerformanceCounter(&qn);
  im->enq_qpc[si] = qn.QuadPart;
  im->q_tail.store(tail + 1, std::memory_order_release);
  SetEvent(im->wake);
}

std::string TxStream::DiagAndReset() {
  Impl* im = impl_.get();
  char b[320];
  snprintf(b, sizeof(b),
           "inline=%llu queued=%llu queue-max=%u drops=%llu wake-max=%lldus sendto-max=%lldus out-late-max=%lldus hist(250us) %llu/%llu/%llu/%llu/%llu",
           (unsigned long long)im->inline_pkts.exchange(0), (unsigned long long)im->queued_pkts.exchange(0),
           im->q_max.exchange(0), (unsigned long long)im->q_drops.exchange(0),
           (long long)im->wake_max_us.exchange(0), (long long)im->send_max_us.exchange(0),
           (long long)im->out_late_max_us.exchange(0),
           (unsigned long long)im->out_late_hist[0].exchange(0), (unsigned long long)im->out_late_hist[1].exchange(0),
           (unsigned long long)im->out_late_hist[2].exchange(0), (unsigned long long)im->out_late_hist[3].exchange(0),
           (unsigned long long)im->out_late_hist[4].exchange(0));
  return b;
}

int TxStream::channels() const { return impl_->channels; }
uint64_t TxStream::packets() const { return impl_->packets.load(); }
uint64_t TxStream::send_errors() const { return impl_->send_errors.load(); }
bool TxStream::running() const { return impl_->running.load(); }
uint32_t TxStream::LocalIpHost() const { return impl_->iface_ip_host; }

std::string TxStream::Sdp() const {
  if (!impl_->running.load() || !impl_->ptp) return "";
  const std::string gmid = impl_->ptp->GetInfo().gmid;
  if (gmid.empty()) return "";
  uint64_t version;
  {
    std::lock_guard<std::mutex> lk(impl_->sdp_mtx);
    if (gmid != impl_->sdp_gmid) {
      impl_->sdp_gmid = gmid;
      impl_->sdp_version = std::max<uint64_t>(impl_->sdp_version + 1, (uint64_t)time(nullptr));
    }
    version = impl_->sdp_version;
  }
  const uint32_t ip = impl_->iface_ip_host;
  char origin[24];
  snprintf(origin, sizeof(origin), "%u.%u.%u.%u", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
           (ip >> 8) & 0xff, ip & 0xff);
  const int domain = impl_->ptp_domain;
  char buf[1200];
  const int n = snprintf(
      buf, sizeof(buf),
      "v=0\r\n"
      "o=- %u %llu IN IP4 %s\r\n"
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
      1000u + (unsigned)impl_->cfg.id, (unsigned long long)version, origin,
      impl_->cfg.name.c_str(),
      impl_->cfg.address.c_str(), impl_->cfg.rtp_port, impl_->cfg.payload_type,
      impl_->cfg.address.c_str(), impl_->cfg.payload_type, impl_->channels, domain);
  std::string sdp(buf, n > 0 ? (size_t)n : 0);
  sdp += "a=ts-refclk:ptp=IEEE1588-2008:" + gmid + ":" + std::to_string(domain) + "\r\n";
  return sdp;
}

}
