#include "ptp/PtpClient.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#include <windows.h>
#include <timeapi.h>
#include <avrt.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "common/Log.h"

namespace aes67 {
namespace {
constexpr char kPtpMcast[] = "224.0.1.129";
constexpr uint16_t kEventPort = 319;
constexpr uint16_t kGeneralPort = 320;

constexpr uint8_t kMsgSync = 0x0;
constexpr uint8_t kMsgFollowUp = 0x8;
constexpr uint8_t kMsgAnnounce = 0xB;

constexpr int64_t kNsPerSec = 1000000000LL;
constexpr int64_t kStepThresholdNs = 1000000;
constexpr int64_t kWindowNs = 1000000000;
constexpr int kMaxPoints = 32;
constexpr double kMaxSlope = 500e-6;
constexpr double kPhaseTauS = 3.0;
constexpr double kLockPhaseNs = 200000.0;
constexpr double kUnlockPhaseNs = 1000000.0;
constexpr double kUnlockRmsNs = 2000000.0;
constexpr uint64_t kSyncTimeoutMs = 3000;

int64_t g_qpc_freq = 0;

int64_t QpcToNs(int64_t ticks) {
  const int64_t whole = ticks / g_qpc_freq;
  const int64_t rem = ticks % g_qpc_freq;
  return whole * kNsPerSec + rem * kNsPerSec / g_qpc_freq;
}

int64_t NowNs() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return QpcToNs(t.QuadPart);
}

uint16_t Be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t Be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
int64_t Be64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return (int64_t)v;
}

int64_t TimestampNs(const uint8_t* p) {
  uint64_t sec = 0;
  for (int i = 0; i < 6; ++i) sec = (sec << 8) | p[i];
  return (int64_t)sec * kNsPerSec + (int64_t)Be32(p + 6);
}

int64_t CorrectionNs(const uint8_t* hdr) { return Be64(hdr + 8) >> 16; }

std::string FormatId(const uint8_t* id) {
  char b[32];
  snprintf(b, sizeof(b), "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X", id[0], id[1],
           id[2], id[3], id[4], id[5], id[6], id[7]);
  return b;
}

struct PortId {
  uint8_t b[10] = {};
  bool operator<(const PortId& o) const { return memcmp(b, o.b, 10) < 0; }
  bool operator==(const PortId& o) const { return memcmp(b, o.b, 10) == 0; }
};

struct AnnounceData {
  uint8_t priority1 = 255, clock_class = 255, accuracy = 255, priority2 = 255;
  uint16_t variance = 0xFFFF;
  uint8_t gm[8] = {};
  uint16_t steps = 0xFFFF;
  int8_t log_interval = 1;
  uint64_t last_ms = 0;
  int count = 0;

  int Compare(const AnnounceData& o) const {
    auto cmp = [](unsigned a, unsigned b) { return a < b ? -1 : (a > b ? 1 : 0); };
    if (int c = cmp(priority1, o.priority1)) return c;
    if (int c = cmp(clock_class, o.clock_class)) return c;
    if (int c = cmp(accuracy, o.accuracy)) return c;
    if (int c = cmp(variance, o.variance)) return c;
    if (int c = cmp(priority2, o.priority2)) return c;
    if (int c = memcmp(gm, o.gm, 8)) return c < 0 ? -1 : 1;
    return cmp(steps, o.steps);
  }
  uint64_t TimeoutMs() const {
    const double interval = std::ldexp(1.0, log_interval);
    return (uint64_t)std::max(3000.0, 3.0 * interval * 1000.0);
  }
};
}

struct PtpClient::Impl {
  SOCKET s_event = INVALID_SOCKET;
  SOCKET s_general = INVALID_SOCKET;
  LPFN_WSARECVMSG recvmsg = nullptr;
  bool kernel_ts = false;
  std::atomic<bool> kernel_ts_seen{false};
  std::thread rx_thread;
  std::thread tic_thread;
  std::atomic<bool> running{false};

  uint32_t iface_ip = 0;
  uint8_t domain = 0;
  std::function<void()> on_tic;
  PortId self;

  mutable std::mutex mtx;
  bool have_model = false;
  int64_t base_local = 0;
  int64_t base_ptp = 0;
  double slope = 0.0;
  double err_ns = 1e9;
  double fit_rms = 1e9;
  int outliers = 0;
  PtpLock lock = PtpLock::Unlocked;
  uint64_t last_sync_ms = 0;

