#pragma once

#include <windows.h>
#include <mmdeviceapi.h>

#include <string>

namespace aes67 {

inline std::string Narrow(const wchar_t* w) {
  if (!w || !*w) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

inline std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

inline HRESULT OpenEndpoint(IMMDeviceEnumerator* enumr, const std::string& id,
                            EDataFlow flow, IMMDevice** dev) {
  if (!id.empty() && SUCCEEDED(enumr->GetDevice(Widen(id).c_str(), dev)))
    return S_OK;
  return enumr->GetDefaultAudioEndpoint(flow, eConsole, dev);
}

}
