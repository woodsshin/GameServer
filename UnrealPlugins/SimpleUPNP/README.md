# SimpleUPNP — Unreal Engine UPnP Port-Forwarding Plugin

Unreal Engine Marketplace에 등록된 **Native Code Plugin**입니다.
게임 Client가 실행 중인 PC의 **공유기(Router/NAT)에 Port Forwarding을 자동으로 등록**해서, 별도의 Relay 서버 없이도 외부 인터넷에서 접속 가능한 **P2P(Listen 서버) Play**를 가능하게 해주는 것이 목적입니다.

---

## 1. 왜 필요한가 — UPnP와 NAT Traversal

가정용 인터넷 환경은 대부분 공유기 뒤에 Private IP(NAT)로 묶여 있습니다. 그래서 내가 Host가 되어 게임 Session을 열어도, 외부에 있는 친구는 내 PC의 Private IP로 직접 접속할 수 없습니다. 이를 해결하는 전통적인 방법은 세 가지입니다.

| 방법 | 설명 | 단점 |
|---|---|---|
| 수동 Port Forwarding | User가 직접 공유기 관리 Page에서 설정 | 일반 User에게는 진입장벽이 매우 높음 |
| Relay 서버 | 모든 Packet을 중계 서버가 대신 전달 | 서버 비용, Latency 증가 |
| **UPnP/NAT-PMP** | **게임이 공유기에게 "이 Port 좀 열어줘"라고 자동 Request** | 공유기가 UPnP를 지원/활성화해야 함 |

**UPnP(Universal Plug and Play)**는 이 세 번째 방법을 표준화한 Protocol입니다. Home Network 안의 기기들이 서로를 자동으로 찾고(Discovery), 서비스를 제어(Control)할 수 있게 해주는데, 그중 인터넷 공유기가 구현하는 **IGD(Internet Gateway Device)** Profile을 이용하면 Application이 직접 Port Mapping을 추가/삭제할 수 있습니다.

이 Plugin은 정확히 이 IGD Profile을 구현해서, 게임이 실행되는 순간 자동으로 "내 게임 Port를 외부로 열어달라"고 공유기에 Request하고, 성공하면 Relay 서버 없이 진짜 P2P로 다른 Player가 접속할 수 있게 됩니다.

---

## 2. UPnP IGD 동작 흐름

UPnP IGD는 아래 4단계로 동작하며, 이 Plugin의 State Machine(`UPNPState`)도 동일한 흐름을 따릅니다.

```
1) Discovery (SSDP)        : Multicast로 "Gateway 있니?" Broadcast
        ↓
2) Description (HTTP GET)  : 응답받은 Location URL에서 기기의 XML Description을 받아옴
        ↓                    (여기서 제어 가능한 Service URL 목록을 Parsing)
3) Control (SOAP over HTTP): XML Description에 있는 ControlURL로 SOAP Action 호출
        ↓                    (AddPortMapping, GetExternalIPAddress 등)
4) 결과 반영                : 성공 시 Port Mapping 완료 → External IP:Port로 접속 가능
```

### 2-1. Discovery — SSDP (Simple Service Discovery Protocol)

`239.255.255.250:1900` Multicast 주소로 `M-SEARCH` Message를 보내 Network 상의 IGD를 찾습니다.

```cpp
// DiscoverDevices.cpp
FString Message = FString::Printf(
    TEXT("M-SEARCH * HTTP/1.1\r\nHOST: %s:%u\r\nST: %s\r\nMAN: \"ssdp:discover\"\r\nMX: %d\r\n\r\n"),
    *FString(UPNP_MULTICAST_ADDR), SSDP_PORT, *ServiceType[Num], DelaySec);
SendMessage(MulticastSocket, Message);
```

- `ST`(Search Target)에 `InternetGatewayDevice`, `WANIPConnection`, `WANPPPConnection` 등 관심 있는 Service Type을 각각 Request합니다.
- Socket 수신/Timeout 처리를 Blueprint(Game Thread)와 분리하기 위해, Socket 생성 시점에 **별도의 `FRunnable` Worker Thread**를 띄워 Response를 상시 Polling합니다.

