#include "media/AsioLink.h"

#include <windows.h>

#include <cstring>

#include "asio/AsioShared.h"
#include "common/Log.h"
#include "common/Types.h"

namespace aes67 {

using aes67asio::kBlock;
using aes67asio::kChannels;
using aes67asio::kRingMask;

static_assert(kChannels == kAsioChannels);
static_assert(aes67asio::kOutputDelay == kAsioTxDelaySamples);

AsioLink::~AsioLink() { Close(); }

bool AsioLink::Open(uint32_t preferred_buffer, std::string* err) {
  if (shm_) return true;
  const DWORD size = (DWORD)sizeof(aes67asio::Shared);
  HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  size, aes67asio::kShmName);
  if (!map) {
    if (err) *err = "CreateFileMapping failed (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
  void* view = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!view) {
    if (err) *err = "MapViewOfFile failed";
    CloseHandle(map);
    return false;
  }
  HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, aes67asio::kTickEventName);
  auto* s = static_cast<aes67asio::Shared*>(view);
  if (!existed || s->magic != aes67asio::kMagic) {
    memset(view, 0, size);
    s->version = aes67asio::kVersion;
    s->sample_rate = aes67asio::kSampleRate;
    s->channels = kChannels;
    s->magic = aes67asio::kMagic;
  }
  s->preferred_buffer = preferred_buffer;
  s->bridge_alive.store(1);
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  qpc_freq_ = f.QuadPart;
  boundary_event_ = CreateEventW(nullptr, FALSE, FALSE, aes67asio::kBoundaryEventName);
  map_ = map;
  event_ = evt;
  shm_ = s;
  LOGI("asio: shared memory ready (%u KB)%s", size / 1024,
       existed ? " (reattached)" : "");
  return true;
}

void AsioLink::Close() {
  if (shm_) {
    shm_->bridge_alive.store(0);
    UnmapViewOfFile(shm_);
    shm_ = nullptr;
  }
  if (event_) {
    CloseHandle((HANDLE)event_);
    event_ = nullptr;
  }
  fire_qpc_.store(0);
  if (boundary_event_) {
    CloseHandle((HANDLE)boundary_event_);
    boundary_event_ = nullptr;
  }
  if (map_) {
    CloseHandle((HANDLE)map_);
    map_ = nullptr;
  }
}

void AsioLink::SetPreferredBuffer(uint32_t frames) {
  if (shm_) shm_->preferred_buffer = frames;
}

void AsioLink::ReadToNet(uint64_t b, float* planar) {
  const size_t total = (size_t)kChannels * kBlock;
  if (!shm_ || !shm_->client_active.load(std::memory_order_acquire)) {
    memset(planar, 0, total * sizeof(float));
    return;
  }
  if (shm_->to_net_end.load(std::memory_order_acquire) < (int64_t)(b + kBlock)) {
    shortfalls_.fetch_add(1, std::memory_order_relaxed);
    memset(planar, 0, total * sizeof(float));
    return;
  }
  const uint64_t mask = shm_->out_active_mask.load(std::memory_order_relaxed);
  for (int c = 0; c < kChannels; ++c) {
    float* dst = planar + (size_t)c * kBlock;
    if (!(mask & (1ull << c))) {
      memset(dst, 0, kBlock * sizeof(float));
      continue;
    }
    const float* ring = shm_->to_net[c];
    for (uint32_t i = 0; i < kBlock; ++i) dst[i] = ring[(b + i) & kRingMask];
  }
}

void AsioLink::WriteFromNet(uint64_t b, const float* planar) {
  if (!shm_) return;
  for (int c = 0; c < kChannels; ++c) {
    float* ring = shm_->from_net[c];
    const float* src = planar + (size_t)c * kBlock;
    for (uint32_t i = 0; i < kBlock; ++i) ring[(b + i) & kRingMask] = src[i];
  }
}

void AsioLink::Publish(uint64_t end_sac) {
  if (!shm_) return;
  shm_->bridge_sac.store((int64_t)end_sac, std::memory_order_release);
  SetEvent((HANDLE)event_);
}

