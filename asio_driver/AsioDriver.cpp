#include <windows.h>
#include <objbase.h>
#include <mmsystem.h>
#include <avrt.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

#include "asio/AsioShared.h"

namespace {

const CLSID CLSID_AES67BridgeAsio = {
    0xa67b0a51, 0x3c2e, 0x4f7d, {0x9b, 0x8a, 0x67, 0xae, 0x5b, 0x1d, 0x67, 0x01}};
constexpr wchar_t kClsidStr[] = L"{A67B0A51-3C2E-4F7D-9B8A-67AE5B1D6701}";
constexpr wchar_t kDriverName[] = L"AES67 Bridge";
constexpr char kDriverNameA[] = "AES67 Bridge";
constexpr wchar_t kTrayWndClass[] = L"AES67BridgeTrayWnd";
constexpr UINT kMsgOpenUi = WM_APP + 2;
constexpr wchar_t kClientSemName[] = L"Local\\AES67BridgeASIO.v1.client";

HINSTANCE g_hinst = nullptr;
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

using aes67asio::kChannels;
using aes67asio::kRingMask;
using aes67asio::kSafety;

void SplitU64(int64_t v, unsigned long* hi, unsigned long* lo) {
  *hi = (unsigned long)((uint64_t)v >> 32);
  *lo = (unsigned long)((uint64_t)v & 0xffffffffu);
}

class AsioDriver : public IASIO {
 public:
  AsioDriver() { ++g_objects; }
  virtual ~AsioDriver() {
    stop();
    disposeBuffers();
    Detach();
    if (done_evt_) CloseHandle(done_evt_);
    --g_objects;
  }

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == CLSID_AES67BridgeAsio) {
      *ppv = static_cast<IASIO*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    const long r = --refs_;
    if (r == 0) delete this;
    return (ULONG)r;
  }