  std::map<PortId, AnnounceData> foreign;
  bool have_master = false;
  PortId master;
  AnnounceData master_data;

  uint16_t sync_seq = 0;
  bool sync_pending = false;
  int64_t sync_t2 = 0;
  int64_t sync_corr = 0;

  struct Point { int64_t local, ptp; };
  Point pts[kMaxPoints] = {};
  int npts = 0;
  int64_t win_start = 0;
  bool win_have = false;
  Point win_best{};

  int64_t PtpAtLocked(int64_t local) const {
    if (!have_model) return local;
    const double dt = (double)(local - base_local);
    return base_ptp + (int64_t)std::llround(dt * (1.0 + slope));
  }

  bool OpenSocket(SOCKET* out, uint16_t port, std::string* err);
  void RxLoop();
  void TicLoop();
  void Handle(const uint8_t* p, int n, int64_t rx_ns, bool event);
  void OnAnnounce(const uint8_t* p, int n, const PortId& src);
  void OnSample(int64_t t1, int64_t t2);
  bool Fit(double* beta, int64_t* x0, int64_t* y0, double* alpha);
  void ResetServo();
  void Housekeeping();
};

bool PtpClient::Impl::OpenSocket(SOCKET* out, uint16_t port, std::string* err) {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) {
    if (err) *err = "PTP socket() failed";
    return false;
  }
  BOOL on = TRUE;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
    if (err) *err = "PTP bind(" + std::to_string(port) + ") failed";
    closesocket(s);
    return false;
  }
  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = inet_addr(kPtpMcast);
  mreq.imr_interface.s_addr = iface_ip;
  if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq)) ==
      SOCKET_ERROR) {
    if (err) *err = "PTP IP_ADD_MEMBERSHIP failed";
    closesocket(s);
    return false;
  }
  if (iface_ip) {
    in_addr ifa;
    ifa.s_addr = iface_ip;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ifa, sizeof(ifa));
  }
  BOOL loop = FALSE;
  setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));
  DWORD ttl = 1;
  setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));

  TIMESTAMPING_CONFIG tc{};
  tc.Flags = TIMESTAMPING_FLAG_RX;
  DWORD bytes = 0;
  if (WSAIoctl(s, SIO_TIMESTAMPING, &tc, sizeof(tc), nullptr, 0, &bytes, nullptr,
               nullptr) == 0)
    kernel_ts = true;

  if (!recvmsg) {
    GUID gid = WSAID_WSARECVMSG;
    WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &gid, sizeof(gid), &recvmsg,
             sizeof(recvmsg), &bytes, nullptr, nullptr);
  }
  *out = s;
  return true;
}

void PtpClient::Impl::ResetServo() {
  have_model = false;
  slope = 0.0;
  err_ns = fit_rms = 1e9;
  outliers = 0;
  sync_pending = false;
  npts = 0;
  win_have = false;
  lock = have_master ? PtpLock::Locking : PtpLock::Unlocked;
}

void PtpClient::Impl::OnAnnounce(const uint8_t* p, int n, const PortId& src) {
  if (n < 64) return;
  AnnounceData a;
  a.priority1 = p[47];
  a.clock_class = p[48];
  a.accuracy = p[49];
  a.variance = Be16(p + 50);
  a.priority2 = p[52];
  memcpy(a.gm, p + 53, 8);
  a.steps = Be16(p + 61);
  a.log_interval = (int8_t)p[33];
  a.last_ms = GetTickCount64();

  auto& f = foreign[src];
  a.count = f.count + 1;
  f = a;

  if (have_master && src == master) {
    master_data = a;
    return;
  }
  if (a.count < 2) return;
  if (!have_master || a.Compare(master_data) < 0) {
    const bool had = have_master;
    have_master = true;
    master = src;
    master_data = a;
    ResetServo();
    LOGI("ptp: %s master %s (GM %s)", had ? "switched to" : "selected",
         FormatId(src.b).c_str(), FormatId(a.gm).c_str());
  }
}

