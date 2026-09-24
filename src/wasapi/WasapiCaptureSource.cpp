#include "wasapi/WasapiCaptureSource.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <future>
#include <thread>
#include <vector>

#include "common/Log.h"
#include "wasapi/WasapiUtil.h"

namespace aes67 {
namespace {
template <class T>
void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}
const GUID kSubFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubPcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
}

struct WasapiCaptureSource::Impl {
  IMMDeviceEnumerator* enumr = nullptr;
  IMMDevice* dev = nullptr;
  IAudioClient* client = nullptr;
  IAudioCaptureClient* capture = nullptr;
  WAVEFORMATEX* mixfmt = nullptr;

  std::thread thread;
  std::atomic<bool> running{false};
  std::promise<bool> ready;
  std::string err;
  std::string device_id;
  bool loopback = true;
  std::string name;

  std::function<void(const float*, uint32_t, uint32_t)> on_frames;
  uint32_t rate = 48000;
  uint32_t ch = 2;
  bool is_float = true;
  int bytes_per_sample = 4;

  void Run();
  void Cleanup() {
    SafeRelease(capture);
    SafeRelease(client);
    SafeRelease(dev);
    SafeRelease(enumr);
    if (mixfmt) {
      CoTaskMemFree(mixfmt);
      mixfmt = nullptr;
    }
  }
};

void WasapiCaptureSource::Impl::Run() {
  bool ok = false;
  const HRESULT hrco = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  do {
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enumr))) {
      err = "CoCreateInstance(MMDeviceEnumerator) failed";
      break;
    }
    if (FAILED(OpenEndpoint(enumr, device_id, loopback ? eRender : eCapture,
                            &dev))) {
      err = "audio endpoint not found";
      break;
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
      err = "IAudioClient Activate failed";
      break;
    }
    if (FAILED(client->GetMixFormat(&mixfmt))) {
      err = "GetMixFormat failed";
      break;
    }
    rate = mixfmt->nSamplesPerSec;
    ch = mixfmt->nChannels;
    bytes_per_sample = mixfmt->wBitsPerSample / 8;
    is_float = (mixfmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (mixfmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
      const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixfmt);
      is_float = IsEqualGUID(ext->SubFormat, kSubFloat);
    }
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
                                  2000000, 0, mixfmt, nullptr))) {
      err = "IAudioClient Initialize failed";
      break;
    }
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient),
                                  (void**)&capture))) {
      err = "GetService(IAudioCaptureClient) failed";
      break;
    }
    if (FAILED(client->Start())) {
      err = "IAudioClient Start failed";
      break;
    }
    ok = true;
  } while (0);

  ready.set_value(ok);
  if (!ok) {
    Cleanup();
    if (SUCCEEDED(hrco)) CoUninitialize();
    return;
  }

  LOGI("wasapi capture: '%s'%s %u Hz, %u ch, %s", name.c_str(),
       loopback ? " (loopback)" : "", rate, ch, is_float ? "float" : "pcm");
  if (rate != 48000)
    LOGW("wasapi capture rate %u != 48000; audio will be off-pitch until ASRC",
         rate);

  std::vector<float> tmp;
  while (running.load()) {
    Sleep(4);
    UINT32 packet = 0;
    if (FAILED(capture->GetNextPacketSize(&packet))) break;
    while (packet > 0 && running.load()) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
        break;
      tmp.resize((size_t)frames * ch);
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        std::fill(tmp.begin(), tmp.end(), 0.0f);
      } else if (is_float) {
        memcpy(tmp.data(), data, (size_t)frames * ch * sizeof(float));
      } else if (bytes_per_sample == 2) {
        const int16_t* s = reinterpret_cast<const int16_t*>(data);
        for (size_t i = 0; i < tmp.size(); ++i) tmp[i] = s[i] * (1.0f / 32768.0f);
      } else if (bytes_per_sample == 4) {
        const int32_t* s = reinterpret_cast<const int32_t*>(data);
        for (size_t i = 0; i < tmp.size(); ++i)
          tmp[i] = (float)(s[i] * (1.0 / 2147483648.0));
      } else {
        std::fill(tmp.begin(), tmp.end(), 0.0f);
      }
      if (on_frames) on_frames(tmp.data(), frames, ch);
      capture->ReleaseBuffer(frames);
      if (FAILED(capture->GetNextPacketSize(&packet))) packet = 0;
    }
  }

  client->Stop();
  Cleanup();
  if (SUCCEEDED(hrco)) CoUninitialize();
}

WasapiCaptureSource::WasapiCaptureSource() : impl_(std::make_unique<Impl>()) {}
WasapiCaptureSource::~WasapiCaptureSource() { Stop(); }

bool WasapiCaptureSource::Start(
    const std::string& device_id, bool loopback,
    std::function<void(const float*, uint32_t, uint32_t)> onFrames,
    std::string* err) {
  if (impl_->running.load()) Stop();
  impl_->device_id = device_id;
  impl_->loopback = loopback;
  impl_->on_frames = std::move(onFrames);
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

void WasapiCaptureSource::Stop() {
  if (!impl_->running.exchange(false)) return;
  if (impl_->thread.joinable()) impl_->thread.join();
}

uint32_t WasapiCaptureSource::sampleRate() const { return impl_->rate; }
uint32_t WasapiCaptureSource::channels() const { return impl_->ch; }
const std::string& WasapiCaptureSource::deviceName() const { return impl_->name; }

}
