#pragma once

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

  void WaitForOutput(uint64_t b, int max_us);

  struct Status {
    bool shm_ok = false;
    bool client_active = false;
    std::string client_name;
    uint32_t client_buffer = 0;
    uint32_t in_mask = 0, out_mask = 0;
  };
  Status GetStatus() const;

 private:
  void* map_ = nullptr;
  void* event_ = nullptr;
  aes67asio::Shared* shm_ = nullptr;
  uint64_t wait_backoff_until_ = 0;
};

}