```cpp
uint32 FDiscoverDevices::Run()
{
    while (!Stopping)
    {
        ReceivePacket(MulticastSocket);
        FPlatformProcess::Sleep(SleepInterval);
    }
    return 0;
}
```

Response Message에서 `LOCATION` Header(기기 Description URL)를 Parsing해 Device 목록에 등록합니다.

```cpp
void FDiscoverDevices::ParseMessage(FString &Message)
{
    FDeviceInfo Info;
    Info.Location = ExtractMessage(Message, "LOCATION:", "\r\n");
    ...
    if (UPNPDeviceInfo.DeviceExist(Info) || Info.Location.Len() == 0)
        return; // 중복 Response 무시

    UPNPDeviceInfo.DeviceList.Add(Info);
}
```

### 2-2. Description — 기기 XML Parsing

`LOCATION` URL로 HTTP GET을 보내면 기기의 Service 목록(XML)이 내려옵니다. 여기서 실제 Port Mapping을 담당하는 `WANIPConnection` / `WANPPPConnection` Service의 `controlURL`을 추출해 이후 SOAP 호출에 사용합니다.

```cpp
void FDiscoverDevices::ParseService(FString &Service, IGDData &localIGDDATAs)
{
    IGDService IGDServices;
    IGDServices.ControlUrl  = ExtractMessage(Service, "<controlURL>", "</controlURL>");
    IGDServices.ServiceType = ExtractMessage(Service, "<serviceType>", "</serviceType>");
    ...
    if (0 <= IGDServices.ServiceType.Find("urn:schemas-upnp-org:service:WANIPConnection:") ||
        0 <= IGDServices.ServiceType.Find("urn:schemas-upnp-org:service:WANPPPConnection:"))
    {
        if (localIGDDATAs.First.ControlUrl.Len() == 0)
            localIGDDATAs.First = IGDServices;   // 이중 WAN 대비 두 Slot 보관
        else
            localIGDDATAs.Second = IGDServices;
    }
}
```

정규 XML Parser 대신 Tag 사이 문자열을 잘라내는 경량 Parser(`ExtractMessage`)를 직접 구현해, 별도 XML Library 의존성 없이 Embedded/공유기 Firmware별로 조금씩 다른 Response Format에도 유연하게 대응합니다.

### 2-3. Control — SOAP Action (Port Mapping 핵심 로직)

여기가 실질적으로 Port를 Open하는 Request가 전송되는 지점입니다. `AddPortMapping` SOAP Action에 External Port, Internal Port, Internal(Local) IP, Protocol(TCP/UDP)을 실어 보냅니다.

```cpp
bool FDiscoverDevices::SendAddPortMapping(const FSimpleUPNPInfo &InUPNPInfo)
{
    TArray<FString> Parameter;
    Parameter.Add(TEXT("<NewRemoteHost></NewRemoteHost>"));
    Parameter.Add(FString::Printf(TEXT("<NewExternalPort>%s</NewExternalPort>"), *InUPNPInfo.ExternalPort));
    Parameter.Add(FString::Printf(TEXT("<NewProtocol>%s</NewProtocol>"), *InUPNPInfo.Protocol));
    Parameter.Add(FString::Printf(TEXT("<NewInternalPort>%s</NewInternalPort>"), *InUPNPInfo.InternalPort));
    Parameter.Add(FString::Printf(TEXT("<NewInternalClient>%s</NewInternalClient>"), *InUPNPInfo.InAddress));
    Parameter.Add(FString::Printf(TEXT("<NewEnabled>%d</NewEnabled>"), InUPNPInfo.Enabled));
    Parameter.Add(FString::Printf(TEXT("<NewLeaseDuration>%d</NewLeaseDuration>"), InUPNPInfo.Duration));

    TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request =
        CreateSoapRequest(IGDDatas.First, "AddPortMapping", Parameter);
    Request->OnProcessRequestComplete().BindRaw(this, &FDiscoverDevices::OnResponseAddPortMapping);
    return Request->ProcessRequest();
}
```

