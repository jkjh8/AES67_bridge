#include "dsp/Asrc.h"

#include <algorithm>

#include <samplerate.h>

namespace aes67 {
namespace {
constexpr double kEmaAlpha = 0.002;
constexpr double kKp = 2.0;
constexpr double kKi = 0.06;
constexpr double kMaxIntegPpm = 800;
constexpr double kMaxPpm = 1000;
}

Resampler::~Resampler() {
  if (st_) src_delete(st_);
}

bool Resampler::Init(int channels) {
  if (st_) src_delete(st_);
  st_ = nullptr;
  ch_ = channels;
  if (channels <= 0) return false;
  int err = 0;
  st_ = src_new(SRC_SINC_FASTEST, channels, &err);
  tmp_.assign((size_t)channels * 4096, 0.0f);
  return st_ != nullptr;
}

void Resampler::Reset() {
  if (st_) src_reset(st_);
}

void Resampler::Process(const float* in, size_t frames, double ratio,
                        std::vector<float>& out) {
  if (!st_ || frames == 0) return;
  ratio = std::clamp(ratio, 1.0 / 16, 16.0);
  const size_t cap = tmp_.size() / (size_t)ch_;
  while (frames > 0) {
    SRC_DATA d{};
    d.data_in = in;
    d.input_frames = (long)std::min<size_t>(frames, cap / 2);
    d.data_out = tmp_.data();
    d.output_frames = (long)cap;
    d.src_ratio = ratio;
    d.end_of_input = 0;
    if (src_process(st_, &d) != 0) return;
    out.insert(out.end(), tmp_.data(), tmp_.data() + (size_t)d.output_frames_gen * ch_);
    if (d.input_frames_used <= 0 && d.output_frames_gen <= 0) return;
    in += (size_t)d.input_frames_used * ch_;
    frames -= (size_t)d.input_frames_used;
  }
}

void DriftController::Reset(double target_frames) {
  target_ = target_frames;
  ema_ = -1.0;
  integ_ = 0;
  ppm_ = 0;
}

double DriftController::Update(double fill) {
  ema_ = ema_ < 0 ? fill : ema_ + kEmaAlpha * (fill - ema_);
  const double e = ema_ - target_;
  integ_ += e * 0.001;
  integ_ = std::clamp(integ_, -kMaxIntegPpm / kKi, kMaxIntegPpm / kKi);
  ppm_ = std::clamp(kKp * e + kKi * integ_, -kMaxPpm, kMaxPpm);
  return ppm_;
}

}
