#include "ui/TrayApp.h"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <shellapi.h>

#include <string>

#include "common/Autostart.h"
#include "common/Log.h"
#include "ui/resource.h"

namespace aes67 {
namespace {
constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kTrayIconId = 1;
constexpr UINT_PTR kStatusTimerId = 1;
constexpr UINT_PTR kNetTimerId = 2;
constexpr UINT kMsgNetChanged = WM_APP + 3;

void CALLBACK OnIfChange(PVOID ctx, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
  PostMessageW(static_cast<HWND>(ctx), kMsgNetChanged, 0, 0);
}

void CALLBACK OnAddrChange(PVOID ctx, PMIB_UNICASTIPADDRESS_ROW, MIB_NOTIFICATION_TYPE) {
  PostMessageW(static_cast<HWND>(ctx), kMsgNetChanged, 0, 0);
}

enum MenuId : UINT {
  kMenuOpen = 100,
  kMenuAutostart,
  kMenuExit,
};
}

TrayApp::TrayApp(HINSTANCE hinst, std::filesystem::path config_path, AppConfig cfg)
    : hinst_(hinst),
      ctl_(std::make_unique<Controller>(std::move(config_path), std::move(cfg))) {}

TrayApp::~TrayApp() {
  if (nid_added_) {
    Shell_NotifyIconW(NIM_DELETE, &nid_);
    nid_added_ = false;
  }
}

bool TrayApp::Create() {
  HICON app_icon_big = (HICON)LoadImageW(hinst_, MAKEINTRESOURCEW(IDI_APPICON),
                                         IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
  HICON app_icon_small = (HICON)LoadImageW(
      hinst_, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
  if (!app_icon_small) app_icon_small = app_icon_big;

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &TrayApp::WndProcThunk;
  wc.hInstance = hinst_;
  wc.lpszClassName = kTrayWndClass;
  wc.hIcon = app_icon_big;
  wc.hIconSm = app_icon_small;
  if (!RegisterClassExW(&wc)) {
    LOGE("tray: RegisterClassEx failed (%lu)", GetLastError());
    return false;
  }

  hwnd_ = CreateWindowExW(0, kTrayWndClass, L"AES67 Bridge", 0, 0, 0, 0, 0,
                          HWND_MESSAGE, nullptr, hinst_, this);
  if (!hwnd_) {
    LOGE("tray: CreateWindow failed (%lu)", GetLastError());
    return false;
  }

  nid_.cbSize = sizeof(nid_);
  nid_.hWnd = hwnd_;
  nid_.uID = kTrayIconId;
  nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid_.uCallbackMessage = kTrayCallback;
  nid_.hIcon = app_icon_small ? app_icon_small : LoadIconW(nullptr, IDI_APPLICATION);
  wcsncpy_s(nid_.szTip, L"AES67 Bridge", _TRUNCATE);
  if (!Shell_NotifyIconW(NIM_ADD, &nid_)) {
    LOGE("tray: Shell_NotifyIcon(ADD) failed (%lu)", GetLastError());
    return false;
  }
  nid_added_ = true;
  LOGI("tray: icon added");

  if (AutostartEnabled()) SetAutostart(true);
  ctl_->Start();
  ui_ = std::make_unique<WebUi>(hinst_, [this](const std::string& m) { OnUiMessage(m); });

  SetTimer(hwnd_, kStatusTimerId, 1000, nullptr);
  NotifyIpInterfaceChange(AF_INET, &OnIfChange, hwnd_, FALSE, &if_notify_);
  NotifyUnicastIpAddressChange(AF_INET, &OnAddrChange, hwnd_, FALSE, &addr_notify_);
  return true;
}

void TrayApp::RequestOpenUi() {
  if (hwnd_) PostMessageW(hwnd_, kMsgOpenUi, 0, 0);
}

int TrayApp::RunMessageLoop() {
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return (int)msg.wParam;
}

LRESULT CALLBACK TrayApp::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  TrayApp* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    self = static_cast<TrayApp*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (self) return self->WndProc(hwnd, msg, wp, lp);
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case kTrayCallback:
      if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
        ShowContextMenu();
      } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
        OpenUi();
      }
      return 0;

    case kMsgOpenUi:
      OpenUi();
      return 0;

    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case kMenuOpen: OpenUi(); return 0;
        case kMenuAutostart: {
          const bool on = !AutostartEnabled();
          if (SetAutostart(on))
            LOGI("tray: start with Windows -> %s", on ? "on" : "off");
          else
            LOGE("tray: start with Windows change failed");
          return 0;
        }
        case kMenuExit: DestroyWindow(hwnd); return 0;
      }
      return 0;

    case kMsgNetChanged:
      SetTimer(hwnd, kNetTimerId, 1500, nullptr);
      return 0;

    case WM_TIMER:
      if (wp == kNetTimerId) {
        KillTimer(hwnd, kNetTimerId);
        OnNetworkChanged();
      } else if (wp == kStatusTimerId) {
        ctl_->Tick();
        UpdateTooltip();
        if (ui_ && ui_->visible()) ui_->Post(ctl_->StateJson());
      }
      return 0;

    case WM_DESTROY:
      if (if_notify_) CancelMibChangeNotify2(if_notify_);
      if (addr_notify_) CancelMibChangeNotify2(addr_notify_);
      if_notify_ = addr_notify_ = nullptr;
      KillTimer(hwnd, kNetTimerId);
      KillTimer(hwnd, kStatusTimerId);
      ui_.reset();
      ctl_->Stop();
      if (nid_added_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        nid_added_ = false;
      }
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void TrayApp::ShowContextMenu() {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING | MF_DEFAULT, kMenuOpen, L"Open AES67 Bridge");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : MF_UNCHECKED), kMenuAutostart,
              L"Start with Windows");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd_);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
  DestroyMenu(menu);
}

void TrayApp::UpdateTooltip() {
  wcsncpy_s(nid_.szTip, ctl_->Tooltip().c_str(), _TRUNCATE);
  if (nid_added_) Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void TrayApp::OpenUi() {
  if (!ui_) return;
  ui_->Show();
  ui_->Post(R"({"type":"show"})");
  ui_->Post(ctl_->StateJson());
}

void TrayApp::OnNetworkChanged() {
  const bool restarted = ctl_->RefreshNetwork();
  if (!ui_) return;
  ui_->Post(ctl_->DevicesJson());
  if (restarted) ui_->Post(ctl_->StateJson());
}

void TrayApp::OnUiMessage(const std::string& json) {
  bool send_devices = false;
  const std::string reply = ctl_->HandleCommand(json, &send_devices);
  if (reply.empty()) return;
  ui_->Post(reply);
  if (send_devices) ui_->Post(ctl_->DevicesJson());
  ui_->Post(ctl_->StateJson());
}

}
