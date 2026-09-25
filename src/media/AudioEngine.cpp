#include "media/AudioEngine.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <map>
#include <mutex>
#include <utility>

#include "common/Log.h"
#include "common/SpscRing.h"
#include "dsp/Asrc.h"
#include "media/RxStream.h"
#include "media/TxStream.h"
#include "ptp/PtpClient.h"
#include "sap/SapAnnouncer.h"
#include "wasapi/WasapiCaptureSource.h"
#include "wasapi/WasapiRenderSink.h"

namespace aes67 {
namespace {
constexpr uint32_t kBlock = 48;
constexpr int kMaxDevChannels = 32;

uint32_t LocalIpv4Host() {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return 0;
  sockaddr_in probe{};
  probe.sin_family = AF_INET;
  probe.sin_addr.s_addr = inet_addr("8.8.8.8");
  probe.sin_port = htons(53);
  uint32_t ip = 0;
  if (connect(s, (sockaddr*)&probe, sizeof(probe)) == 0) {
    sockaddr_in local{};
    int len = sizeof(local);
    if (getsockname(s, (sockaddr*)&local, &len) == 0)
      ip = ntohl(local.sin_addr.s_addr);
  }
  closesocket(s);
  return ip;
}

int PortId(const std::string& port, const char* prefix) {
  const size_t n = strlen(prefix);
  if (port.compare(0, n, prefix) != 0 || port.size() == n) return -1;
  return atoi(port.c_str() + n);
}

constexpr int kMinPacketGapUs = 300;

void PaceAfter(const LARGE_INTEGER& since, int min_us) {
  static const int64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (int64_t)f.QuadPart;
  }();
  const int64_t need = freq * min_us / 1000000;
  LARGE_INTEGER now;
  do {
    YieldProcessor();
    QueryPerformanceCounter(&now);
  } while (now.QuadPart - since.QuadPart < need);
}

constexpr double kInTargetMs = 15.0;
constexpr double kOutTargetMs = 20.0;
}

struct TxEntry {
  TxConfig cfg;
  std::unique_ptr<TxStream> stream;
  std::unique_ptr<SapAnnouncer> sap;
  std::string err;
};

struct RxEntry {
  RxConfig cfg;
  std::unique_ptr<RxStream> stream;
  std::string err;
};

struct Plan {
  uint32_t in_ch = 0, out_ch = 0;
  std::vector<float> in_buf, out_buf;
  std::vector<float> asio_src, asio_dst;
  bool asio = false;
  struct TxB { int id; TxStream* s; std::vector<float> buf; };
  struct RxB { int id; RxStream* s; std::vector<float> buf; };
  std::vector<TxB> tx;
  std::vector<RxB> rx;
  std::vector<std::pair<const float*, float*>> xp;
  std::vector<float*> dst_rows;
  std::vector<float> scratch;
};

struct AudioEngine::Impl {
  PtpClient* ptp = nullptr;
  uint8_t domain = 0;
  SapConfig sap_cfg;
  uint32_t local_ip = 0;
  int rx_delay_ms = kMinRxDelayMs;

  mutable std::mutex mtx;
  std::map<int, TxEntry> tx;
  std::map<int, RxEntry> rx;
  std::vector<Route> routes;
  std::unique_ptr<Plan> plan;

  WasapiCaptureSource cap;
  FloatSpsc cap_fifo;
  uint32_t cap_ch = 0;
  DevStatus in_st;
  WasapiRenderSink ren;
  uint32_t ren_ch = 0;
  DevStatus out_st;

  AsioLink asio;

  Resampler in_rs, out_rs;
  DriftController in_pi, out_pi;
  double in_nominal = 1.0;
  double out_nominal = 1.0;
  bool in_priming = true;
  std::vector<float> in_chunk, in_stage, out_stage;
  uint64_t in_underruns = 0;
  std::atomic<double> in_ppm{0}, out_ppm{0}, in_fill_ms{0}, out_fill_ms{0};
  std::atomic<uint64_t> in_underruns_pub{0};
  uint32_t cap_rate = 48000, ren_rate = 48000;

  uint64_t eng_sac = 0;
  bool eng_init = false;
  LARGE_INTEGER last_block_qpc{};
  mutable std::atomic<uint64_t> tic_count{0}, tic_burst2{0}, tic_burst3{0}, max_lag{0};

  void RebuildLocked();
  void ProcessBlock(Plan& p, uint64_t b);
};

