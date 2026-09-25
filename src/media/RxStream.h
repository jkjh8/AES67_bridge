#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/Types.h"

namespace aes67 {

class PtpClient;

class RxStream {
 public:
  RxStream();
  ~RxStream();

  RxStream(const RxStream&) = delete;
  RxStream& operator=(const RxStream&) = delete;

  bool Start(const RxConfig& cfg, int delay_ms, uint32_t ifaceIpHost, PtpClient* ptp,
             std::string* err);
  void Stop();
  bool running() const;

  bool ReadBlock(uint64_t block_sac, float* planar);

  int channels() const;
  uint64_t packets() const;
  bool receiving() const;
  uint64_t filtered() const;
  uint64_t pt_mismatch() const;
  std::string DiagAndReset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
