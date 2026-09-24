#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace aes67 {

class WasapiCaptureSource {
 public:
  WasapiCaptureSource();
  ~WasapiCaptureSource();

  WasapiCaptureSource(const WasapiCaptureSource&) = delete;
  WasapiCaptureSource& operator=(const WasapiCaptureSource&) = delete;

  bool Start(const std::string& device_id, bool loopback,
             std::function<void(const float*, uint32_t, uint32_t)> onFrames,
             std::string* err);
  void Stop();

  uint32_t sampleRate() const;
  uint32_t channels() const;
  const std::string& deviceName() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