void AudioEngine::Impl::RebuildLocked() {
  auto p = std::make_unique<Plan>();
  p->in_ch = cap_ch;
  p->out_ch = ren_ch;
  p->in_buf.assign((size_t)p->in_ch * kBlock, 0.0f);
  p->out_buf.assign((size_t)p->out_ch * kBlock, 0.0f);
  p->asio = asio.ok();
  p->asio_src.assign((size_t)kAsioChannels * kBlock, 0.0f);
  p->asio_dst.assign((size_t)kAsioChannels * kBlock, 0.0f);
  for (auto& [id, e] : tx)
    if (e.stream && e.stream->running())
      p->tx.push_back({id, e.stream.get(),
                       std::vector<float>((size_t)e.stream->channels() * kBlock)});
  for (auto& [id, e] : rx)
    if (e.stream && e.stream->running())
      p->rx.push_back({id, e.stream.get(),
                       std::vector<float>((size_t)e.stream->channels() * kBlock)});
  size_t max_ch = std::max<size_t>(p->in_ch, p->out_ch);
  p->scratch.assign(max_ch * kBlock, 0.0f);

  auto src_row = [&](const std::string& port, int ch) -> const float* {
    if (ch < 0) return nullptr;
    if (port == "wasapi")
      return ch < (int)p->in_ch ? p->in_buf.data() + (size_t)ch * kBlock : nullptr;
    if (port == "asio")
      return (p->asio && ch < kAsioChannels) ? p->asio_src.data() + (size_t)ch * kBlock
                                             : nullptr;
    const int id = PortId(port, "rx");
    for (auto& r : p->rx)
      if (r.id == id && ch < r.s->channels()) return r.buf.data() + (size_t)ch * kBlock;
    return nullptr;
  };
  auto dst_row = [&](const std::string& port, int ch) -> float* {
    if (ch < 0) return nullptr;
    if (port == "wasapi")
      return ch < (int)p->out_ch ? p->out_buf.data() + (size_t)ch * kBlock : nullptr;
    if (port == "asio")
      return (p->asio && ch < kAsioChannels) ? p->asio_dst.data() + (size_t)ch * kBlock
                                             : nullptr;
    const int id = PortId(port, "tx");
    for (auto& t : p->tx)
      if (t.id == id && ch < t.s->channels()) return t.buf.data() + (size_t)ch * kBlock;
    return nullptr;
  };
  for (const Route& r : routes) {
    const float* s = src_row(r.src, r.src_ch);
    float* d = dst_row(r.dst, r.dst_ch);
    if (s && d) p->xp.push_back({s, d});
  }
  for (uint32_t c = 0; c < p->out_ch; ++c) p->dst_rows.push_back(p->out_buf.data() + c * kBlock);
  for (int c = 0; c < kAsioChannels; ++c) p->dst_rows.push_back(p->asio_dst.data() + c * kBlock);
  for (auto& t : p->tx)
    for (int c = 0; c < t.s->channels(); ++c) p->dst_rows.push_back(t.buf.data() + c * kBlock);
  plan = std::move(p);
}

