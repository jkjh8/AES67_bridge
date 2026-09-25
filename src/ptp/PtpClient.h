#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace aes67 {

enum class PtpLock { Unlocked, Locking, Locked };

struct PtpInfo {
  PtpLock lock = PtpLock::Unlocked;
  std::string gmid;
};

class PtpClient {
 public:
  PtpClient();
  ~PtpClient();

  PtpClient(const PtpClient&) = delete;
  PtpClient& operator=(const PtpClient&) = delete;

  bool Start(uint32_t ifaceIpBE, const uint8_t* mac, uint8_t domain,
             std::function<void()> onTic, std::string* err);
  void Stop();

  PtpInfo GetInfo() const;

  uint64_t GlobalSac() const;
  uint64_t GlobalTime() const;
  std::string Diag() const;
  std::string ClockId() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
