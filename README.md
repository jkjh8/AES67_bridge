# AES67 Bridge

Windows system-tray application that connects Windows audio (WASAPI) and DAWs
(virtual ASIO driver, 32 in / 32 out) to AES67 networks.

- AES67 transmit / receive (L24, 48 kHz, 1 ms), multiple streams, SAP discovery
- PTP (IEEE 1588-2008) slave
- Crosspoint routing matrix
- Local web UI (WebView2)

## Install

Run `AES67Bridge-Setup-<version>.exe`. Requires Windows 10 1809+ (x64) and the
Microsoft Edge WebView2 Runtime.

## Build

Visual Studio 2022 (C++), CMake 3.21+, Inno Setup 6:

```
powershell -ExecutionPolicy Bypass -File scripts\build-installer.ps1
```

## License

The virtual ASIO driver (`asio_driver/`) uses the Steinberg ASIO SDK under
the GNU GPL v3 and is licensed under GPL v3 (`asio_driver/LICENSE`).
Third-party notices: `THIRD_PARTY_NOTICES.txt`.
