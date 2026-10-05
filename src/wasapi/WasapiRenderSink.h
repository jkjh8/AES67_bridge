#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace aes67 {

class WasapiRenderSink {
 public:
  WasapiRenderSink();
  ~WasapiRenderSink();

  WasapiRenderSink(const WasapiRenderSink&) = delete;
  WasapiRenderSink& operator=(const WasapiRenderSink&) = delete;

  bool Start(const std::string& device_id, std::string* err);
  void Stop();

  void Push(const float* interleaved, uint32_t frames);

  uint32_t sampleRate() const;
  uint32_t channels() const;
  const std::string& deviceName() const;
  size_t fifoFrames() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