SOAP Envelope와 HTTP Header 조립은 Common Helper로 분리되어, `AddPortMapping` / `DeletePortMapping` / `GetExternalIPAddress` / `GetGenericPortMappingEntry` 등 모든 Action이 동일한 경로를 재사용합니다.

```cpp
FString FDiscoverDevices::GetSOAPBody(const IGDService &Service, const FString& Action, const TArray<FString> &Parameter)
{
    FString Parameters;
    for (int Num = 0; Num < Parameter.Num(); ++Num)
        Parameters += Parameter[Num];

    return FString::Printf(TEXT(
        "<?xml version=\"1.0\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>"),
        *Action, *Service.ServiceType, *Parameters, *Action);
}

TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> FDiscoverDevices::CreateSoapRequest(
    const IGDService &Service, const FString& Action, const TArray<FString> &Parameter)
{
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = Http->CreateRequest();
    Request->SetHeader("SOAPAction", GetSOAPAction(Service, Action));
    Request->SetHeader("Content-Type", "text/xml; charset=\"utf-8\"");
    Request->SetVerb("POST");
    Request->SetContentAsString(GetSOAPBody(Service, Action, Parameter));
    Request->SetURL(FString::Printf(TEXT("http://%s%s"), *HostAddress, *Service.ControlUrl));
    return Request;
}
```

이 Request가 성공하면 공유기 NAT Table에 `External Port → (내PC Private IP):Internal Port` Mapping이 즉시 등록되고, 이후로는 외부에서 `Public IP:External Port`로 들어온 Packet이 곧장 내 게임 Client로 전달됩니다 — Relay 서버를 경유하지 않는 순수 P2P Path가 Open되는 순간입니다.

### 2-4. State Machine & Retry

UPnP 통신은 UDP Broadcast + HTTP Request가 연쇄적으로 이어지는 비동기 흐름이라 실패 지점이 많습니다. `UPNPState` Enum으로 현재 어느 단계인지 추적하고, 각 단계별로 최대 Retry 횟수(`MAX_RETRY_COUNT`)까지 자동 Retry합니다.

```cpp
bool FDiscoverDevices::RetryRequest(const ESimpleUPNPErrorCode InErrorCode)
{
    ++RetryCount;
    if (AllowRetry && RetryCount < MAX_RETRY_COUNT)
    {
        switch (State)
        {
            case UPNPState::UPNPState_GetValidIGD: RetrySendValidIGD(); break;
            case UPNPState::UPNPState_AddPortMapping:
                State = UPNPState::UPNPState_IDLE;
                SendAddPortMapping(AddUPNPInfo);
                break;
            // ...
        }
        return true;
    }
    ResetRetryCount();
    TriggerResult(false, InErrorCode);
    return false;
}
```

가정집 Network는 공유기에 IGD가 여러 개 잡히거나(가상화 Bridge, Guest Network 등), Response가 간헐적으로 유실되는 경우가 흔해서, 실패 시 다음 Device로 넘어가며 Retry하는 로직(`RetrySendValidIGD`)이 실사용 안정성에 중요한 역할을 합니다.

---

## 3. Architecture

```
Blueprint (게임 로직)
      │
      ▼
SimpleUPNPBlueprintLibrary / xxxCallbackProxy (UAddPortCallbackProxy, UGetPortListCallbackProxy, ...)
      │   BlueprintAsyncActionBase 기반 - Latent Node로 Blueprint에 결과 Callback 제공
      ▼
UUPNPModule (GameInstanceSubsystem 격 - FDiscoverDevices Singleton 소유/수명 관리)
      │
      ▼
FDiscoverDevices (FRunnable)
      │   - Worker Thread: SSDP Multicast 송수신
      │   - Game Thread: HTTP(SOAP) Request/Parsing, State Machine
      ▼
SSDP(UDP Multicast) + SOAP over HTTP  ──▶  공유기(IGD)
```

