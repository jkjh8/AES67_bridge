#pragma once

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <memory>

#include "app/Controller.h"
#include "common/Types.h"
#include "ui/WebUi.h"

namespace aes67 {

inline constexpr wchar_t kTrayWndClass[] = L"AES67BridgeTrayWnd";
inline constexpr UINT kMsgOpenUi = WM_APP + 2;

class TrayApp {
 public:
  TrayApp(HINSTANCE hinst, std::filesystem::path config_path, AppConfig cfg);
  ~TrayApp();

  bool Create();
  void RequestOpenUi();
  int RunMessageLoop();

 private:
  static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
  LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

  void ShowContextMenu();
  void UpdateTooltip();
  void OpenUi();
  void OnUiMessage(const std::string& json);
  void OnNetworkChanged();

  HINSTANCE hinst_ = nullptr;
  HWND hwnd_ = nullptr;
  NOTIFYICONDATAW nid_{};
  bool nid_added_ = false;
  HANDLE if_notify_ = nullptr;
  HANDLE addr_notify_ = nullptr;

  std::unique_ptr<Controller> ctl_;
  std::unique_ptr<WebUi> ui_;
};

}
