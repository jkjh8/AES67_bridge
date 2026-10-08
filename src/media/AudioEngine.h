#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/Types.h"
#include "media/AsioLink.h"

namespace aes67 {

class PtpClient;

class AudioEngine {
 public:
  AudioEngine();
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  void Init(PtpClient* ptp, uint8_t ptp_domain, const SapConfig& sap,
            const AsioConfig& asio, uint32_t iface_ip_host);
  void Shutdown();

  void OnTic();
  void OnSpin();

  bool ApplyTx(const TxConfig& cfg, std::string* err);
  void RemoveTx(int id);
  void SetRxDelay(int ms);
  void SetTxMargin(int us);
  bool ApplyRx(const RxConfig& cfg, std::string* err);
  void RemoveRx(int id);
  void ResetRxCounts(int id);
  void SetRoutes(const std::vector<Route>& routes);
  void ApplyAudio(const AudioDeviceConfig& cfg);
  void ApplyAsio(const AsioConfig& cfg);
  void KickSap();

  struct TxStatus {
    int id = 0;
    bool running = false;
    uint64_t packets = 0;
    uint64_t send_errors = 0;
    std::string error;
    std::string sdp;
    std::string diag;
  };
  struct RxStatus {
    int id = 0;
    bool running = false;
    bool receiving = false;
    uint64_t packets = 0;
    uint64_t filtered = 0;
    uint64_t pt_mismatch = 0;
    uint64_t lost = 0;
    uint64_t late = 0;
    uint64_t bad = 0;
    uint32_t ssrc = 0;
    uint64_t ssrc_changes = 0;
    uint64_t since_ms = 0;
    std::string error;
    std::string diag;
  };
  struct DevStatus {
    bool running = false;
    int channels = 0;
    int rate = 0;
    std::string name;
    std::string error;
    double asrc_ppm = 0;
    double fifo_ms = 0;
    double target_ms = 0;
    double min_ms = 0;
    uint64_t underruns = 0;
  };
  struct Status {
    std::vector<TxStatus> tx;
    std::vector<RxStatus> rx;
    DevStatus in, out;
    AsioLink::Status asio;
    uint32_t local_ip = 0;
    uint64_t tics = 0, tic_burst2 = 0, tic_burst3 = 0, max_lag = 0;
    int64_t tx_late_max_ns = 0;
    uint64_t tx_late_over = 0;
    uint64_t asio_shortfalls = 0;
    uint64_t tx_late_hist[5] = {};
  };
  Status GetStatus(bool with_diag = false) const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}