  ASIOBool init(void* ) override {
    if (shm_) return ASIOTrue;
    if (!client_sem_) {
      HANDLE sem = CreateSemaphoreW(nullptr, 1, 1, kClientSemName);
      if (!sem || WaitForSingleObject(sem, 0) != WAIT_OBJECT_0) {
        if (sem) CloseHandle(sem);
        SetError("AES67 Bridge ASIO is in use by another application.");
        return ASIOFalse;
      }
      client_sem_ = sem;
    }
    map_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, aes67asio::kShmName);
    if (!map_) {
      SetError("AES67 Bridge app is not running. Start AES67Bridge.exe first.");
      return ASIOFalse;
    }
    shm_ = static_cast<aes67asio::Shared*>(
        MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(aes67asio::Shared)));
    if (!shm_ || shm_->magic != aes67asio::kMagic ||
        shm_->version != aes67asio::kVersion) {
      SetError("AES67 Bridge version mismatch (reinstall).");
      Detach();
      return ASIOFalse;
    }
    evt_ = OpenEventW(SYNCHRONIZE, FALSE, aes67asio::kTickEventName);
    if (!evt_) {
      SetError("AES67 Bridge clock event unavailable.");
      Detach();
      return ASIOFalse;
    }
    return ASIOTrue;
  }

  void getDriverName(char* name) override { strcpy(name, kDriverNameA); }
  long getDriverVersion() override { return 1; }
  void getErrorMessage(char* s) override { strcpy(s, err_); }

  ASIOError start() override {
    if (!shm_ || !cb_ || buf_size_ <= 0) return ASE_NotPresent;
    if (running_.load()) return ASE_OK;
    for (auto& o : out_) memset(o.buf.data(), 0, o.buf.size() * sizeof(int32_t));
    sample_pos_ = 0;
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* base = strrchr(exe, '\\');
    base = base ? base + 1 : exe;
    memset(shm_->client_name, 0, sizeof(shm_->client_name));
    strncpy(shm_->client_name, base, sizeof(shm_->client_name) - 1);
    shm_->client_pid.store(GetCurrentProcessId());
    shm_->client_buffer.store((uint32_t)buf_size_);
    shm_->to_net_end.store(0);
    shm_->client_active.store(1);
    if (!done_evt_) done_evt_ = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    ResetEvent(done_evt_);
    running_ = true;
    try {
      thread_ = std::thread([this] { Run(); });
    } catch (...) {
      running_ = false;
      SetEvent(done_evt_);
      return ASE_HWMalfunction;
    }
    return ASE_OK;
  }

  ASIOError stop() override {
    if (!running_.exchange(false)) return ASE_OK;
    if (thread_.joinable()) {
      if (std::this_thread::get_id() == thread_.get_id()) {
        thread_.detach();
      } else if (WaitForSingleObject(done_evt_, 2000) == WAIT_OBJECT_0) {
        thread_.join();
      } else {
        thread_.detach();
      }
    }
    if (shm_) shm_->client_active.store(0);
    return ASE_OK;
  }

  ASIOError getChannels(long* numIn, long* numOut) override {
    if (!shm_) return ASE_NotPresent;
    *numIn = kChannels;
    *numOut = kChannels;
    return ASE_OK;
  }

  ASIOError getLatencies(long* inLat, long* outLat) override {
    if (!shm_) return ASE_NotPresent;
    const long b = buf_size_ > 0 ? buf_size_ : (long)Preferred();
    *inLat = b;
    *outLat = b + (long)kSafety;
    return ASE_OK;
  }

  ASIOError getBufferSize(long* minSize, long* maxSize, long* preferred,
                          long* granularity) override {
    if (!shm_) return ASE_NotPresent;
    *minSize = (long)aes67asio::kMinBuffer;
    *maxSize = (long)aes67asio::kMaxBuffer;
    *preferred = (long)Preferred();
    *granularity = -1;
    return ASE_OK;
  }

  ASIOError canSampleRate(ASIOSampleRate rate) override {
    return rate == 48000.0 ? ASE_OK : ASE_NoClock;
  }
  ASIOError getSampleRate(ASIOSampleRate* rate) override {
    *rate = 48000.0;
    return ASE_OK;
  }
  ASIOError setSampleRate(ASIOSampleRate rate) override {
    return (rate == 48000.0 || rate == 0.0) ? ASE_OK : ASE_NoClock;
  }

  ASIOError getClockSources(ASIOClockSource* clocks, long* num) override {
    clocks[0].index = 0;
    clocks[0].associatedChannel = -1;
    clocks[0].associatedGroup = -1;
    clocks[0].isCurrentSource = ASIOTrue;
    strcpy(clocks[0].name, "PTP (AES67)");
    *num = 1;
    return ASE_OK;
  }
  ASIOError setClockSource(long ref) override {
    return ref == 0 ? ASE_OK : ASE_InvalidMode;
  }

  ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override {
    std::lock_guard<std::mutex> lk(pos_mtx_);
    SplitU64(sample_pos_, &sPos->hi, &sPos->lo);
    SplitU64(sys_time_ns_, &tStamp->hi, &tStamp->lo);
    return ASE_OK;
  }

  ASIOError getChannelInfo(ASIOChannelInfo* info) override {
    if (info->channel < 0 || info->channel >= kChannels) return ASE_InvalidParameter;
    info->isActive = ASIOFalse;
    for (const auto& c : info->isInput ? in_ : out_)
      if (c.num == info->channel) info->isActive = ASIOTrue;
    info->channelGroup = 0;
    info->type = ASIOSTInt32LSB;
    snprintf(info->name, sizeof(info->name), "AES67 %s %ld",
             info->isInput ? "In" : "Out", info->channel + 1);
    return ASE_OK;
  }

  ASIOError createBuffers(ASIOBufferInfo* infos, long num, long bufferSize,
                          ASIOCallbacks* callbacks) override {
    if (!shm_) return ASE_NotPresent;
    if (running_.load()) return ASE_InvalidMode;
    if (bufferSize < 16 || bufferSize > (long)aes67asio::kMaxBuffer || !callbacks)
      return ASE_InvalidParameter;
    disposeBuffers();
    uint32_t in_mask = 0, out_mask = 0;
    for (long i = 0; i < num; ++i) {
      ASIOBufferInfo& bi = infos[i];
      if (bi.channelNum < 0 || bi.channelNum >= kChannels) {
        disposeBuffers();
        return ASE_InvalidParameter;
      }
      auto& list = bi.isInput ? in_ : out_;
      list.push_back({(int)bi.channelNum, std::vector<int32_t>((size_t)bufferSize * 2, 0)});
      (bi.isInput ? in_mask : out_mask) |= 1u << bi.channelNum;
    }
    long ii = 0, oi = 0;
    for (long i = 0; i < num; ++i) {
      ASIOBufferInfo& bi = infos[i];
      auto& ch = bi.isInput ? in_[ii++] : out_[oi++];
      bi.buffers[0] = ch.buf.data();
      bi.buffers[1] = ch.buf.data() + bufferSize;
    }
    buf_size_ = bufferSize;
    cb_ = callbacks;
    time_info_ = callbacks->asioMessage &&
                 callbacks->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
    memset(&asio_time_, 0, sizeof(asio_time_));
    shm_->in_active_mask.store(in_mask);
    shm_->out_active_mask.store(out_mask);
    return ASE_OK;
  }

  ASIOError disposeBuffers() override {
    stop();
    if (done_evt_ && WaitForSingleObject(done_evt_, 1000) != WAIT_OBJECT_0) {
      new std::vector<Chan>(std::move(in_));
      new std::vector<Chan>(std::move(out_));
    }
    in_.clear();
    out_.clear();
    buf_size_ = 0;
    cb_ = nullptr;
    if (shm_) {
      shm_->in_active_mask.store(0);
      shm_->out_active_mask.store(0);
    }
    return ASE_OK;
  }

  ASIOError controlPanel() override {
    if (HWND w = FindWindowExW(HWND_MESSAGE, nullptr, kTrayWndClass, nullptr)) {
      PostMessageW(w, kMsgOpenUi, 0, 0);
      return ASE_OK;
    }
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_hinst, path, MAX_PATH);
    std::wstring exe(path);
    exe = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"AES67Bridge.exe";
    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return ASE_OK;
  }

  ASIOError future(long selector, void* ) override {
    switch (selector) {
      case kAsioCanTimeInfo: return ASE_SUCCESS;
      default: return ASE_NotPresent;
    }
  }

  ASIOError outputReady() override { return ASE_NotPresent; }

 private:
  struct Chan {
    int num;
    std::vector<int32_t> buf;
  };

  void SetError(const char* e) { strncpy(err_, e, sizeof(err_) - 1); }
  void Detach() {
    if (client_sem_) {
      ReleaseSemaphore(client_sem_, 1, nullptr);
      CloseHandle(client_sem_);
      client_sem_ = nullptr;
    }
    if (shm_) UnmapViewOfFile(shm_);
    shm_ = nullptr;
    if (map_) CloseHandle(map_);
    map_ = nullptr;
    if (evt_) CloseHandle(evt_);
    evt_ = nullptr;
  }
  uint32_t Preferred() const {
    return aes67asio::NormalizeBuffer(shm_ ? shm_->preferred_buffer
                                           : aes67asio::kDefaultBuffer);
  }

  void Run() {
    try {
      RunLoop();
    } catch (...) {
    }
    SetEvent(done_evt_);
  }

  void RunLoop() {
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
    timeBeginPeriod(1);

    const int64_t B = buf_size_;
    LARGE_INTEGER freq, last_qpc, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last_qpc);
    int64_t last_S = shm_->bridge_sac.load(std::memory_order_acquire);
    int64_t next_T = -1;
    int idx = 0;
    bool first = true;

    while (running_.load()) {
      const DWORD w = WaitForSingleObject(evt_, 20);
      if (!running_.load()) break;
      QueryPerformanceCounter(&now);
      int64_t S;
      bool free_run = false;
      if (w == WAIT_OBJECT_0 && shm_->bridge_alive.load()) {
        S = shm_->bridge_sac.load(std::memory_order_acquire);
        last_S = S;
        last_qpc = now;
      } else {
        S = last_S + (now.QuadPart - last_qpc.QuadPart) * 48000 / freq.QuadPart;
        free_run = true;
      }
      if (next_T < 0 || S - next_T > 8 * B || next_T - S > 8 * B) {
        next_T = (S / B + 1) * B;
        first = true;
      }
      while (running_.load() && S >= next_T) {
        Switch(next_T, idx, free_run, first);
        idx ^= 1;
        first = false;
        next_T += B;
      }
    }
    timeEndPeriod(1);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  }

  void Switch(int64_t T, int idx, bool free_run, bool first) {
    const int64_t B = buf_size_;
    if (!first) {
      const int64_t base = T + (int64_t)kSafety;
      for (const auto& o : out_) {
        float* ring = shm_->to_net[o.num];
        const int32_t* src = o.buf.data() + (size_t)(idx ^ 1) * B;
        for (int64_t i = 0; i < B; ++i)
          ring[(base + i) & kRingMask] = (float)(src[i] * (1.0 / 2147483648.0));
      }
      shm_->to_net_end.store(base + B, std::memory_order_release);
    }
    for (auto& in : in_) {
      int32_t* dst = in.buf.data() + (size_t)idx * B;
      if (free_run) {
        memset(dst, 0, (size_t)B * sizeof(int32_t));
        continue;
      }
      const float* ring = shm_->from_net[in.num];
      for (int64_t i = 0; i < B; ++i) {
        double v = ring[(T - B + i) & kRingMask];
        v = v > 1.0 ? 1.0 : (v < -1.0 ? -1.0 : v);
        dst[i] = (int32_t)(v * 2147483647.0);
      }
    }
    if (!running_.load()) return;
    {
      std::lock_guard<std::mutex> lk(pos_mtx_);
      sample_pos_ = T - B;
      sys_time_ns_ = (int64_t)timeGetTime() * 1000000;
    }
    if (time_info_ && cb_->bufferSwitchTimeInfo) {
      AsioTimeInfo& ti = asio_time_.timeInfo;
      ti.speed = 1.0;
      ti.sampleRate = 48000.0;
      SplitU64(T - B, &ti.samplePosition.hi, &ti.samplePosition.lo);
      SplitU64((int64_t)timeGetTime() * 1000000, &ti.systemTime.hi, &ti.systemTime.lo);
      ti.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
      cb_->bufferSwitchTimeInfo(&asio_time_, idx, ASIOTrue);
    } else {
      cb_->bufferSwitch(idx, ASIOTrue);
    }
  }

  std::atomic<long> refs_{1};
  HANDLE map_ = nullptr;
  HANDLE evt_ = nullptr;
  aes67asio::Shared* shm_ = nullptr;
  char err_[160] = "";

  long buf_size_ = 0;
  ASIOCallbacks* cb_ = nullptr;
  bool time_info_ = false;
  ASIOTime asio_time_{};
  std::vector<Chan> in_, out_;

  std::thread thread_;
  std::atomic<bool> running_{false};
  HANDLE done_evt_ = nullptr;
  HANDLE client_sem_ = nullptr;
  std::mutex pos_mtx_;
  int64_t sample_pos_ = 0;
  int64_t sys_time_ns_ = 0;
};

