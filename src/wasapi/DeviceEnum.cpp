#include "wasapi/DeviceEnum.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include "wasapi/WasapiUtil.h"

namespace aes67 {

std::vector<AudioDevice> EnumAudioDevices(bool render) {
  std::vector<AudioDevice> out;
  const HRESULT hrco = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IMMDeviceEnumerator* enumr = nullptr;
  IMMDeviceCollection* coll = nullptr;
  const EDataFlow flow = render ? eRender : eCapture;
  do {
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enumr)))
      break;
    std::wstring def_id;
    IMMDevice* def = nullptr;
    if (SUCCEEDED(enumr->GetDefaultAudioEndpoint(flow, eConsole, &def))) {
      LPWSTR id = nullptr;
      if (SUCCEEDED(def->GetId(&id))) {
        def_id = id;
        CoTaskMemFree(id);
      }
      def->Release();
    }
    if (FAILED(enumr->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll))) break;
    UINT n = 0;
    coll->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
      IMMDevice* dev = nullptr;
      if (FAILED(coll->Item(i, &dev))) continue;
      AudioDevice d;
      LPWSTR id = nullptr;
      if (SUCCEEDED(dev->GetId(&id))) {
        d.id = Narrow(id);
        d.is_default = (def_id == id);
        CoTaskMemFree(id);
      }
      IPropertyStore* props = nullptr;
      if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) &&
            v.vt == VT_LPWSTR)
          d.name = Narrow(v.pwszVal);
        PropVariantClear(&v);
        props->Release();
      }
      IAudioClient* client = nullptr;
      if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  (void**)&client))) {
        WAVEFORMATEX* fmt = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&fmt))) {
          d.channels = fmt->nChannels;
          d.rate = (int)fmt->nSamplesPerSec;
          CoTaskMemFree(fmt);
        }
        client->Release();
      }
      dev->Release();
      out.push_back(std::move(d));
    }
  } while (0);
  if (coll) coll->Release();
  if (enumr) enumr->Release();
  if (SUCCEEDED(hrco)) CoUninitialize();
  return out;
}

}
