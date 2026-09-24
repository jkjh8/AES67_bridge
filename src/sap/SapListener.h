#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aes67 {

struct SapSource {
  std::string name;
  std::string address;
  int port = 5004;
  int channels = 0;
  int payload_type = 0;
  uint64_t last_seen_ms = 0;
};

class SapListener {
 public:
  SapListener();
  ~SapListener();

  SapListener(const SapListener&) = delete;
  SapListener& operator=(const SapListener&) = delete;

  bool Start(uint32_t ifaceIpHost, std::string* err);
  void Stop();

  std::vector<SapSource> Sources() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
