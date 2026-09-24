#include <windows.h>
#include <objbase.h>

#include "common/Config.h"
#include "version.h"
#include "common/Log.h"
#include "ui/TrayApp.h"

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
  using namespace aes67;

  HANDLE inst_mutex = CreateMutexW(nullptr, TRUE, L"Global\\AES67BridgeSingleton");
  if (inst_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND w = FindWindowExW(HWND_MESSAGE, nullptr, kTrayWndClass, nullptr)) {
      AllowSetForegroundWindow(ASFW_ANY);
      PostMessageW(w, kMsgOpenUi, 0, 0);
    }
    return 0;
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  const auto config_path = DefaultConfigPath();
  bool used_defaults = false;
  AppConfig cfg = LoadConfig(config_path, &used_defaults);

  LogInit(cfg.log_severity.c_str());
  LOGI("AES67 Bridge " AES67B_VERSION " starting (config: %ls, defaults=%d)", config_path.c_str(),
       (int)used_defaults);

  if (const std::string err = ValidateConfig(cfg); !err.empty()) {
    LOGW("config invalid (%s); continuing with values as-is", err.c_str());
  }
  if (used_defaults) {
    TxConfig tx;
    tx.id = 1;
    cfg.tx.push_back(tx);
    cfg.routes.push_back({"wasapi", 0, "tx1", 0});
    cfg.routes.push_back({"wasapi", 1, "tx1", 1});
    if (SaveConfig(config_path, cfg))
      LOGI("seeded default config at %ls", config_path.c_str());
  }

  TrayApp app(hinst, config_path, cfg);
  if (!app.Create()) {
    LOGE("failed to create tray app");
    LogShutdown();
    return 1;
  }

  const int rc = app.RunMessageLoop();
  LOGI("AES67 Bridge exiting (%d)", rc);
  LogShutdown();
  if (inst_mutex) CloseHandle(inst_mutex);
  CoUninitialize();
  return rc;
}
