#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace aes67 {

class SapAnnouncer {
 public:
  SapAnnouncer();
  ~SapAnnouncer();

  SapAnnouncer(const SapAnnouncer&) = delete;
  SapAnnouncer& operator=(const SapAnnouncer&) = delete;

  bool Start(std::function<std::string()> sdp_provider, uint32_t origin_ip_host,
             int interval_sec, std::string* err);
  void Stop();

  void Kick();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
