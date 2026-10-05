#include "wasapi/WasapiRenderSink.h"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <future>
#include <thread>
#include <vector>

#include "common/Log.h"
#include "common/SpscRing.h"
#include "wasapi/WasapiUtil.h"

namespace aes67 {
namespace {
template <class T>
void SafeRelease(T*& p) {
  if (p) { p->Release(); p = nullptr; }
}
const GUID kSubFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
}

struct WasapiRenderSink::Impl {
  IMMDeviceEnumerator* enumr = nullptr;
  IMMDevice* dev = nullptr;
  IAudioClient* client = nullptr;
  IAudioRenderClient* render = nullptr;
  WAVEFORMATEX* mixfmt = nullptr;
  HANDLE evt = nullptr;

  std::thread thread;
  std::atomic<bool> running{false};
  std::promise<bool> ready;
  std::string err;
  std::string device_id;
  std::string name;

  FloatSpsc fifo;
  uint32_t rate = 48000;
  uint32_t ch = 2;
  bool is_float = true;
  int bytes_per_sample = 4;
  UINT32 buffer_frames = 0;

  void Run();
  void WriteFrames(BYTE* dst, UINT32 frames);
  void Cleanup() {
    SafeRelease(render);
    SafeRelease(client);
    SafeRelease(dev);
    SafeRelease(enumr);
    if (mixfmt) { CoTaskMemFree(mixfmt); mixfmt = nullptr; }
    if (evt) { CloseHandle(evt); evt = nullptr; }
  }
};

void WasapiRenderSink::Impl::WriteFrames(BYTE* dst, UINT32 frames) {
  static thread_local std::vector<float> tmp;
  const size_t need = (size_t)frames * ch;
  tmp.resize(need);
  const size_t got = fifo.pop(tmp.data(), need);
  for (size_t i = got; i < need; ++i) tmp[i] = 0.0f;

  if (is_float) {
    memcpy(dst, tmp.data(), need * sizeof(float));
  } else if (bytes_per_sample == 2) {
    int16_t* d = reinterpret_cast<int16_t*>(dst);
    for (size_t i = 0; i < need; ++i) {
      float v = tmp[i]; if (v > 1) v = 1; else if (v < -1) v = -1;
      d[i] = (int16_t)(v * 32767.0f);
    }
  } else {
    int32_t* d = reinterpret_cast<int32_t*>(dst);
    for (size_t i = 0; i < need; ++i) {
      double v = tmp[i]; if (v > 1) v = 1; else if (v < -1) v = -1;
      d[i] = (int32_t)(v * 2147483647.0);
    }
  }
}

void WasapiRenderSink::Impl::Run() {
  bool ok = false;
  const HRESULT hrco = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  do {
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enumr))) {
      err = "CoCreateInstance failed"; break;
    }
    if (FAILED(OpenEndpoint(enumr, device_id, eRender, &dev))) {
      err = "render endpoint not found"; break;
    }
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
      PROPVARIANT v;
      PropVariantInit(&v);
      if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) &&
          v.vt == VT_LPWSTR)
        name = Narrow(v.pwszVal);
      PropVariantClear(&v);
      props->Release();
    }
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             (void**)&client))) {
      err = "Activate failed"; break;
    }
    if (FAILED(client->GetMixFormat(&mixfmt))) { err = "GetMixFormat failed"; break; }
    rate = mixfmt->nSamplesPerSec;
    ch = mixfmt->nChannels;
    bytes_per_sample = mixfmt->wBitsPerSample / 8;
    is_float = (mixfmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (mixfmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
      is_float = IsEqualGUID(
          reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixfmt)->SubFormat, kSubFloat);

    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mixfmt,
                                  nullptr))) {
      err = "Initialize failed"; break;
    }
    evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt || FAILED(client->SetEventHandle(evt))) { err = "SetEventHandle failed"; break; }
    if (FAILED(client->GetBufferSize(&buffer_frames))) { err = "GetBufferSize failed"; break; }
    if (FAILED(client->GetService(__uuidof(IAudioRenderClient), (void**)&render))) {
      err = "GetService(render) failed"; break;
    }
    fifo.init((size_t)rate * ch);
    BYTE* buf = nullptr;
    if (SUCCEEDED(render->GetBuffer(buffer_frames, &buf)))
      render->ReleaseBuffer(buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(client->Start())) { err = "Start failed"; break; }
    ok = true;
  } while (0);

  ready.set_value(ok);
  if (!ok) { Cleanup(); if (SUCCEEDED(hrco)) CoUninitialize(); return; }

  LOGI("wasapi render: '%s' %u Hz, %u ch, %s, buf=%u", name.c_str(), rate, ch,
       is_float ? "float" : "pcm", buffer_frames);

  DWORD task = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
  if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);

  const size_t hi_floats = (size_t)rate * ch * 120 / 1000;
  const size_t tgt_floats = (size_t)rate * ch * 20 / 1000;

  while (running.load()) {
    if (WaitForSingleObject(evt, 200) != WAIT_OBJECT_0) continue;
    if (fifo.size() > hi_floats) fifo.drop(fifo.size() - tgt_floats);
    UINT32 padding = 0;
    if (FAILED(client->GetCurrentPadding(&padding))) break;
    const UINT32 avail = buffer_frames - padding;
    if (avail == 0) continue;
    BYTE* buf = nullptr;
    if (FAILED(render->GetBuffer(avail, &buf))) break;
    WriteFrames(buf, avail);
    render->ReleaseBuffer(avail, 0);
  }

  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  client->Stop();
  Cleanup();
  if (SUCCEEDED(hrco)) CoUninitialize();
}

WasapiRenderSink::WasapiRenderSink() : impl_(std::make_unique<Impl>()) {}
WasapiRenderSink::~WasapiRenderSink() { Stop(); }

bool WasapiRenderSink::Start(const std::string& device_id, std::string* err) {
  if (impl_->running.load()) Stop();
  impl_->device_id = device_id;
  impl_->running = true;
  impl_->ready = std::promise<bool>();
  auto fut = impl_->ready.get_future();
  impl_->thread = std::thread([this] { impl_->Run(); });
  const bool ok = fut.get();
  if (!ok) {
    impl_->running = false;
    if (impl_->thread.joinable()) impl_->thread.join();
    if (err) *err = impl_->err;
  }
  return ok;
}

void WasapiRenderSink::Stop() {
  if (!impl_->running.exchange(false)) return;
  if (impl_->thread.joinable()) impl_->thread.join();
}

void WasapiRenderSink::Push(const float* interleaved, uint32_t frames) {
  impl_->fifo.push(interleaved, (size_t)frames * impl_->ch);
}

uint32_t WasapiRenderSink::sampleRate() const { return impl_->rate; }
uint32_t WasapiRenderSink::channels() const { return impl_->ch; }
const std::string& WasapiRenderSink::deviceName() const { return impl_->name; }
size_t WasapiRenderSink::fifoFrames() const {
  return impl_->ch ? impl_->fifo.size() / impl_->ch : 0;
}

}
