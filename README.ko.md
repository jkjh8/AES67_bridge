[English](README.md) | [한국어](README.ko.md)

# AES67 Bridge

AES67 Bridge는 Windows 트레이(tray) 애플리케이션입니다. Windows 오디오(WASAPI)와 DAW(자체 가상 ASIO 드라이버를 통해)를 AES67 오디오-오버-IP 네트워크에 연결합니다.

- AES67 송신/수신: L24, 48 kHz, 1 ms 패킷, 스트림당 최대 8채널, 다중 스트림 지원
- PTP(IEEE 1588-2008) 슬레이브 — 네트워크상의 그랜드마스터를 따라갑니다
- 송신 스트림의 SAP/SDP 광고 및 원격 스트림 디스커버리
- 가상 ASIO 드라이버 "AES67 Bridge" — 32입력/32출력, PTP로 직접 클럭 공급
- WASAPI 입력(캡처 장치 또는 재생 루프백) 및 WASAPI 출력, 샘플레이트 변환 포함
- 모든 소스와 목적지 사이의 크로스포인트 라우팅 매트릭스
- WebView2 창으로 제공되는 로컬 웹 UI — 네트워크에 노출되는 웹 서버 없음

## 요구 사항

- Windows 10 1809 이상, x64
- [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) — 최신 Windows 10/11에는 이미 설치되어 있습니다.
- AES67 네트워크에 연결된 유선 이더넷 인터페이스
- 동일 네트워크·도메인상의 PTP 그랜드마스터(Dante/AES67 장치 또는 전용 그랜드마스터 등)

## 시작하기

1. 설치 프로그램을 실행합니다. AES67 Bridge를 설치하고 ASIO 드라이버를 등록합니다. 드라이버를 재시작 없이 교체할 수 있도록, 설치·업데이트 전에 DAW를 종료하세요.
2. Windows 방화벽이 물으면 AES67 Bridge를 허용합니다(필요에 따라 개인/공용 네트워크 모두).
3. DAW에 ASIO 드라이버가 보이지 않으면 **Devices → ASIO Driver**에서 **Register (admin)**을 누릅니다.

AES67 Bridge는 시스템 트레이에서 실행됩니다. 트레이 아이콘을 더블클릭하거나 **Open AES67 Bridge**를 선택하면 창이 열립니다. 창을 닫아도 스트림은 트레이에서 계속 실행됩니다. 트레이 메뉴에는 **Start with Windows**와 **Exit**도 있으며, **Exit**는 모든 스트림을 정지하고 애플리케이션을 종료합니다.

## 빠른 시작

