#include "common/Autostart.h"

#include <windows.h>

#include <string>

namespace aes67 {
namespace {
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"AES67 Bridge";
}

bool AutostartEnabled() {
  return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr,
                      nullptr) == ERROR_SUCCESS;
}

bool SetAutostart(bool on) {
  HKEY key;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                      nullptr) != ERROR_SUCCESS)
    return false;
  LSTATUS rc;
  if (on) {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring cmd = L"\"" + std::wstring(exe, n) + L"\" --tray";
    rc = RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                        (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
  } else {
    rc = RegDeleteValueW(key, kRunValue);
    if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
  }
  RegCloseKey(key);
  return rc == ERROR_SUCCESS;
}

}