bool PtpClient::Impl::Fit(double* beta, int64_t* x0, int64_t* y0, double* alpha) {
  if (npts < 3) return false;
  bool keep[kMaxPoints];
  for (int i = 0; i < npts; ++i) keep[i] = true;
  *x0 = pts[0].local;
  *y0 = pts[0].ptp;
  for (int pass = 0; pass < 2; ++pass) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    for (int i = 0; i < npts; ++i) {
      if (!keep[i]) continue;
      const double x = (double)(pts[i].local - *x0), y = (double)(pts[i].ptp - *y0);
      sx += x; sy += y; sxx += x * x; sxy += x * y;
      ++n;
    }
    if (n < 3) return false;
    const double den = n * sxx - sx * sx;
    if (den <= 0) return false;
    *beta = (n * sxy - sx * sy) / den;
    *alpha = (sy - *beta * sx) / n;
    double ss = 0;
    for (int i = 0; i < npts; ++i) {
      const double r = (double)(pts[i].ptp - *y0) -
                       (*alpha + *beta * (double)(pts[i].local - *x0));
      if (keep[i]) ss += r * r;
    }
    fit_rms = std::sqrt(ss / n);
    if (pass == 0) {
      const double cut = -std::max(50000.0, 2.0 * fit_rms);
      for (int i = 0; i < npts; ++i) {
        const double r = (double)(pts[i].ptp - *y0) -
                         (*alpha + *beta * (double)(pts[i].local - *x0));
        if (r < cut) keep[i] = false;
      }
    }
  }
  return true;
}

void PtpClient::Impl::OnSample(int64_t t1, int64_t t2) {
  last_sync_ms = GetTickCount64();

  if (!have_model) {
    base_local = t2;
    base_ptp = t1;
    slope = 0.0;
    have_model = true;
    lock = PtpLock::Locking;
    npts = 0;
    win_have = false;
    win_start = t2;
    err_ns = fit_rms = 1e9;
    return;
  }

  const double e = (double)(t1 - PtpAtLocked(t2));
  if (std::fabs(e) > kStepThresholdNs) {
    if (lock != PtpLock::Locked || ++outliers >= 8) {
      base_local = t2;
      base_ptp = t1;
      npts = 0;
      win_have = false;
      win_start = t2;
      outliers = 0;
      err_ns = fit_rms = 1e9;
      lock = PtpLock::Locking;
    }
    return;
  }
  outliers = 0;

  if (!win_have || (t1 - t2) > (win_best.ptp - win_best.local)) {
    win_best = {t2, t1};
    win_have = true;
  }
  if (t2 - win_start < kWindowNs) return;
  win_start = t2;
  win_have = false;

  if (npts == kMaxPoints) {
    for (int i = 1; i < kMaxPoints; ++i) pts[i - 1] = pts[i];
    --npts;
  }
  pts[npts++] = win_best;

  double beta = 1.0, alpha = 0.0;
  int64_t x0 = 0, y0 = 0;
  if (!Fit(&beta, &x0, &y0, &alpha)) return;

  const int64_t now_ptp = PtpAtLocked(t2);
  const double target = (double)y0 + alpha + beta * (double)(t2 - x0);
  const double phase = target - (double)now_ptp;
  err_ns = std::fabs(phase);
  base_ptp = now_ptp;
  base_local = t2;
  slope = std::clamp(beta - 1.0 + phase / (kPhaseTauS * 1e9), -kMaxSlope, kMaxSlope);

  if (lock != PtpLock::Locked && npts >= 8 && err_ns < kLockPhaseNs)
    lock = PtpLock::Locked;
  else if (lock == PtpLock::Locked && (err_ns > kUnlockPhaseNs || fit_rms > kUnlockRmsNs))
    lock = PtpLock::Locking;
}

void PtpClient::Impl::Housekeeping() {
  const uint64_t now = GetTickCount64();
  for (auto it = foreign.begin(); it != foreign.end();) {
    if (now - it->second.last_ms > it->second.TimeoutMs()) it = foreign.erase(it);
    else ++it;
  }
  if (have_master && foreign.find(master) == foreign.end()) {
    have_master = false;
    LOGW("ptp: master %s announce timeout", FormatId(master.b).c_str());
    const AnnounceData* best = nullptr;
    const PortId* best_id = nullptr;
    for (auto& [id, a] : foreign)
      if (a.count >= 2 && (!best || a.Compare(*best) < 0)) {
        best = &a;
        best_id = &id;
      }
    if (best) {
      have_master = true;
      master = *best_id;
      master_data = *best;
      LOGI("ptp: selected master %s (GM %s)", FormatId(master.b).c_str(),
           FormatId(best->gm).c_str());
    }
    ResetServo();
  }
  if (lock != PtpLock::Unlocked && now - last_sync_ms > kSyncTimeoutMs)
    lock = have_master ? PtpLock::Locking : PtpLock::Unlocked;
}