class Factory : public IClassFactory {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
      *ppv = static_cast<IClassFactory*>(this);
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return 2; }
  STDMETHODIMP_(ULONG) Release() override { return 1; }
  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
    if (outer) return CLASS_E_NOAGGREGATION;
    auto* d = new (std::nothrow) AsioDriver();
    if (!d) return E_OUTOFMEMORY;
    const HRESULT hr = d->QueryInterface(riid, ppv);
    d->Release();
    return hr;
  }
  STDMETHODIMP LockServer(BOOL lock) override {
    lock ? ++g_locks : --g_locks;
    return S_OK;
  }
};
Factory g_factory;

bool SetKey(HKEY root, const std::wstring& path, const wchar_t* name,
            const std::wstring& value) {
  HKEY k;
  if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k,
                      nullptr) != ERROR_SUCCESS)
    return false;
  const LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(),
                                (DWORD)((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(k);
  return r == ERROR_SUCCESS;
}

}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_hinst = hinst;
    DisableThreadLibraryCalls(hinst);
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
  if (clsid != CLSID_AES67BridgeAsio) return CLASS_E_CLASSNOTAVAILABLE;
  return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() {
  return (g_objects.load() == 0 && g_locks.load() == 0) ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(g_hinst, path, MAX_PATH);
  const std::wstring clsid_key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidStr;
  const std::wstring asio_key = std::wstring(L"SOFTWARE\\ASIO\\") + kDriverName;
  bool ok = SetKey(HKEY_LOCAL_MACHINE, clsid_key, nullptr, L"AES67 Bridge ASIO Driver") &&
            SetKey(HKEY_LOCAL_MACHINE, clsid_key + L"\\InprocServer32", nullptr, path) &&
            SetKey(HKEY_LOCAL_MACHINE, clsid_key + L"\\InprocServer32",
                   L"ThreadingModel", L"Apartment") &&
            SetKey(HKEY_LOCAL_MACHINE, asio_key, L"CLSID", kClsidStr) &&
            SetKey(HKEY_LOCAL_MACHINE, asio_key, L"Description", kDriverName);
  return ok ? S_OK : E_ACCESSDENIED;
}

STDAPI DllUnregisterServer() {
  RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                 (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidStr).c_str());
  RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\ASIO\\") + kDriverName).c_str());
  return S_OK;
}
