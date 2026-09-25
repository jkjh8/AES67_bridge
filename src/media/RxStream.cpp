#include "media/RxStream.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include "common/Log.h"
#include "ptp/PtpClient.h"

namespace aes67 {
namespace {
constexpr uint32_t kFrameSize = 48;
constexpr uint32_t kRingLen = 65536;
constexpr uint32_t kRingMask = kRingLen - 1;

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
    if (getsockname(s, (sockaddr*)&local, &len) == 0)
      ip = ntohl(local.sin_addr.s_addr);
  }
  closesocket(s);
  return ip;
}
}

struct RxStream::Impl {
  PtpClient* ptp = nullptr;
  SOCKET sock = INVALID_SOCKET;
  LPFN_WSARECVMSG recvmsg = nullptr;
  std::thread thread;
  std::atomic<bool> running{false};

  RxConfig cfg{};
  uint32_t group_be = 0;
  uint32_t src_ch = 2;
  uint32_t link_off = 192;

  std::vector<std::vector<float>> lives;
  std::atomic<uint32_t> end_ts{0};
  std::atomic<bool> got_audio{false};
  std::atomic<uint64_t> packets{0};
  std::atomic<uint64_t> last_pkt_ms{0};
  std::atomic<uint64_t> filtered{0};
  std::atomic<uint64_t> pt_mismatch{0};
  std::atomic<uint64_t> blk_ok{0}, blk_late{0}, blk_stale{0};
  std::atomic<int32_t> lead{0};
  std::atomic<float> peak{0.0f};

  void RecvLoop();
};