void PtpClient::Impl::Handle(const uint8_t* p, int n, int64_t rx_ns, bool event) {
  if (n < 34) return;
  if ((p[1] & 0x0F) != 2 || p[4] != domain) return;
  const uint8_t type = p[0] & 0x0F;
  PortId src;
  memcpy(src.b, p + 20, 10);
  if (src == self) return;

  std::lock_guard<std::mutex> lk(mtx);
  if (type == kMsgAnnounce) {
    OnAnnounce(p, n, src);
    return;
  }
  if (!have_master || !(src == master)) return;

  const uint16_t seq = Be16(p + 30);
  if (type == kMsgSync && event && n >= 44) {
    const bool two_step = (p[6] & 0x02) != 0;
    if (two_step) {
      sync_seq = seq;
      sync_t2 = rx_ns;
      sync_corr = CorrectionNs(p);
      sync_pending = true;
    } else {
      OnSample(TimestampNs(p + 34) + CorrectionNs(p), rx_ns);
    }
  } else if (type == kMsgFollowUp && n >= 44) {
    if (sync_pending && seq == sync_seq) {
      sync_pending = false;
      OnSample(TimestampNs(p + 34) + sync_corr + CorrectionNs(p), sync_t2);
    }
  }
}

void PtpClient::Impl::RxLoop() {
  DWORD task = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
  if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
  uint8_t buf[1500];
  char ctrl[WSA_CMSG_SPACE(sizeof(UINT64)) + 64];
  while (running.load()) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s_event, &rfds);
    FD_SET(s_general, &rfds);
    timeval tv{0, 100000};
    const int nfds = select(0, &rfds, nullptr, nullptr, &tv);
    if (nfds > 0) {
      for (int i = 0; i < 2; ++i) {
        SOCKET s = i == 0 ? s_event : s_general;
        if (!FD_ISSET(s, &rfds)) continue;
        WSABUF wb{(ULONG)sizeof(buf), (CHAR*)buf};
        sockaddr_in from{};
        WSAMSG msg{};
        msg.name = (LPSOCKADDR)&from;
        msg.namelen = sizeof(from);
        msg.lpBuffers = &wb;
        msg.dwBufferCount = 1;
        msg.Control.buf = ctrl;
        msg.Control.len = sizeof(ctrl);
        DWORD got = 0;
        int64_t rx_ns = 0;
        if (recvmsg) {
          if (recvmsg(s, &msg, &got, nullptr, nullptr) == SOCKET_ERROR) continue;
          rx_ns = NowNs();
          for (WSACMSGHDR* c = WSA_CMSG_FIRSTHDR(&msg); c; c = WSA_CMSG_NXTHDR(&msg, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMP) {
              UINT64 qpc;
              memcpy(&qpc, WSA_CMSG_DATA(c), sizeof(qpc));
              const int64_t k = qpc ? QpcToNs((int64_t)qpc) : 0;
              if (k > 0 && k <= rx_ns && rx_ns - k < 50000000) {
                rx_ns = k;
                kernel_ts_seen = true;
              }
            }
        } else {
          int fromlen = sizeof(from);
          const int r = recvfrom(s, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromlen);
          if (r <= 0) continue;
          got = (DWORD)r;
          rx_ns = NowNs();
        }
        Handle(buf, (int)got, rx_ns, i == 0);
      }
    }
    std::lock_guard<std::mutex> lk(mtx);
    Housekeeping();
  }
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void PtpClient::Impl::TicLoop() {
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  DWORD task = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
  if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);

  while (running.load()) {
    if (on_tic) on_tic();
    int64_t wait_ns;
    {
      std::lock_guard<std::mutex> lk(mtx);
      const int64_t now = NowNs();
      const int64_t p = PtpAtLocked(now);
      const int64_t next = (p / 1000000 + 1) * 1000000;
      wait_ns = (int64_t)((double)(next - p) / (1.0 + slope));
    }
    wait_ns = std::clamp<int64_t>(wait_ns, 0, 2000000);
    if (timer && wait_ns > 0) {
      LARGE_INTEGER due;
      due.QuadPart = -(wait_ns / 100);
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, 50);
    } else if (wait_ns > 0) {
      Sleep(1);
    }
  }
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  if (timer) CloseHandle(timer);
}