1. **Devices → Network**: AES67 네트워크에 연결된 인터페이스를 선택합니다. PTP, SAP, 모든 스트림이 이 인터페이스를 사용합니다. PTP clock identity는 이 인터페이스의 MAC 주소에서 생성됩니다.
2. **Status**: PTP가 **locked**로 표시될 때까지 기다립니다. 잠금 상태 옆에 그랜드마스터 ID가 표시됩니다. 오디오는 PTP가 locked 상태일 때만 송수신됩니다.
3. **Transmit → Add**: 스트림을 생성합니다. 멀티캐스트 주소는 자동으로 채워집니다([멀티캐스트 주소](#멀티캐스트-주소) 참고). 스트림은 SAP로 광고되므로 AES67 수신기와 Dante Controller(AES67 모드)에서 구독할 수 있습니다.
4. **Receive**: 발견된 스트림은 **Discovered** 아래에 나열되며, 클릭 한 번으로 구독할 수 있습니다. 광고되지 않는 스트림을 받으려면 **Manual**에서 주소, 포트, 채널 수, payload type을 직접 입력해 추가합니다.
5. **Matrix**: 소스를 목적지에 연결합니다. 소스는 WASAPI 입력, ASIO Out, 수신 스트림들입니다. 목적지는 송신 스트림들, ASIO In, WASAPI 출력입니다. WASAPI 포트에는 모드와 장치가 함께 표시됩니다(예: `Loopback · Speakers`). 각 크로스포인트는 소스 채널 하나를 목적지 채널 하나에 연결하며, 여러 소스를 하나의 목적지로 믹스할 수도 있습니다. 16채널을 넘는 포트(ASIO)는 16채널 단위 블록으로 표시되어 각각 접고 펼칠 수 있습니다.

### 스트림 상태

각 송신/수신 카드는 이름 옆 아이콘으로 상태를 표시합니다. 마우스를 올리면 텍스트로도 확인할 수 있습니다.

| 아이콘 | 송신(Transmit) | 수신(Receive) |
|---|---|---|
| 초록 ▶ | 전송 중 | 수신 중 |
| 주황 ■ | 시작 중 | 신호 없음 |
| 회색 ■ | 꺼짐 | 꺼짐 |
| 빨강 ■ | 오류 | 오류 |

### 수신 스트림 상세 정보

수신 카드의 ⓘ 아이콘을 누르면 스트림이 시작된 이후(또는 마지막으로 카운터를 초기화한 이후)의 패킷 통계를 볼 수 있습니다: 수신한 패킷 수, 손실(시퀀스 번호 누락), 순서 어긋남(out-of-order), 손상된 패킷(malformed), 그리고 송신측의 현재 SSRC와 그 변경 횟수(변경되었다는 것은 송신측이 자체 스트림을 재시작했다는 의미입니다). **Reset counters**를 누르면 수신을 중단하지 않고 이 카운터들만 0으로 초기화됩니다.

## ASIO 드라이버 사용하기

DAW(Cubase, Nuendo, Pro Tools, Reaper 등)에서 ASIO 장치로 **AES67 Bridge**를 선택하세요. 48 kHz에서 32입력/32출력으로 표시됩니다.

- **ASIO Out** 채널은 DAW가 재생하는 신호입니다. 매트릭스에서 송신 스트림으로 라우팅하세요.
- **ASIO In** 채널은 DAW가 녹음하는 신호입니다. 수신 스트림을 이 채널로 라우팅하세요.
- ASIO 버퍼 크기는 **Devices → ASIO Driver**에서 설정합니다(64~2048 샘플). DAW가 드라이버를 다시 열 때 적용됩니다. 한 번에 하나의 DAW만 드라이버를 사용할 수 있습니다.
- AES67 Bridge가 실행 중이어야 합니다. 실행 중이 아니면 드라이버가 자동으로 시작시킵니다.
- DAW 출력은 전송 전에 고정 3 ms만큼 버퍼링되므로, DAW 콜백이 조금 늦어도 끊김이 발생하지 않습니다.

ASIO 경로는 샘플레이트 변환 없이 PTP로 직접 클럭이 공급됩니다.

## WASAPI 입력과 출력

**Devices**에서 WASAPI 입력 1개와 WASAPI 출력 1개를 활성화할 수 있습니다.

- **Input**: 캡처 장치를 선택하거나, 재생 장치를 루프백 모드로 선택합니다. 루프백은 Windows가 해당 장치로 재생하는 모든 소리(브라우저, 미디어 플레이어, 시스템 사운드)를 캡처합니다. 매트릭스에서는 WASAPI 소스로 표시되며, 송신 스트림으로 라우팅하면 네트워크로 전송됩니다. WASAPI Output이 재생 중인 장치를 그대로 루프백하지 마세요. 수신한 오디오가 다시 전송되어 버립니다 — Devices 페이지에서도 이에 대해 경고합니다.
- **Output**: 수신한 오디오를 Windows 오디오 장치로 재생합니다.

각 장치 아래의 상태 줄에는 모드, 포맷, 버퍼 채움 정도, 그리고 입력의 경우 시작 이후 언더런(underrun) 횟수가 표시됩니다.

Windows 오디오 장치는 자체 클럭으로 동작하므로, WASAPI 경로는 PTP를 따라가기 위해 샘플레이트 변환을 사용합니다. 따라서 ASIO 경로보다 지연시간이 몇 밀리초 더 추가됩니다.

## 설정

### 수신 재생 지연

**Devices → AES67 Receive**에서 설정합니다. 이 값은 모든 수신 스트림에 동일하게 적용됩니다. **4, 6, 8, 10, 20 ms** 중 선택할 수 있습니다. 지연을 늘릴수록 네트워크 지터에 더 잘 견딥니다.

### Windows와 함께 시작

**Devices**에서, 또는 트레이 메뉴에서 **Start with Windows**를 켭니다. 이후 Windows에 로그인하면 AES67 Bridge가 창을 열지 않고 트레이에서 자동으로 시작합니다.

### 멀티캐스트 주소

송신 주소는 `239.69.X.Y` 형식으로 자동 할당됩니다:

- `X`는 선택한 인터페이스의 MAC 주소에서 파생됩니다. 따라서 컴퓨터마다 각자의 블록을 사용합니다.
- `Y`는 1부터 증가하는 스트림 번호입니다.
- 이미 사용 중인 주소는 건너뜁니다: 이 컴퓨터의 다른 스트림, 이 컴퓨터가 수신 중인 스트림, 그리고 다른 장치가 SAP로 광고하는 스트림이 모두 포함됩니다.

주소는 직접 수정할 수 있고, 스트림 편집기에서 **Auto**를 누르면 새로운 빈 주소를 다시 할당받을 수 있습니다. 다른 장치도 같은 주소를 광고하고 있는 스트림은 카드에 빨간 경고가 표시됩니다. SAP로 스트림을 광고하지 않는 장치는 감지할 수 없습니다. 그런 장치의 주소는 `239.69.0.0/16` 밖으로 정하거나, 직접 확인해서 겹치지 않게 하세요.

### 기타 기본값

| 설정 | 기본값 |
|---|---|
| RTP 포트 | 5004 |
| Payload type | 98 |
| TTL | 15 |
| DSCP, 오디오 | 34 (AF41) |
| DSCP, PTP | 46 (EF) |
| PTP 도메인 | 0 |
| SAP 주기 | 30초 |

설정은 `%ProgramData%\AES67Bridge\config.json`에 저장됩니다. 로그는 `%ProgramData%\AES67Bridge\logs\aes67bridge.log`에 기록됩니다.

## 네트워크 권장 사항

- 내장(PCIe) 유선 이더넷 어댑터가 가장 낮고 안정적인 지연을 제공합니다. Intel 이더넷 컨트롤러를 사용하는 어댑터를 권장합니다.
- USB 이더넷 어댑터도 동작하지만 지연과 지터가 늘어납니다. 아래 어댑터 설정을 적용하고, 칩셋이 아니라 CPU에 직접 연결된 USB 포트에 어댑터를 꽂으세요:
  - 최근 Intel 노트북에서는 보통 Thunderbolt/USB4 Type-C 포트가 CPU에, 나머지 USB 포트는 칩셋에 연결되어 있습니다.
  - 확인하려면 장치 관리자 → 보기 → 연결별 장치를 열어, 어댑터가 어떤 USB 호스트 컨트롤러 아래에 있는지 찾아보세요. 카메라, 블루투스, 지문 인식기 등 내부 장치와 공유되는 컨트롤러보다는 외부 포트 전용 컨트롤러가 더 좋습니다.
  - CPU 포트에 연결된 Thunderbolt 독(dock)은 괜찮습니다. USB 2 허브는 피하세요.
- Wi-Fi는 AES67에 적합하지 않습니다.

### 어댑터 설정

장치 관리자 → 네트워크 어댑터 → 해당 어댑터 → 속성 → 고급에서 다음을 설정하세요:

| 속성 | 설정값 | 이유 |
|---|---|---|
| Energy-Efficient Ethernet / Green Ethernet | 사용 안 함(Disabled) | 링크 전력 절약 기능이 패킷을 지연시킵니다 |
| Flow Control | 사용 안 함(Disabled) | 스위치가 보내는 pause 프레임이 수 밀리초 동안 전송을 멈출 수 있습니다 |
| Transmit URBs (USB 어댑터) | 16 이상 | 동시에 처리 중인 USB 전송이 늘어나 패킷이 덜 쌓입니다 |
| Receive URBs (USB 어댑터) | 16 이상 | 수신 패킷에도 동일하게 적용됩니다 |
| Interrupt Moderation (PCIe 어댑터) | 사용 안 함 또는 낮음 | 작고 빈번한 패킷의 지연이 줄어듭니다 |

USB 어댑터의 경우 USB 선택적 절전도 꺼야 합니다: 제어판 → 전원 옵션 → 전원 관리 옵션 변경 → 고급 전원 관리 옵션 변경 → USB 설정 → USB 선택적 절전 모드 설정 → 사용 안 함.

관리자 권한 PowerShell에서 동일한 설정을 적용하는 예시입니다(어댑터 이름은 `Ethernet`으로 가정하며, 속성 이름은 드라이버마다 다를 수 있으니 `Get-NetAdapterAdvancedProperty -Name Ethernet`으로 목록을 먼저 확인하세요):

```
Set-NetAdapterAdvancedProperty -Name Ethernet -RegistryKeyword "*FlowControl" -DisplayValue "Disabled"
Set-NetAdapterAdvancedProperty -Name Ethernet -RegistryKeyword "PendingTransmits" -DisplayValue "16"
Set-NetAdapterAdvancedProperty -Name Ethernet -RegistryKeyword "PendingReceives" -DisplayValue "16"
```

어댑터 속성을 변경하면 어댑터가 재시작됩니다. 이후 AES67 Bridge도 다시 시작하세요.

### 스위치와 방화벽

- 스위치에서 쿼리어(querier)가 있는 IGMP 스누핑을 활성화하세요. 일반적인 AES67/Dante QoS 구성과 마찬가지로 DSCP 46(PTP)과 34(오디오)에 우선순위를 부여하세요.
- 방화벽은 UDP 319/320(PTP), 9875(SAP), 그리고 RTP 포트들을 허용해야 합니다.

## 문제 해결

| 증상 | 확인 사항 |
|---|---|
| PTP가 전혀 locked 되지 않음 | 선택한 인터페이스·도메인에 그랜드마스터가 있나요? 방화벽 규칙이 등록되어 있나요? **Devices → Network**에서 올바른 인터페이스를 선택했나요? |
| 수신기에서 late packet 또는 타임스탬프(SAC) 오류 보고 | 수신기 쪽 레이턴시를 늘려보세요. USB 이더넷 어댑터라면 [어댑터 설정](#어댑터-설정)을 적용하세요. |
| 특정 수신 카드의 [상세 정보](#수신-스트림-상세-정보)에서 손실/손상 패킷이 보임 | 케이블과 스위치 QoS를 점검하고, USB 이더넷 어댑터라면 [어댑터 설정](#어댑터-설정)을 적용하세요. SSRC 변경 횟수가 계속 늘어난다면 송신측이 스트림을 계속 재시작하고 있다는 뜻입니다. |
| 특정 송신 스트림에서만 late packet 발생 | 다른 장치가 같은 멀티캐스트 주소로 광고 없이 전송 중일 수 있습니다. 스트림을 다른 주소로 옮기세요. |
| 송신 스트림에서 소리가 안 남 | 매트릭스 라우팅이 되어 있나요? 소스(ASIO 또는 WASAPI)가 활성화되어 있나요? 해당 스트림에서 PTP locked로 표시되나요? |
| Discovered 목록에 어떤 스트림이 안 보임 | 송신측이 SAP로 광고하고 있나요? 아니라면 수동으로 추가하세요. |
| 네트워크 케이블이나 어댑터를 재연결한 후 스트림이 멈춤 | 트레이 메뉴에서 AES67 Bridge를 다시 시작하세요. |
| WASAPI 장치에서 오류 또는 다수의 언더런이 보임 | 다른 애플리케이션이 해당 장치를 독점(exclusive) 모드로 사용하고 있지 않은지 확인한 뒤, **Devices**에서 장치를 다시 선택하세요. |

## 라이선스

AES67 Bridge는 자유 소프트웨어이며, [GNU General Public License v3.0](LICENSE) 라이선스를 따릅니다. 이 소프트웨어는 **어떠한 보증도 없이 "있는 그대로"** 제공되며, 라이선스에 명시된 대로 사용에 따른 책임은 전적으로 사용자에게 있습니다.

가상 ASIO 드라이버는 Steinberg ASIO SDK를 GPLv3 라이선스 옵션으로 사용해 빌드됩니다. SDK 자체는 이 저장소에 포함되어 있지 않으며, [Steinberg](https://www.steinberg.net/developers/asiosdk-open/)에서 직접 받아야 합니다. ASIO는 Steinberg Media Technologies GmbH의 상표입니다. 포함된 구성 요소들의 라이선스는 [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)를 참고하세요.