void RxStream::Impl::RecvLoop() {
  std::vector<uint8_t> buf(4096);
  char ctrl[WSA_CMSG_SPACE(sizeof(IN_PKTINFO))];
  while (running.load()) {
    WSABUF wb{(ULONG)buf.size(), (CHAR*)buf.data()};
    sockaddr_in from{};
    WSAMSG msg{};
    msg.name = (LPSOCKADDR)&from;
    msg.namelen = sizeof(from);
    msg.lpBuffers = &wb;
    msg.dwBufferCount = 1;
    msg.Control.buf = ctrl;
    msg.Control.len = sizeof(ctrl);
    DWORD got = 0;
    if (recvmsg(sock, &msg, &got, nullptr, nullptr) == SOCKET_ERROR) continue;
    const int n = (int)got;
    bool mine = true;
    for (WSACMSGHDR* c = WSA_CMSG_FIRSTHDR(&msg); c; c = WSA_CMSG_NXTHDR(&msg, c)) {
      if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
        const IN_PKTINFO* pi = (const IN_PKTINFO*)WSA_CMSG_DATA(c);
        mine = (pi->ipi_addr.s_addr == group_be);
      }
    }
    if (!mine) {
      filtered.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (n < 12) continue;

    const uint8_t* p = buf.data();
    if ((p[0] >> 6) != 2) continue;
    if ((p[1] & 0x7f) != (uint8_t)cfg.payload_type) {
      pt_mismatch.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    uint32_t hdr = 12 + (p[0] & 0x0f) * 4;
    if (p[0] & 0x10) {
      if ((uint32_t)n < hdr + 4) continue;
      hdr += 4 + (((uint32_t)p[hdr + 2] << 8) | p[hdr + 3]) * 4;
    }
    if ((uint32_t)n <= hdr) continue;
    const uint32_t rtp_ts = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
                            ((uint32_t)p[6] << 8) | p[7];
    const uint8_t* pay = p + hdr;
    const uint32_t bpf = src_ch * 3;
    const uint32_t frames = ((uint32_t)n - hdr) / bpf;
    for (uint32_t i = 0; i < frames; ++i) {
      const uint8_t* fr = pay + (size_t)i * bpf;
      const uint32_t idx = (rtp_ts + i) & kRingMask;
      for (uint32_t c = 0; c < src_ch; ++c) {
        const uint8_t* s = fr + (size_t)c * 3;
        int32_t x = ((int32_t)s[0] << 16) | ((int32_t)s[1] << 8) | s[2];
        if (x & 0x800000) x |= (int32_t)0xFF000000;
        lives[c][idx] = (float)(x / 8388608.0);
      }
    }
    const uint32_t end = rtp_ts + frames;
    const uint32_t prev = end_ts.load(std::memory_order_relaxed);
    if (!got_audio.load(std::memory_order_relaxed) || (int32_t)(end - prev) > 0)
      end_ts.store(end, std::memory_order_release);
    got_audio.store(true, std::memory_order_release);
    last_pkt_ms.store(GetTickCount64(), std::memory_order_relaxed);
    packets.fetch_add(1, std::memory_order_relaxed);
  }
}

RxStream::RxStream() : impl_(std::make_unique<Impl>()) {}
RxStream::~RxStream() { Stop(); }

bool RxStream::Start(const RxConfig& cfg, int delay_ms, uint32_t ifaceIpHost, PtpClient* ptp,
                     std::string* err) {
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);

  if (ifaceIpHost == 0) ifaceIpHost = LocalIpv4Host();
  Impl* im = impl_.get();
  im->cfg = cfg;
  im->ptp = ptp;
  im->src_ch = (cfg.channels >= 1 && cfg.channels <= 64) ? cfg.channels : 2;
  im->link_off = (uint32_t)std::clamp(delay_ms, kMinRxDelayMs, kMaxRxDelayMs) * 48;
  im->group_be = inet_addr(cfg.address.c_str());
  im->got_audio = false;
  im->packets = 0;
  im->lives.assign(im->src_ch, std::vector<float>(kRingLen, 0.0f));

  auto fail = [&](const char* what) {
    if (err) *err = what;
    if (im->sock != INVALID_SOCKET) closesocket(im->sock);
    im->sock = INVALID_SOCKET;
    return false;
  };

  im->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (im->sock == INVALID_SOCKET) return fail("rx socket() failed");
  GUID gid = WSAID_WSARECVMSG;
  DWORD bytes = 0;
  if (WSAIoctl(im->sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &gid, sizeof(gid),
               &im->recvmsg, sizeof(im->recvmsg), &bytes, nullptr,
               nullptr) == SOCKET_ERROR)
    return fail("WSARecvMsg unavailable");
  BOOL on = TRUE;
  setsockopt(im->sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
  setsockopt(im->sock, IPPROTO_IP, IP_PKTINFO, (const char*)&on, sizeof(on));
  int rcv = 1 << 20;
  setsockopt(im->sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rcv, sizeof(rcv));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons((unsigned short)cfg.rtp_port);
  if (bind(im->sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    return fail("rx bind failed");
  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = im->group_be;
  mreq.imr_interface.s_addr = htonl(ifaceIpHost);
  if (setsockopt(im->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq,
                 sizeof(mreq)) == SOCKET_ERROR)
    return fail("rx IP_ADD_MEMBERSHIP failed");

  im->running = true;
  im->thread = std::thread([im] { im->RecvLoop(); });
  LOGI("rx: started '%s' %s:%d PT%d %uch delay=%dms (PTP-aligned)",
       cfg.name.c_str(), cfg.address.c_str(), cfg.rtp_port, cfg.payload_type,
       im->src_ch, delay_ms);
  return true;
}

bool RxStream::ReadBlock(uint64_t block_sac, float* planar) {
  Impl* im = impl_.get();
  const size_t total = (size_t)im->src_ch * kFrameSize;
  auto silence = [&] {
    for (size_t i = 0; i < total; ++i) planar[i] = 0.0f;
    return false;
  };
  if (!im->running.load() || !im->ptp) return silence();
  if (!im->got_audio.load(std::memory_order_acquire)) return silence();
  if (im->ptp->GetInfo().lock != PtpLock::Locked) return silence();
  if (block_sac < im->link_off) return silence();

  const uint32_t start = (uint32_t)(block_sac - im->link_off);
  const uint32_t end = im->end_ts.load(std::memory_order_acquire);
  const int32_t ahead = (int32_t)(end - (start + kFrameSize));
  im->lead.store(ahead, std::memory_order_relaxed);
  if (ahead < 0) {
    im->blk_late.fetch_add(1, std::memory_order_relaxed);
    return silence();
  }
  if (ahead > (int32_t)(kRingLen / 2)) {
    im->blk_stale.fetch_add(1, std::memory_order_relaxed);
    return silence();
  }
  float pk = 0.0f;
  for (uint32_t c = 0; c < im->src_ch; ++c) {
    const float* ring = im->lives[c].data();
    float* dst = planar + (size_t)c * kFrameSize;
    for (uint32_t i = 0; i < kFrameSize; ++i) {
      dst[i] = ring[(start + i) & kRingMask];
      const float a = dst[i] < 0 ? -dst[i] : dst[i];
      if (a > pk) pk = a;
    }
  }
  if (pk > im->peak.load(std::memory_order_relaxed)) im->peak.store(pk, std::memory_order_relaxed);
  im->blk_ok.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void RxStream::Stop() {
  if (!impl_->running.exchange(false)) return;
  if (impl_->sock != INVALID_SOCKET) {
    closesocket(impl_->sock);
    impl_->sock = INVALID_SOCKET;
  }
  if (impl_->thread.joinable()) impl_->thread.join();
  impl_->got_audio.store(false);
  LOGI("rx: stopped '%s'", impl_->cfg.name.c_str());
}

bool RxStream::running() const { return impl_->running.load(); }
int RxStream::channels() const { return (int)impl_->src_ch; }
uint64_t RxStream::packets() const { return impl_->packets.load(); }
uint64_t RxStream::filtered() const { return impl_->filtered.load(); }
uint64_t RxStream::pt_mismatch() const { return impl_->pt_mismatch.load(); }
std::string RxStream::DiagAndReset() {
  Impl* im = impl_.get();
  char b[160];
  snprintf(b, sizeof(b), "play ok=%llu late=%llu stale=%llu lead=%d smp peak=%.3f",
           (unsigned long long)im->blk_ok.exchange(0),
           (unsigned long long)im->blk_late.exchange(0),
           (unsigned long long)im->blk_stale.exchange(0), (int)im->lead.load(),
           (double)im->peak.exchange(0.0f));
  return b;
}

bool RxStream::receiving() const {
  return impl_->running.load() && impl_->packets.load() > 0 &&
         GetTickCount64() - impl_->last_pkt_ms.load() < 500;
}

}
