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

## Getting started

1. Run the installer. It installs AES67 Bridge and registers the ASIO driver. Close your DAW before installing or updating, so the driver can be replaced without a restart.
2. Allow AES67 Bridge through Windows Firewall when asked, for private and public networks as needed.
3. If the DAW does not list the ASIO driver, open **Devices → ASIO Driver** and press **Register (admin)**.

AES67 Bridge runs in the system tray. Double-click the tray icon, or choose **Open AES67 Bridge**, to open the window. Closing the window keeps the streams running in the tray. The tray menu also has **Start with Windows** and **Exit**; **Exit** stops all streams and closes the application.

## Quick start

1. **Devices → Network**: choose the interface that is connected to the AES67 network. PTP, SAP and all streams use this interface. The PTP clock identity comes from this interface's MAC address.
2. **Status**: wait for PTP to show **locked**. It shows the grandmaster ID alongside the lock state. Audio is only sent and received while PTP is locked.
3. **Transmit → Add**: create a stream. A multicast address is filled in automatically (see [Multicast addresses](#multicast-addresses)). The stream is announced with SAP, so AES67 receivers and Dante Controller (AES67 mode) can subscribe to it.
4. **Receive**: discovered streams are listed under **Discovered**; subscribe with one click. To receive a stream that is not announced, add it under **Manual** with its address, port, channel count and payload type.
5. **Matrix**: connect sources to destinations. Sources are the WASAPI input, ASIO Out and the receive streams. Destinations are the transmit streams, ASIO In and the WASAPI output. WASAPI ports are labelled with their mode and device, for example `Loopback · Speakers`. Each crosspoint connects one source channel to one destination channel, and several sources can be mixed into one destination. Ports with more than 16 channels (ASIO) are shown in blocks of 16 that collapse and expand separately.

### Stream status

Each transmit and receive card shows its state as an icon next to the name. Hover over it for the text.

| Icon | Transmit | Receive |
|---|---|---|
| Green ▶ | Sending | Receiving |
| Orange ■ | Starting | No signal |
| Grey ■ | Off | Off |
| Red ■ | Error | Error |

## Using the ASIO driver

Select **AES67 Bridge** as the ASIO device in your DAW (Cubase, Nuendo, Pro Tools, Reaper and others). It shows 32 inputs and 32 outputs at 48 kHz.

- **ASIO Out** channels are what the DAW plays. Route them to transmit streams in the matrix.
- **ASIO In** channels are what the DAW records. Route receive streams to them.
- The ASIO buffer size is set on **Devices → ASIO Driver** (64 to 2048 samples). It applies when the DAW reopens the driver. One DAW can use the driver at a time.
- AES67 Bridge must be running. The driver starts it if it is not.
- DAW output is buffered by a fixed 3 ms before it is sent, so a late DAW callback does not cause a dropout.

The ASIO path is clocked by PTP directly, with no sample-rate conversion.

## WASAPI input and output

On **Devices** you can enable one WASAPI input and one WASAPI output.

- **Input**: choose a capture device, or a playback device in loopback mode. Loopback captures what Windows plays on that device (browsers, media players, system sounds). In the matrix it appears as the WASAPI source; route it to a transmit stream to send it to the network. Do not loop back the same device that WASAPI Output plays to, or received audio is sent back out; the Devices page warns about this.
- **Output**: plays received audio on a Windows audio device.

The status line under each device shows its mode, format, buffer fill and, for the input, the number of underruns since it started.

Windows audio devices run on their own clocks, so the WASAPI paths use sample-rate conversion to follow PTP. They therefore add a few milliseconds more latency than the ASIO path.

## Settings

### Receive playout delay

Set on **Devices → AES67 Receive**. The same value applies to every receive stream. You can choose **4, 6, 8, 10 or 20 ms**. A longer delay tolerates more network jitter.

### Start with Windows

Turn on **Start with Windows** on **Devices**, or in the tray menu. AES67 Bridge then starts in the tray when you sign in, without opening the window.

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

- A built-in (PCIe) wired Ethernet adapter gives the lowest and steadiest delay. Prefer adapters with an Intel Ethernet controller.
- USB Ethernet adapters work, but they add delay and jitter. Apply the adapter settings below, and connect the adapter to a USB port that is attached to the CPU rather than to the chipset:
  - On recent Intel laptops, the Thunderbolt / USB4 Type-C ports are usually attached to the CPU, and the other USB ports to the chipset.
  - To check, open Device Manager → View → Devices by connection, and find the adapter under its USB host controller. A controller that serves only external ports is preferable to one that is shared with internal devices such as the camera, Bluetooth or the fingerprint reader.
  - A Thunderbolt dock on a CPU port is fine. Avoid USB 2 hubs.
- Wi-Fi is not suitable for AES67.

### Adapter settings

Open Device Manager → Network adapters → your adapter → Properties → Advanced, and set:

| Property | Setting | Why |
|---|---|---|
| Energy-Efficient Ethernet / Green Ethernet | Disabled | Link power saving delays packets |
| Flow Control | Disabled | Pause frames from the switch can stop transmission for milliseconds |
| Transmit URBs (USB adapters) | 16 or more | More USB transfers in flight, so packets queue less |
| Receive URBs (USB adapters) | 16 or more | Same for incoming packets |
| Interrupt Moderation (PCIe adapters) | Disabled or Low | Lower delay for small, frequent packets |

For USB adapters, also turn off USB selective suspend: Control Panel → Power Options → Change plan settings → Change advanced power settings → USB settings → USB selective suspend setting → Disabled.

The same settings from an elevated PowerShell, with the adapter named `Ethernet` (property names depend on the driver; list them with `Get-NetAdapterAdvancedProperty -Name Ethernet`):

```
Set-NetAdapterAdvancedProperty -Name Ethernet -RegistryKeyword "*FlowControl" -DisplayValue "Disabled"
Set-NetAdapterAdvancedProperty -Name Ethernet -RegistryKeyword "PendingTransmits" -DisplayValue "16"
Set-NetAdapterAdvancedProperty -Name Ethernet -RegistryKeyword "PendingReceives" -DisplayValue "16"
```

Changing adapter properties restarts the adapter. Restart AES67 Bridge afterwards.

### Switches and firewall

- Enable IGMP snooping with a querier on the switches. Give DSCP 46 (PTP) and 34 (audio) priority, as in the usual AES67/Dante QoS setup.
- The firewall must allow UDP 319/320 (PTP), 9875 (SAP) and the RTP ports.

## Troubleshooting

| Symptom | What to check |
|---|---|
| PTP never locks | Is there a grandmaster on the chosen interface and domain? Is the firewall rule present? Is the right interface selected under **Devices → Network**? |
| A receiver reports late packets or timestamp (SAC) errors | Raise the receiver's latency. On a USB Ethernet adapter, apply the [adapter settings](#adapter-settings). |
| Late packets from one transmit stream only | Another device may send to the same multicast address without announcing it. Move the stream to a different address. |
| No sound from a transmit stream | Is the matrix routing in place, and is the source (ASIO or WASAPI) active? Does the stream show PTP locked? |
| A stream is not listed under Discovered | Does the sender announce it with SAP? If not, add it manually. |
| Streams stop after the network cable or adapter was reconnected | Restart AES67 Bridge from the tray menu. |
| A WASAPI device shows an error or many underruns | Check that no other application uses the device in exclusive mode, then reselect it on **Devices**. |

## License

AES67 Bridge is free software, licensed under the [GNU General Public License v3.0](LICENSE).

The virtual ASIO driver is built with the Steinberg ASIO SDK under its GPLv3 license option. The SDK itself is not part of this repository; download it from [Steinberg](https://www.steinberg.net/developers/asiosdk-open/). ASIO is a trademark of Steinberg Media Technologies GmbH. See [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) for the licenses of the bundled components.
