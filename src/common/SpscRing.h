#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace aes67 {

class FloatSpsc {
 public:
  void init(size_t capacity_floats) {
    cap_ = capacity_floats + 1;
    buf_.assign(cap_, 0.0f);
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
  }

  size_t capacity() const { return cap_ ? cap_ - 1 : 0; }

  size_t size() const {
    const size_t h = head_.load(std::memory_order_acquire);
    const size_t t = tail_.load(std::memory_order_acquire);
    return (t + cap_ - h) % cap_;
  }

  size_t push(const float* p, size_t n) {
    size_t t = tail_.load(std::memory_order_relaxed);
    const size_t h = head_.load(std::memory_order_acquire);
    const size_t space = (h + cap_ - t - 1) % cap_;
    if (n > space) n = space;
    for (size_t i = 0; i < n; ++i) {
      buf_[t] = p[i];
      t = (t + 1) % cap_;
    }
    tail_.store(t, std::memory_order_release);
    return n;
  }

  size_t pop(float* p, size_t n) {
    size_t h = head_.load(std::memory_order_relaxed);
    const size_t t = tail_.load(std::memory_order_acquire);
    const size_t avail = (t + cap_ - h) % cap_;
    if (n > avail) n = avail;
    for (size_t i = 0; i < n; ++i) {
      p[i] = buf_[h];
      h = (h + 1) % cap_;
    }
    head_.store(h, std::memory_order_release);
    return n;
  }

  void drop(size_t n) {
    size_t h = head_.load(std::memory_order_relaxed);
    const size_t t = tail_.load(std::memory_order_acquire);
    const size_t avail = (t + cap_ - h) % cap_;
    if (n > avail) n = avail;
    head_.store((h + n) % cap_, std::memory_order_release);
  }

 private:
  std::vector<float> buf_;
  size_t cap_ = 0;
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
};

}