- **`FDiscoverDevices`**: Socket/Thread/HTTP/State Machine을 모두 소유하는 Core Class. `FRunnable`을 상속해 SSDP Response 수신을 전용 Thread에서 Non-blocking으로 처리합니다.
- **`*CallbackProxy` 계열**(`UAddPortCallbackProxy`, `URemovePortCallbackProxy`, `UGetPortListCallbackProxy`, `UPerformAllDevicesCallbackProxy`): `UBlueprintAsyncActionBase`를 상속받아, Blueprint에서 `Add Port`, `Get Port List`처럼 **Latent Node 한 번 호출**로 "Port 추가/삭제 → 성공/실패 Callback"까지 자연스럽게 이어지도록 감쌌습니다. Designer가 UPnP Protocol을 몰라도 Node 하나로 P2P Open을 붙일 수 있게 하는 것이 목적입니다.
- **`UPerformAllDevicesCallbackProxy`**: Network에 IGD가 여러 대 있는 경우(예: 이중 공유기 구성) 모든 Device를 순회하며 Port Mapping을 시도하는 상위 Orchestration 로직입니다.
- **`FSimpleUPNPInfo` / `FUPNPDeviceInfo`**: `USTRUCT(BlueprintType)`으로 선언해 Mapping 정보(Protocol, Port, Internal IP 등)와 Device 목록을 Blueprint Graph에서 그대로 값으로 다룰 수 있게 노출했습니다.

```cpp
// AddPortCallbackProxy.cpp — Blueprint Node 하나로 끝나는 비동기 흐름
void UAddPortCallbackProxy::Activate()
{
    DiscoverDevices = UPNPModule->GetDiscoverDevices();
    DiscoverDevicesResultDelegate = FDiscoverDevicesResultDelegate::CreateUObject(
        this, &UAddPortCallbackProxy::OnAddPort);
    DiscoverDevicesResultDelegateHandle =
        DiscoverDevices->AddDiscoverDevicesResultDelegate_Handle(DiscoverDevicesResultDelegate);

    DiscoverDevices->ResetRetryCount();
    if (!DiscoverDevices->SendAddPortMapping(UPNPInfo))
        OnAddPort(false, ESimpleUPNPErrorCode::ADD_MAPPING_FAILED);
}
```

---

## 4. 주요 기능 요약

- **IGD 자동 탐색**: SSDP Multicast로 Network 내 공유기(IGD) 탐색 및 다중 Device 대응
- **Port Mapping 추가/삭제**: `AddPortMapping` / `DeletePortMapping` SOAP Action으로 TCP/UDP Port 자동 개방
- **External IP / 연결 상태 조회**: `GetExternalIPAddress`, `GetStatusInfo`, `GetCommonLinkProperties` 등으로 현재 Public IP·회선 상태 확인
- **현재 Mapping 목록 조회**: `GetGenericPortMappingEntry`로 이미 등록된 Port Mapping 전체 열람
- **Blueprint 완전 지원**: 모든 기능이 `BlueprintCallable` / Latent Node로 노출되어 C++ 없이 사용 가능
- **자동 Retry & Timeout**: Request 단계별 실패 시 최대 Retry 횟수까지 자동 복구 시도
- **비동기 Thread 처리**: Socket 송수신을 Game Thread와 분리해 Frame Drop 없이 동작

---

## 5. 기술 스택

- **Engine**: Unreal Engine (C++ Plugin, `IModuleInterface` 기반)
- **Networking**: UDP Multicast Socket(`FUdpSocketBuilder`), Unreal `Http` Module(SOAP/HTTP)
- **Concurrency**: `FRunnable` + `FRunnableThread`, `FCriticalSection`으로 Thread 간 상태 동기화
- **Blueprint 통합**: `UBlueprintAsyncActionBase`, `USTRUCT(BlueprintType)`, Multicast Delegate
- **Protocol**: SSDP(UPnP Discovery), SOAP over HTTP(UPnP Control), UPnP IGD:1/2 표준

---

## 6. 참고 규격 문서

- UPnP Device Architecture v1.1 / v2.0 — http://upnp.org/specs/arch/
- WANIPConnection:1 Service — http://upnp.org/specs/gw/UPnP-gw-WANIPConnection-v1-Service.pdf
- SSDP 개요 — https://wiki.wireshark.org/SSDP