PtpClient::PtpClient() : impl_(std::make_unique<Impl>()) {}
PtpClient::~PtpClient() { Stop(); }

bool PtpClient::Start(uint32_t ifaceIpBE, const uint8_t* mac, uint8_t domain,
                      std::function<void()> onTic, std::string* err) {
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  timeBeginPeriod(1);
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_qpc_freq = f.QuadPart;

  Impl* im = impl_.get();
  im->iface_ip = ifaceIpBE;
  im->domain = domain;
  im->on_tic = std::move(onTic);

  if (mac) {
    const uint8_t id[8] = {mac[0], mac[1], mac[2], 0xFF, 0xFE, mac[3], mac[4], mac[5]};
    memcpy(im->self.b, id, 8);
  } else {
    LARGE_INTEGER seed;
    QueryPerformanceCounter(&seed);
    const uint32_t ip = ntohl(ifaceIpBE);
    const uint8_t id[8] = {0x02, (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), 0xFF, 0xFE,
                           (uint8_t)(ip >> 8), (uint8_t)ip, (uint8_t)(seed.QuadPart & 0xFF)};
    memcpy(im->self.b, id, 8);
  }
  im->self.b[8] = 0;
  im->self.b[9] = 1;

  if (!im->OpenSocket(&im->s_event, kEventPort, err)) return false;
  if (!im->OpenSocket(&im->s_general, kGeneralPort, err)) return false;

  im->running = true;
  im->tic_thread = std::thread([im] { im->TicLoop(); });
  im->rx_thread = std::thread([im] { im->RxLoop(); });
  in_addr ia;
  ia.s_addr = ifaceIpBE;
  LOGI("ptp: started on %s (domain=%u, clock id %s)", inet_ntoa(ia), domain,
       FormatId(im->self.b).c_str());
  return true;
}

void PtpClient::Stop() {
  if (!impl_->running.exchange(false)) return;
  if (impl_->s_event != INVALID_SOCKET) closesocket(impl_->s_event);
  if (impl_->s_general != INVALID_SOCKET) closesocket(impl_->s_general);
  if (impl_->rx_thread.joinable()) impl_->rx_thread.join();
  if (impl_->tic_thread.joinable()) impl_->tic_thread.join();
  impl_->s_event = impl_->s_general = INVALID_SOCKET;
  timeEndPeriod(1);
  LOGI("ptp: stopped");
}

PtpInfo PtpClient::GetInfo() const {
  std::lock_guard<std::mutex> lk(impl_->mtx);
  PtpInfo info;
  info.lock = impl_->lock;
  if (impl_->have_master) info.gmid = FormatId(impl_->master_data.gm);
  return info;
}

uint64_t PtpClient::GlobalTime() const {
  std::lock_guard<std::mutex> lk(impl_->mtx);
  return (uint64_t)impl_->PtpAtLocked(NowNs());
}

std::string PtpClient::Diag() const {
  std::lock_guard<std::mutex> lk(impl_->mtx);
  char b[200];
  snprintf(b, sizeof(b), "phase=%.1fus rms=%.1fus freq=%+.2fppm points=%d ts=%s",
           impl_->err_ns > 1e8 ? -1.0 : impl_->err_ns / 1000.0,
           impl_->fit_rms > 1e8 ? -1.0 : impl_->fit_rms / 1000.0, impl_->slope * 1e6,
           impl_->npts, impl_->kernel_ts_seen ? "kernel" : "user");
  return b;
}

std::string PtpClient::ClockId() const { return FormatId(impl_->self.b); }

uint64_t PtpClient::GlobalSac() const {
  const uint64_t t = GlobalTime();
  return (t / kNsPerSec) * 48000 + (t % kNsPerSec) * 48000 / kNsPerSec;
}

}
