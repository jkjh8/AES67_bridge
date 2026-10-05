#include "ui/WebUi.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <wrl.h>

#include <string_view>
#include <vector>

#include <WebView2.h>

#include "common/Log.h"
#include "ui/resource.h"
#include "wasapi/WasapiUtil.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace aes67 {
namespace {
constexpr wchar_t kCls[] = L"AES67BridgeWebUi";
constexpr wchar_t kOrigin[] = L"https://aes67bridge.local/";
constexpr wchar_t kPageUrl[] = L"https://aes67bridge.local/index.html";
constexpr wchar_t kHeaders[] =
    L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store";

std::wstring UserDataFolder() {
  PWSTR p = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
    dir = p;
    CoTaskMemFree(p);
  }
  dir += L"\\AES67Bridge\\WebView2";
  SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
  return dir;
}

constexpr wchar_t kTextHeaders[] =
    L"Content-Type: text/plain; charset=utf-8\r\nCache-Control: no-store";

std::string_view ResourceBytes(HINSTANCE hinst, int id) {
  HRSRC r = FindResourceW(hinst, MAKEINTRESOURCEW(id), RT_RCDATA);
  if (!r) return "resource missing";
  HGLOBAL g = LoadResource(hinst, r);
  return {static_cast<const char*>(LockResource(g)), SizeofResource(hinst, r)};
}

int ResourceForUri(const wchar_t* uri) {
  const std::wstring_view u = uri ? uri : L"";
  const std::wstring_view origin = kOrigin;
  const std::wstring_view path = u.substr(0, origin.size()) == origin ? u.substr(origin.size()) : u;
  if (path == L"LICENSE.txt") return IDR_LICENSE;
  if (path == L"THIRD_PARTY_NOTICES.txt") return IDR_NOTICES;
  return IDR_WEBUI;
}
}

struct WebUi::Impl {
  HINSTANCE hinst = nullptr;
  std::function<void(const std::string&)> on_message;
  HWND hwnd = nullptr;
  ComPtr<ICoreWebView2Environment> env;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;
  bool ready = false;
  std::vector<std::wstring> pending;

  static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
  void Create();
  void InitWebView();
  void Resize() {
    if (!controller) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    controller->put_Bounds(rc);
  }
};

LRESULT CALLBACK WebUi::Impl::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    static_cast<Impl*>(cs->lpCreateParams)->hwnd = h;
  }
  auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (!self) return DefWindowProcW(h, m, w, l);
  switch (m) {
    case WM_SIZE:
      self->Resize();
      return 0;
    case WM_CLOSE:
      ShowWindow(h, SW_HIDE);
      return 0;
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(l);
      const UINT dpi = GetDpiForWindow(h);
      mm->ptMinTrackSize = {MulDiv(640, dpi, 96), MulDiv(520, dpi, 96)};
      return 0;
    }
    case WM_DPICHANGED: {
      const RECT* rc = reinterpret_cast<const RECT*>(l);
      SetWindowPos(h, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_DESTROY:
      if (self->controller) self->controller->Close();
      self->controller.Reset();
      self->webview.Reset();
      self->ready = false;
      self->hwnd = nullptr;
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

void WebUi::Impl::Create() {
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &Impl::WndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = kCls;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(0xf5, 0xf6, 0xf8));
    wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = (HICON)LoadImageW(hinst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);
    registered = true;
  }
  const UINT dpi = GetDpiForSystem();
  const int w = MulDiv(860, dpi, 96), hgt = MulDiv(820, dpi, 96);
  CreateWindowExW(0, kCls, L"AES67 Bridge", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                  CW_USEDEFAULT, w, hgt, nullptr, nullptr, hinst, this);
  LOGI("webui: window %s (%lu)", hwnd ? "created" : "FAILED", hwnd ? 0 : GetLastError());
  if (hwnd) InitWebView();
}

