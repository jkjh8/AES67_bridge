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

  bool ApplyTx(const TxConfig& cfg, std::string* err);
  void RemoveTx(int id);
  void SetRxDelay(int ms);
  bool ApplyRx(const RxConfig& cfg, std::string* err);
  void RemoveRx(int id);
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
  };
  struct RxStatus {
    int id = 0;
    bool running = false;
    bool receiving = false;
    uint64_t packets = 0;
    uint64_t filtered = 0;
    uint64_t pt_mismatch = 0;
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
    uint64_t underruns = 0;
  };
  struct Status {
    std::vector<TxStatus> tx;
    std::vector<RxStatus> rx;
    DevStatus in, out;
    AsioLink::Status asio;
    uint32_t local_ip = 0;
    uint64_t tics = 0, tic_burst2 = 0, tic_burst3 = 0, max_lag = 0;
  };
  Status GetStatus(bool with_diag = false) const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}
