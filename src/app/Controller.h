#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "common/Types.h"

namespace aes67 {

class PtpClient;
class AudioEngine;
class SapListener;

class Controller {
 public:
  Controller(std::filesystem::path config_path, AppConfig cfg);
  ~Controller();

  bool Start();
  void Stop();

  void Tick();

  std::string HandleCommand(const std::string& json, bool* send_devices);

  std::string StateJson() const;
  std::string DevicesJson() const;
  std::wstring Tooltip() const;

 private:
  void Persist();
  void PruneRoutes();

  std::filesystem::path config_path_;
  AppConfig cfg_;
  std::unique_ptr<PtpClient> ptp_;
  std::unique_ptr<AudioEngine> engine_;
  std::unique_ptr<SapListener> sap_listener_;
  int last_ptp_lock_ = -1;
  int tick_count_ = 0;
};

}
