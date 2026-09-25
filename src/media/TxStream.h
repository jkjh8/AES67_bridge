#pragma once

#include <memory>
#include <string>

#include "common/Types.h"

namespace aes67 {

class PtpClient;

class TxStream {
 public:
  TxStream();
  ~TxStream();

  TxStream(const TxStream&) = delete;
  TxStream& operator=(const TxStream&) = delete;

  bool Start(PtpClient* ptp, const TxConfig& cfg, uint32_t ifaceIpHost,
             uint8_t ptpDomain, std::string* err);
  void Stop();

  void SendBlock(uint64_t block_sac, const float* planar);

  int channels() const;
  uint64_t packets() const;
  uint64_t send_errors() const;

  bool running() const;
  std::string Sdp() const;
  uint32_t LocalIpHost() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}
