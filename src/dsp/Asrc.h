#pragma once

#include <cstddef>
#include <vector>

struct SRC_STATE_tag;

namespace aes67 {

class Resampler {
 public:
  Resampler() = default;
  ~Resampler();
  Resampler(const Resampler&) = delete;
  Resampler& operator=(const Resampler&) = delete;

  bool Init(int channels);
  void Reset();
  int channels() const { return ch_; }

  void Process(const float* in, size_t frames, double ratio, std::vector<float>& out);

 private:
  SRC_STATE_tag* st_ = nullptr;
  int ch_ = 0;
  std::vector<float> tmp_;
};

class DriftController {
 public:
  void Reset(double target_frames);
  double Update(double fill_frames);
  void ResetFilter() { ema_ = -1.0; }
  double ppm() const { return ppm_; }
  double filtered_fill() const { return ema_ < 0 ? 0 : ema_; }
  double target() const { return target_; }

 private:
  double target_ = 0;
  double ema_ = -1.0;
  double integ_ = 0;
  double ppm_ = 0;
};

}
