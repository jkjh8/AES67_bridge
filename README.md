# AES67 Bridge

AES67 Bridge is a Windows tray application. It connects Windows audio (WASAPI) and DAWs (through its own virtual ASIO driver) to AES67 audio-over-IP networks.

- AES67 transmit and receive: L24, 48 kHz, 1 ms packets, up to 8 channels per stream, multiple streams
- PTP (IEEE 1588-2008) slave, following the grandmaster on the network
- SAP/SDP announcement of transmit streams and discovery of remote streams
- Virtual ASIO driver "AES67 Bridge", 32 inputs / 32 outputs, clocked directly from PTP
- WASAPI input (a capture device or playback loopback) and WASAPI output, with sample-rate conversion
- Crosspoint routing matrix between every source and destination
- Local web UI in a WebView2 window; no network-facing web server

## Requirements

- Windows 10 1809 or later, x64
- [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/). It is already installed on current Windows 10/11.
- A wired Ethernet interface on the AES67 network
- A PTP grandmaster on the same network and domain, such as a Dante/AES67 device or a dedicated grandmaster

## Installation

1. Download `AES67Bridge-Setup-<version>.exe` from the [Releases](../../releases) page and run it.
2. The installer registers the ASIO driver. It also adds a Windows Firewall rule for the application.
3. You can choose to start AES67 Bridge with Windows.

AES67 Bridge runs in the system tray. Double-click the tray icon, or choose **Open AES67 Bridge**, to open the window. **Exit** stops all streams and closes the application.

## Quick start

1. **Devices → Network**: choose the interface that is connected to the AES67 network. PTP, SAP and all streams use this interface. The PTP clock identity comes from this interface's MAC address.
2. **Status**: wait for PTP to show **locked**. It shows the grandmaster ID alongside the lock state. Audio is only sent and received while PTP is locked.
3. **Transmit → Add**: create a stream. A multicast address is filled in automatically (see [Multicast addresses](#multicast-addresses)). The stream is announced with SAP, so AES67 receivers and Dante Controller (AES67 mode) can subscribe to it.
4. **Receive**: discovered streams are listed under **Discovered**; subscribe with one click. To receive a stream that is not announced, add it under **Manual** with its address, port, channel count and payload type.
5. **Matrix**: connect sources to destinations. Sources are WASAPI In, ASIO Out and the receive streams. Destinations are the transmit streams, ASIO In and WASAPI Out. Each crosspoint connects one source channel to one destination channel, and several sources can be mixed into one destination.

## Using the ASIO driver

Select **AES67 Bridge** as the ASIO device in your DAW (Cubase, Nuendo, Pro Tools, Reaper and others). It shows 32 inputs and 32 outputs at 48 kHz.

- **ASIO Out** channels are what the DAW plays. Route them to transmit streams in the matrix.
- **ASIO In** channels are what the DAW records. Route receive streams to them.
- The ASIO buffer size is set on **Devices → ASIO** (64 to 2048 samples). One DAW can use the driver at a time.
- AES67 Bridge must be running. The driver starts it if it is not.

The ASIO path is clocked by PTP directly, with no sample-rate conversion.

## WASAPI input and output

On **Devices** you can enable one WASAPI input and one WASAPI output.

- **Input**: choose a capture device, or a playback device in loopback mode. Loopback sends what Windows plays to the network.
- **Output**: plays received audio on a Windows audio device.

Windows audio devices run on their own clocks, so the WASAPI paths use sample-rate conversion to follow PTP. They therefore add a few milliseconds more latency than the ASIO path.

## Settings

### Receive playout delay

Set on **Devices → AES67 Receive**. The same value applies to every receive stream. You can choose **4, 6, 8 or 10 ms**. A longer delay tolerates more network jitter.

### Multicast addresses

Transmit addresses are assigned automatically in `239.69.X.Y`:

- `X` is derived from the MAC address of the selected interface. Each computer therefore uses its own block.
- `Y` counts streams from 1 upward.
- Addresses already in use are skipped: this computer's other streams, streams it receives, and streams that other devices announce with SAP.

You can edit the address by hand, or press **Auto** in the stream editor to pick a new free one. A stream whose address is also announced by another device on the network shows a red warning on its card. Devices that do not announce their streams with SAP cannot be detected. Keep their addresses outside `239.69.0.0/16`, or check them by hand.

### Other defaults

| Setting | Default |
|---|---|
| RTP port | 5004 |
| Payload type | 98 |
| TTL | 15 |
| DSCP, audio | 34 (AF41) |
| DSCP, PTP | 46 (EF) |
| PTP domain | 0 |
| SAP interval | 30 s |

The configuration is stored in `%ProgramData%\AES67Bridge\config.json`. The log is written to `%ProgramData%\AES67Bridge\logs\aes67bridge.log`.

## Network recommendations

- Use a built-in (PCIe) wired Ethernet adapter. USB Ethernet adapters can hold packets back for several milliseconds at a time. Receivers with short playout delays then report late packets.
- Disable Energy-Efficient Ethernet and flow control on the adapter.
- Wi-Fi is not suitable for AES67.
- Enable IGMP snooping with a querier on the switches. Give DSCP 46 (PTP) and 34 (audio) priority, as in the usual AES67/Dante QoS setup.
- The firewall must allow UDP 319/320 (PTP), 9875 (SAP) and the RTP ports. The installer adds a rule for the application.

## Troubleshooting

| Symptom | What to check |
|---|---|
| PTP never locks | Is there a grandmaster on the chosen interface and domain? Is the firewall rule present? Is the right interface selected under **Devices → Network**? |
| A receiver reports late packets or timestamp (SAC) errors | Is the computer using a USB Ethernet adapter? Otherwise, raise the receiver's playout delay. |
| No sound from a transmit stream | Is the matrix routing in place, and is the source (ASIO or WASAPI) active? Does the stream show PTP locked? |
| A stream is not listed under Discovered | Does the sender announce it with SAP? If not, add it manually. |
| Streams stop after the network cable or adapter was reconnected | Restart AES67 Bridge from the tray menu. |

## Building from source

Requirements: Visual Studio 2022 with the C++ workload, CMake 3.21 or later, and [Inno Setup 6](https://jrsoftware.org/isinfo.php) for the installer. Dependencies (libsamplerate, nlohmann/json, the WebView2 SDK and the ASIO SDK) are downloaded during the CMake configure step.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

To build the installer (output in `dist\`):

```
powershell -ExecutionPolicy Bypass -File scripts\build-installer.ps1
```

## License

AES67 Bridge is free software, licensed under the [GNU General Public License v3.0](LICENSE).

The virtual ASIO driver is built with the Steinberg ASIO SDK under its GPLv3 option. ASIO is a trademark of Steinberg Media Technologies GmbH. See [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) for the licenses of the bundled components.