void AudioEngine::Impl::ProcessBlock(Plan& p, uint64_t b) {
  if (p.in_ch) {
    const uint32_t ch = p.in_ch;
    const size_t need = (size_t)kBlock * ch;
    const double fill = (double)(cap_fifo.size() / ch);
    const double ppm = in_pi.Update(fill);
    if (in_priming && fill >= in_pi.target()) in_priming = false;
    const double ratio = in_nominal * (1.0 - ppm * 1e-6);
    while (!in_priming && in_stage.size() < need) {
      const size_t avail = cap_fifo.size() / ch;
      if (avail == 0) {
        in_priming = true;
        ++in_underruns;
        break;
      }
      const size_t n = std::min<size_t>(avail, 32);
      in_chunk.resize(n * ch);
      cap_fifo.pop(in_chunk.data(), n * ch);
      in_rs.Process(in_chunk.data(), n, ratio, in_stage);
    }
    if (in_stage.size() >= need) {
      for (uint32_t i = 0; i < kBlock; ++i)
        for (uint32_t c = 0; c < ch; ++c)
          p.in_buf[(size_t)c * kBlock + i] = in_stage[(size_t)i * ch + c];
      in_stage.erase(in_stage.begin(), in_stage.begin() + need);
    } else {
      std::fill(p.in_buf.begin(), p.in_buf.end(), 0.0f);
    }
    in_ppm.store(ppm, std::memory_order_relaxed);
    in_fill_ms.store(in_pi.filtered_fill() * 1000.0 / cap_rate, std::memory_order_relaxed);
    in_underruns_pub.store(in_underruns, std::memory_order_relaxed);
  }
  if (p.asio) asio.ReadToNet(b, p.asio_src.data());
  for (auto& r : p.rx) r.s->ReadBlock(b, r.buf.data());

  for (float* d : p.dst_rows) memset(d, 0, kBlock * sizeof(float));
  for (auto& [s, d] : p.xp)
    for (uint32_t i = 0; i < kBlock; ++i) d[i] += s[i];
  for (float* d : p.dst_rows)
    for (uint32_t i = 0; i < kBlock; ++i)
      d[i] = d[i] > 1.0f ? 1.0f : (d[i] < -1.0f ? -1.0f : d[i]);

  for (auto& t : p.tx) t.s->SendBlock(b, t.buf.data());
  if (p.out_ch) {
    float* tmp = p.scratch.data();
    for (uint32_t i = 0; i < kBlock; ++i)
      for (uint32_t c = 0; c < p.out_ch; ++c)
        tmp[(size_t)i * p.out_ch + c] = p.out_buf[(size_t)c * kBlock + i];
    const size_t rfill = ren.fifoFrames();
    if ((double)rfill < out_pi.target() * 0.25) {
      out_stage.assign(((size_t)out_pi.target() - rfill) * p.out_ch, 0.0f);
      ren.Push(out_stage.data(), (uint32_t)(out_stage.size() / p.out_ch));
      out_pi.ResetFilter();
    }
    const double ppm = out_pi.Update((double)ren.fifoFrames());
    out_stage.clear();
    out_rs.Process(tmp, kBlock, out_nominal * (1.0 - ppm * 1e-6), out_stage);
    if (!out_stage.empty()) ren.Push(out_stage.data(), (uint32_t)(out_stage.size() / p.out_ch));
    out_ppm.store(ppm, std::memory_order_relaxed);
    out_fill_ms.store(out_pi.filtered_fill() * 1000.0 / ren_rate, std::memory_order_relaxed);
  }
  if (p.asio) asio.WriteFromNet(b, p.asio_dst.data());
}

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() { Shutdown(); }

void AudioEngine::Init(PtpClient* ptp, uint8_t ptp_domain, const SapConfig& sap,
                       const AsioConfig& asio, uint32_t iface_ip_host) {
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  impl_->ptp = ptp;
  impl_->domain = ptp_domain;
  impl_->sap_cfg = sap;
  impl_->local_ip = iface_ip_host ? iface_ip_host : LocalIpv4Host();
  std::string err;
  if (!impl_->asio.Open((uint32_t)asio.preferred_buffer, &err))
    LOGW("asio: shared memory unavailable: %s", err.c_str());
  std::lock_guard<std::mutex> lk(impl_->mtx);
  impl_->RebuildLocked();
}

void AudioEngine::Shutdown() {
  Impl* im = impl_.get();
  std::map<int, TxEntry> tx;
  std::map<int, RxEntry> rx;
  {
    std::lock_guard<std::mutex> lk(im->mtx);
    im->plan.reset();
    tx.swap(im->tx);
    rx.swap(im->rx);
    im->cap_ch = im->ren_ch = 0;
  }
  for (auto& [id, e] : tx) {
    if (e.sap) e.sap->Stop();
    if (e.stream) e.stream->Stop();
  }
  for (auto& [id, e] : rx)
    if (e.stream) e.stream->Stop();
  im->cap.Stop();
  im->ren.Stop();
  im->asio.Close();
}

void AudioEngine::OnTic() {
  Impl* im = impl_.get();
  if (!im->ptp) return;
  std::lock_guard<std::mutex> lk(im->mtx);
  Plan* p = im->plan.get();
  if (!p) return;

  const uint64_t ptp_sac = im->ptp->GlobalSac();
  if (!im->eng_init || ptp_sac < im->eng_sac || ptp_sac - im->eng_sac > 4800) {
    if (im->eng_init)
      LOGW("engine: realign (clock %s by %lld samples)", ptp_sac < im->eng_sac ? "back" : "ahead",
           (long long)(ptp_sac < im->eng_sac ? im->eng_sac - ptp_sac : ptp_sac - im->eng_sac));
    im->eng_sac = ptp_sac - (ptp_sac % kBlock);
    im->eng_init = true;
  }
  {
    const uint64_t lag = ptp_sac - im->eng_sac;
    uint64_t prev = im->max_lag.load(std::memory_order_relaxed);
    if (lag > prev) im->max_lag.store(lag, std::memory_order_relaxed);
  }

  if (p->in_ch) {
    const size_t tgt = (size_t)(im->in_pi.target()) * p->in_ch;
    const size_t sz = im->cap_fifo.size();
    if (sz > tgt * 6) {
      im->cap_fifo.drop(sz - tgt);
      im->in_pi.ResetFilter();
    }
  }

  int guard = 0;
  while (im->eng_sac + kBlock <= ptp_sac && guard < 8) {
    PaceAfter(im->last_block_qpc, kMinPacketGapUs);
    if (p->asio) im->asio.WaitForOutput(im->eng_sac, 500);
    QueryPerformanceCounter(&im->last_block_qpc);
    im->ProcessBlock(*p, im->eng_sac);
    im->eng_sac += kBlock;
    ++guard;
  }
  im->tic_count.fetch_add(1, std::memory_order_relaxed);
  if (guard >= 2) im->tic_burst2.fetch_add(1, std::memory_order_relaxed);
  if (guard >= 3) im->tic_burst3.fetch_add(1, std::memory_order_relaxed);
  if (p->asio) im->asio.Publish(im->eng_sac);
}