void AsioLink::SetClock(uint64_t sac, int64_t qpc) {
  if (!shm_) return;
  const uint32_t s = shm_->clock_seq.load(std::memory_order_relaxed);
  shm_->clock_seq.store(s + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  shm_->clock_sac.store((int64_t)sac, std::memory_order_relaxed);
  shm_->clock_qpc.store(qpc, std::memory_order_relaxed);
  shm_->clock_seq.store(s + 2, std::memory_order_release);
}

void AsioLink::ScheduleBoundary(uint64_t sac, int64_t qpc) {
  SpinPoll();
  if (!shm_ || !boundary_event_ || !shm_->client_active.load(std::memory_order_acquire)) {
    fire_qpc_.store(0, std::memory_order_relaxed);
    return;
  }
  const uint64_t b = shm_->client_buffer.load(std::memory_order_relaxed);
  if (b == 0) return;
  const uint64_t next = (sac / b + 1) * b;
  fire_qpc_.store(qpc + (int64_t)(next - sac) * qpc_freq_ / 48000, std::memory_order_relaxed);
}

void AsioLink::SpinPoll() {
  const int64_t fire = fire_qpc_.load(std::memory_order_relaxed);
  if (!fire) return;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (now.QuadPart < fire) return;
  fire_qpc_.store(0, std::memory_order_relaxed);
  if (shm_) {
    const int64_t late = (now.QuadPart - fire) * 1000000 / qpc_freq_;
    if (late > shm_->fire_late_max_us.load(std::memory_order_relaxed))
      shm_->fire_late_max_us.store(late, std::memory_order_relaxed);
    shm_->fire_qpc.store(now.QuadPart, std::memory_order_release);
  }
  SetEvent((HANDLE)boundary_event_);
}

void AsioLink::LogDiag() const {
  if (!shm_ || !shm_->client_active.load()) return;
  char name[65] = {};
  memcpy(name, shm_->client_name, 64);
  LOGI("stat: asio client=%s buf=%u callbacks=%llu cb-late-max=%lldus fire-late-max=%lldus "
       "wake-max=%lldus input-wait-max=%lldus proc-max=%lldus",
       name, shm_->client_buffer.load(), (unsigned long long)shm_->cb_count.exchange(0),
       (long long)shm_->cb_late_max_us.exchange(0), (long long)shm_->fire_late_max_us.exchange(0),
       (long long)shm_->wake_max_us.exchange(0), (long long)shm_->cb_wait_max_us.exchange(0),
       (long long)shm_->cb_proc_max_us.exchange(0));
}

void AsioLink::WaitForOutput(uint64_t read_b, uint64_t now_sac, int max_us) {
  if (!shm_ || now_sac < wait_backoff_until_) return;
  if (!shm_->client_active.load(std::memory_order_acquire)) return;
  const int64_t need = (int64_t)(read_b + kBlock);
  int64_t have = shm_->to_net_end.load(std::memory_order_acquire);
  if (have >= need) return;
  if (need - have > 4096) return;
  Publish(now_sac);
  LARGE_INTEGER f, t0, t;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  const int64_t limit = f.QuadPart * max_us / 1000000;
  do {
    YieldProcessor();
    have = shm_->to_net_end.load(std::memory_order_acquire);
    if (have >= need) return;
    QueryPerformanceCounter(&t);
  } while (t.QuadPart - t0.QuadPart < limit);
  wait_backoff_until_ = now_sac + 48000 / 10;
}

AsioLink::Status AsioLink::GetStatus() const {
  Status st;
  if (!shm_) return st;
  st.shm_ok = true;
  st.client_active = shm_->client_active.load() != 0;
  if (st.client_active) {
    const DWORD pid = shm_->client_pid.load();
    HANDLE h = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    if (!h || WaitForSingleObject(h, 0) == WAIT_OBJECT_0) st.client_active = false;
    if (h) CloseHandle(h);
  }
  if (st.client_active) {
    char name[65] = {};
    memcpy(name, shm_->client_name, 64);
    st.client_name = name;
    st.client_buffer = shm_->client_buffer.load();
    st.in_mask = shm_->in_active_mask.load();
    st.out_mask = shm_->out_active_mask.load();
  }
  return st;
}

}
