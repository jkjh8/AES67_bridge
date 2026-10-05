#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace aes67asio { struct Shared; }

namespace aes67 {

class AsioLink {
 public:
  AsioLink() = default;
  ~AsioLink();

  AsioLink(const AsioLink&) = delete;
  AsioLink& operator=(const AsioLink&) = delete;

  bool Open(uint32_t preferred_buffer, std::string* err);
  void Close();
  bool ok() const { return shm_ != nullptr; }

  void SetPreferredBuffer(uint32_t frames);

  void ReadToNet(uint64_t block_sac, float* planar);
  void WriteFromNet(uint64_t block_sac, const float* planar);
  void Publish(uint64_t end_sac);
  void SetClock(uint64_t sac, int64_t qpc);
  void ScheduleBoundary(uint64_t sac, int64_t qpc);
  void SpinPoll();
  void LogDiag() const;

  void WaitForOutput(uint64_t read_b, uint64_t now_sac, int max_us);
  uint64_t TakeShortfalls() const { return shortfalls_.exchange(0); }

  struct Status {
    bool shm_ok = false;
    bool client_active = false;
    std::string client_name;
    uint32_t client_buffer = 0;
    uint64_t in_mask = 0, out_mask = 0;
  };
  Status GetStatus() const;

 private:
  void* map_ = nullptr;
  void* event_ = nullptr;
  void* boundary_event_ = nullptr;
  int64_t qpc_freq_ = 1;
  std::atomic<int64_t> fire_qpc_{0};
  aes67asio::Shared* shm_ = nullptr;
  uint64_t wait_backoff_until_ = 0;
  mutable std::atomic<uint64_t> shortfalls_{0};
};

}