bool AudioEngine::ApplyTx(const TxConfig& cfg, std::string* err) {
  Impl* im = impl_.get();
  RemoveTx(cfg.id);
  TxEntry e;
  e.cfg = cfg;
  bool ok = true;
  if (cfg.enabled) {
    e.stream = std::make_unique<TxStream>();
    std::string serr;
    if (!e.stream->Start(im->ptp, cfg, im->local_ip, im->domain, &serr)) {
      e.err = serr;
      e.stream.reset();
      ok = false;
      LOGE("engine: TX '%s' start failed: %s", cfg.name.c_str(), serr.c_str());
    } else if (im->sap_cfg.enabled) {
      e.sap = std::make_unique<SapAnnouncer>();
      TxStream* s = e.stream.get();
      if (!e.sap->Start([s] { return s->Sdp(); }, s->LocalIpHost(),
                        im->sap_cfg.interval_s, &serr)) {
        LOGW("engine: SAP for '%s' failed: %s", cfg.name.c_str(), serr.c_str());
        e.sap.reset();
      }
    }
  }
  if (err) *err = e.err;
  std::lock_guard<std::mutex> lk(im->mtx);
  im->tx[cfg.id] = std::move(e);
  im->RebuildLocked();
  return ok;
}

void AudioEngine::RemoveTx(int id) {
  Impl* im = impl_.get();
  TxEntry old;
  {
    std::lock_guard<std::mutex> lk(im->mtx);
    auto it = im->tx.find(id);
    if (it == im->tx.end()) return;
    old = std::move(it->second);
    im->tx.erase(it);
    im->RebuildLocked();
  }
  if (old.sap) old.sap->Stop();
  if (old.stream) old.stream->Stop();
}

void AudioEngine::SetRxDelay(int ms) { impl_->rx_delay_ms = ms; }

bool AudioEngine::ApplyRx(const RxConfig& cfg, std::string* err) {
  Impl* im = impl_.get();
  RemoveRx(cfg.id);
  RxEntry e;
  e.cfg = cfg;
  bool ok = true;
  if (cfg.enabled) {
    e.stream = std::make_unique<RxStream>();
    std::string serr;
    if (!e.stream->Start(cfg, im->rx_delay_ms, im->local_ip, im->ptp, &serr)) {
      e.err = serr;
      e.stream.reset();
      ok = false;
      LOGE("engine: RX '%s' start failed: %s", cfg.name.c_str(), serr.c_str());
    }
  }
  if (err) *err = e.err;
  std::lock_guard<std::mutex> lk(im->mtx);
  im->rx[cfg.id] = std::move(e);
  im->RebuildLocked();
  return ok;
}

void AudioEngine::RemoveRx(int id) {
  Impl* im = impl_.get();
  RxEntry old;
  {
    std::lock_guard<std::mutex> lk(im->mtx);
    auto it = im->rx.find(id);
    if (it == im->rx.end()) return;
    old = std::move(it->second);
    im->rx.erase(it);
    im->RebuildLocked();
  }
  if (old.stream) old.stream->Stop();
}

void AudioEngine::SetRoutes(const std::vector<Route>& routes) {
  std::lock_guard<std::mutex> lk(impl_->mtx);
  impl_->routes = routes;
  impl_->RebuildLocked();
}

