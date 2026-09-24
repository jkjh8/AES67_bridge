#pragma once

#include <atomic>
#include <cstdint>

namespace aes67asio {

inline constexpr wchar_t kShmName[] = L"Local\\AES67BridgeASIO.v1";
inline constexpr wchar_t kTickEventName[] = L"Local\\AES67BridgeASIO.v1.tick";
inline constexpr uint32_t kMagic = 0x41363742;
inline constexpr uint32_t kVersion = 1;
inline constexpr int kChannels = 32;
inline constexpr uint32_t kRingLen = 16384;
inline constexpr uint32_t kRingMask = kRingLen - 1;
inline constexpr uint32_t kBlock = 48;
inline constexpr uint32_t kSafety = 48;
inline constexpr uint32_t kMinBuffer = 64;
inline constexpr uint32_t kMaxBuffer = 2048;
inline constexpr uint32_t kDefaultBuffer = 256;

inline uint32_t NormalizeBuffer(uint32_t b) {
  uint32_t p = kMinBuffer;
  while (p < b && p < kMaxBuffer) p <<= 1;
  return p;
}
inline constexpr uint32_t kSampleRate = 48000;

struct Shared {
  uint32_t magic;
  uint32_t version;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t preferred_buffer;
  uint32_t reserved0;

  std::atomic<uint32_t> bridge_alive;
  std::atomic<int64_t> bridge_sac;

  std::atomic<uint32_t> client_active;
  std::atomic<uint32_t> client_buffer;
  std::atomic<int64_t> to_net_end;
  std::atomic<uint32_t> client_pid;
  char client_name[64];

  std::atomic<uint32_t> in_active_mask;
  std::atomic<uint32_t> out_active_mask;

  float from_net[kChannels][kRingLen];
  float to_net[kChannels][kRingLen];
};

static_assert(std::atomic<int64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);

}