void WebUi::Impl::InitWebView() {
  const std::wstring udf = UserDataFolder();
  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, udf.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this](HRESULT res, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(res) || !env) {
              LOGE("webui: environment failed 0x%08lx", (unsigned long)res);
              return S_OK;
            }
            this->env = env;
            env->CreateCoreWebView2Controller(
                hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this](HRESULT res2, ICoreWebView2Controller* c) -> HRESULT {
                      if (FAILED(res2) || !c || !hwnd) {
                        LOGE("webui: controller failed 0x%08lx", (unsigned long)res2);
                        return S_OK;
                      }
                      controller = c;
                      controller->get_CoreWebView2(&webview);
                      ComPtr<ICoreWebView2Settings> s;
                      if (SUCCEEDED(webview->get_Settings(&s))) {
                        s->put_IsStatusBarEnabled(FALSE);
                        s->put_IsZoomControlEnabled(FALSE);
#ifdef NDEBUG
                        s->put_AreDevToolsEnabled(FALSE);
                        s->put_AreDefaultContextMenusEnabled(FALSE);
#endif
                        ComPtr<ICoreWebView2Settings3> s3;
                        if (SUCCEEDED(s.As(&s3)))
                          s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                      }
                      EventRegistrationToken tok;
                      webview->add_WebMessageReceived(
                          Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                              [this](ICoreWebView2*,
                                     ICoreWebView2WebMessageReceivedEventArgs* a)
                                  -> HRESULT {
                                LPWSTR msg = nullptr;
                                if (SUCCEEDED(a->TryGetWebMessageAsString(&msg)) && msg) {
                                  const std::string text = Narrow(msg);
                                  CoTaskMemFree(msg);
                                  if (on_message) on_message(text);
                                }
                                return S_OK;
                              })
                              .Get(),
                          &tok);
                      webview->add_NavigationCompleted(
                          Callback<ICoreWebView2NavigationCompletedEventHandler>(
                              [this](ICoreWebView2*,
                                     ICoreWebView2NavigationCompletedEventArgs*)
                                  -> HRESULT {
                                ready = true;
                                for (const auto& p : pending)
                                  webview->PostWebMessageAsJson(p.c_str());
                                pending.clear();
                                return S_OK;
                              })
                              .Get(),
                          &tok);
                      const std::wstring filter = std::wstring(kOrigin) + L"*";
                      webview->AddWebResourceRequestedFilter(
                          filter.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                      webview->add_WebResourceRequested(
                          Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                              [this](ICoreWebView2*,
                                     ICoreWebView2WebResourceRequestedEventArgs* a)
                                  -> HRESULT {
                                ComPtr<ICoreWebView2WebResourceRequest> req;
                                LPWSTR uri = nullptr;
                                if (SUCCEEDED(a->get_Request(&req))) req->get_Uri(&uri);
                                const int id = ResourceForUri(uri);
                                if (uri) CoTaskMemFree(uri);
                                const std::string_view bytes = ResourceBytes(hinst, id);
                                ComPtr<IStream> body;
                                body.Attach(SHCreateMemStream(
                                    reinterpret_cast<const BYTE*>(bytes.data()),
                                    (UINT)bytes.size()));
                                ComPtr<ICoreWebView2WebResourceResponse> resp;
                                if (this->env && SUCCEEDED(this->env->CreateWebResourceResponse(
                                               body.Get(), 200, L"OK",
                                               id == IDR_WEBUI ? kHeaders : kTextHeaders,
                                               &resp)))
                                  a->put_Response(resp.Get());
                                return S_OK;
                              })
                              .Get(),
                          &tok);
                      Resize();
                      LOGI("webui: WebView2 ready");
                      webview->Navigate(kPageUrl);
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          })
          .Get());
  if (FAILED(hr)) {
    LOGE("webui: CreateCoreWebView2Environment failed 0x%08lx", (unsigned long)hr);
    MessageBoxW(hwnd,
                L"Microsoft Edge WebView2 Runtime is required.\n"
                L"Install it from https://developer.microsoft.com/microsoft-edge/webview2/",
                L"AES67 Bridge", MB_OK | MB_ICONWARNING);
  }
}

WebUi::WebUi(HINSTANCE hinst, std::function<void(const std::string&)> on_message)
    : impl_(std::make_unique<Impl>()) {
  impl_->hinst = hinst;
  impl_->on_message = std::move(on_message);
}

WebUi::~WebUi() {
  if (impl_->hwnd) DestroyWindow(impl_->hwnd);
}

void WebUi::Show() {
  if (!impl_->hwnd) impl_->Create();
  if (!impl_->hwnd) return;
  if (IsIconic(impl_->hwnd)) ShowWindow(impl_->hwnd, SW_RESTORE);
  ShowWindow(impl_->hwnd, SW_SHOW);
  SetForegroundWindow(impl_->hwnd);
}

bool WebUi::visible() const {
  return impl_->hwnd && IsWindowVisible(impl_->hwnd) && !IsIconic(impl_->hwnd);
}

void WebUi::Post(const std::string& json) {
  if (!impl_->hwnd) return;
  std::wstring w = Widen(json);
  if (impl_->ready && impl_->webview)
    impl_->webview->PostWebMessageAsJson(w.c_str());
  else if (impl_->pending.size() < 16)
    impl_->pending.push_back(std::move(w));
}

}