void AudioEngine::ApplyAudio(const AudioDeviceConfig& cfg) {
  Impl* im = impl_.get();
  {
    std::lock_guard<std::mutex> lk(im->mtx);
    im->cap_ch = im->ren_ch = 0;
    im->RebuildLocked();
  }
  im->cap.Stop();
  im->ren.Stop();

  DevStatus in, out;
  uint32_t cap_ch = 0, ren_ch = 0;
  if (cfg.input_enabled) {
    im->cap_fifo.init((size_t)48000 * kMaxDevChannels / 4);
    std::string e;
    if (im->cap.Start(cfg.input_device, cfg.input_loopback,
                      [im](const float* p, uint32_t frames, uint32_t chans) {
                        im->cap_fifo.push(p, (size_t)frames * chans);
                      },
                      &e)) {
      const uint32_t cc = im->cap.channels();
      in.name = im->cap.deviceName();
      in.channels = (int)cc;
      in.rate = (int)im->cap.sampleRate();
      if (cc >= 1 && cc <= (uint32_t)kMaxDevChannels && im->in_rs.Init((int)cc)) {
        cap_ch = cc;
        in.running = true;
        im->cap_rate = im->cap.sampleRate();
        im->in_nominal = 48000.0 / im->cap_rate;
        im->in_pi.Reset(kInTargetMs * im->cap_rate / 1000.0);
        im->in_priming = true;
        im->in_underruns = 0;
        im->in_stage.clear();
        im->in_stage.reserve((size_t)cc * 4096);
      } else {
        im->cap.Stop();
        in.error = std::to_string(cc) + "-channel devices are not supported";
      }
    } else {
      in.error = e;
    }
  }
  if (cfg.output_enabled) {
    std::string e;
    if (im->ren.Start(cfg.output_device, &e)) {
      const uint32_t rc = im->ren.channels();
      out.name = im->ren.deviceName();
      out.channels = (int)rc;
      out.rate = (int)im->ren.sampleRate();
      if (rc >= 1 && rc <= (uint32_t)kMaxDevChannels && im->out_rs.Init((int)rc)) {
        ren_ch = rc;
        out.running = true;
        im->ren_rate = im->ren.sampleRate();
        im->out_nominal = im->ren_rate / 48000.0;
        im->out_pi.Reset(kOutTargetMs * im->ren_rate / 1000.0);
        im->out_stage.reserve((size_t)rc * 4096);
      } else {
        im->ren.Stop();
        out.error = std::to_string(rc) + "-channel devices are not supported";
      }
    } else {
      out.error = e;
    }
  }
  std::lock_guard<std::mutex> lk(im->mtx);
  im->in_st = in;
  im->out_st = out;
  im->cap_ch = cap_ch;
  im->ren_ch = ren_ch;
  im->RebuildLocked();
}

void AudioEngine::ApplyAsio(const AsioConfig& cfg) {
  impl_->asio.SetPreferredBuffer((uint32_t)cfg.preferred_buffer);
}

void AudioEngine::KickSap() {
  std::lock_guard<std::mutex> lk(impl_->mtx);
  for (auto& [id, e] : impl_->tx)
    if (e.sap) e.sap->Kick();
}

AudioEngine::Status AudioEngine::GetStatus(bool with_diag) const {
  const Impl* im = impl_.get();
  Status st;
  st.asio = im->asio.GetStatus();
  st.local_ip = im->local_ip;
  std::lock_guard<std::mutex> lk(im->mtx);
  st.in = im->in_st;
  st.out = im->out_st;
  if (with_diag) {
    st.tics = im->tic_count.exchange(0);
    st.tic_burst2 = im->tic_burst2.exchange(0);
    st.tic_burst3 = im->tic_burst3.exchange(0);
    st.max_lag = im->max_lag.exchange(0);
  }
  if (st.in.running) {
    st.in.asrc_ppm = im->in_ppm.load();
    st.in.fifo_ms = im->in_fill_ms.load();
    st.in.underruns = im->in_underruns_pub.load();
  }
  if (st.out.running) {
    st.out.asrc_ppm = im->out_ppm.load();
    st.out.fifo_ms = im->out_fill_ms.load();
  }
  for (const auto& [id, e] : im->tx) {
    TxStatus t;
    t.id = id;
    t.error = e.err;
    if (e.stream) {
      t.running = e.stream->running();
      t.packets = e.stream->packets();
      t.send_errors = e.stream->send_errors();
      t.sdp = e.stream->Sdp();
    }
    st.tx.push_back(std::move(t));
  }
  for (const auto& [id, e] : im->rx) {
    RxStatus r;
    r.id = id;
    r.error = e.err;
    if (e.stream) {
      r.running = e.stream->running();
      r.receiving = e.stream->receiving();
      r.packets = e.stream->packets();
      r.filtered = e.stream->filtered();
      r.pt_mismatch = e.stream->pt_mismatch();
      if (with_diag) r.diag = e.stream->DiagAndReset();
    }
    st.rx.push_back(std::move(r));
  }
  return st;
}

}
