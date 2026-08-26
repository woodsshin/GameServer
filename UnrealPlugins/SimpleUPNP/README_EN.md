# SimpleUPNP — Unreal Engine UPnP Port-Forwarding Plugin

A **Native Code Plugin** listed on the Unreal Engine Marketplace.
Its purpose is to let the PC running the game Client **automatically register port forwarding on its router (Router/NAT)**, enabling **P2P (Listen server) play** that's reachable from the open internet without needing a separate relay server.

---

## 1. Why It's Needed — UPnP and NAT Traversal

Most home internet setups sit behind a router with a private IP (NAT). So even if you host a game session, a friend outside your network can't connect directly to your PC's private IP. There are three traditional ways to solve this.

| Method | Description | Downside |
|---|---|---|
| Manual port forwarding | User configures it manually in the router's admin page | Very high barrier of entry for most users |
| Relay server | A relay server forwards all packets on your behalf | Server cost, increased latency |
| **UPnP/NAT-PMP** | **The game automatically requests the router to "open this port"** | Router must support and have UPnP enabled |

**UPnP (Universal Plug and Play)** is the standardized protocol for this third approach. It lets devices on a home network discover each other automatically and control services — and by using the **IGD (Internet Gateway Device)** profile that internet routers implement, an application can add/remove port mappings directly.

This plugin implements exactly that IGD profile: the moment the game runs, it automatically requests the router to "open my game's port to the outside," and if that succeeds, other players can connect via true P2P without going through a relay server.

---

## 2. UPnP IGD Flow

UPnP IGD operates in the four stages below, and this plugin's state machine (`UPNPState`) follows the same flow.

```
1) Discovery (SSDP)        : Broadcast "is there a gateway?" via multicast
        ↓
2) Description (HTTP GET)  : Fetch the device's XML description from the returned Location URL
        ↓                    (parse the list of controllable service URLs here)
3) Control (SOAP over HTTP): Call a SOAP action against the ControlURL found in the XML description
        ↓                    (AddPortMapping, GetExternalIPAddress, etc.)
4) Apply the result         : On success, the port mapping is complete → reachable via External IP:Port
```

### 2-1. Discovery — SSDP (Simple Service Discovery Protocol)

Sends an `M-SEARCH` message to the multicast address `239.255.255.250:1900` to find the IGD on the network.

```cpp
// DiscoverDevices.cpp
FString Message = FString::Printf(
    TEXT("M-SEARCH * HTTP/1.1\r\nHOST: %s:%u\r\nST: %s\r\nMAN: \"ssdp:discover\"\r\nMX: %d\r\n\r\n"),
    *FString(UPNP_MULTICAST_ADDR), SSDP_PORT, *ServiceType[Num], DelaySec);
SendMessage(MulticastSocket, Message);
```

- Each service type of interest — `InternetGatewayDevice`, `WANIPConnection`, `WANPPPConnection`, etc. — is requested separately via `ST` (Search Target).
- To keep socket receive/timeout handling separate from the Blueprint (game thread), a **dedicated `FRunnable` worker thread** is spun up at socket creation time to continuously poll for responses.

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

The `LOCATION` header (the device's description URL) is parsed out of the response message and registered into the device list.

```cpp
void FDiscoverDevices::ParseMessage(FString &Message)
{
    FDeviceInfo Info;
    Info.Location = ExtractMessage(Message, "LOCATION:", "\r\n");
    ...
    if (UPNPDeviceInfo.DeviceExist(Info) || Info.Location.Len() == 0)
        return; // ignore duplicate responses

    UPNPDeviceInfo.DeviceList.Add(Info);
}
```

### 2-2. Description — Parsing the Device XML

An HTTP GET to the `LOCATION` URL returns the device's list of services (XML). From this, the `controlURL` of the `WANIPConnection` / `WANPPPConnection` service — the one actually responsible for port mapping — is extracted for use in subsequent SOAP calls.

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
            localIGDDATAs.First = IGDServices;   // keep two slots in case of dual WAN
        else
            localIGDDATAs.Second = IGDServices;
    }
}
```

Instead of a full XML parser, a lightweight custom parser (`ExtractMessage`) that simply extracts the string between tags is used, avoiding a dependency on an external XML library and flexibly handling the slightly different response formats found across embedded/router firmwares.

### 2-3. Control — SOAP Action (the Core Port-Mapping Logic)

This is where the request that actually opens the port is sent. The `AddPortMapping` SOAP action carries the external port, internal port, internal (local) IP, and protocol (TCP/UDP).

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

Building the SOAP envelope and HTTP headers is factored into a common helper, so every action — `AddPortMapping` / `DeletePortMapping` / `GetExternalIPAddress` / `GetGenericPortMappingEntry`, etc. — reuses the same code path.

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

Once this request succeeds, the router's NAT table immediately registers an `External Port → (my PC's private IP):Internal Port` mapping, and from then on any packet arriving from the outside at `Public IP:External Port` is forwarded straight to my game client — the moment a pure P2P path opens without going through a relay server.

### 2-4. State Machine & Retry

UPnP communication is an asynchronous chain of UDP broadcasts and HTTP requests, so there are many points where it can fail. A `UPNPState` enum tracks which stage is currently in progress, and each stage automatically retries up to a maximum retry count (`MAX_RETRY_COUNT`).

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

It's common for a home network's router to expose multiple IGDs (virtual bridges, guest networks, etc.) or for responses to be intermittently lost, so the logic that moves on to the next device and retries on failure (`RetrySendValidIGD`) plays an important role in real-world reliability.

---

## 3. Architecture

```
Blueprint (game logic)
      │
      ▼
SimpleUPNPBlueprintLibrary / xxxCallbackProxy (UAddPortCallbackProxy, UGetPortListCallbackProxy, ...)
      │   Built on BlueprintAsyncActionBase - provides result callbacks to Blueprint via a Latent node
      ▼
UUPNPModule (a GameInstanceSubsystem-like class - owns/manages the FDiscoverDevices singleton's lifetime)
      │
      ▼
FDiscoverDevices (FRunnable)
      │   - Worker thread: SSDP multicast send/receive
      │   - Game thread: HTTP (SOAP) request/parsing, state machine
      ▼
SSDP (UDP Multicast) + SOAP over HTTP  ──▶  Router (IGD)
```

- **`FDiscoverDevices`**: the core class that owns the socket, thread, HTTP, and state machine. It inherits `FRunnable` to handle receiving SSDP responses non-blockingly on a dedicated thread.
- **The `*CallbackProxy` family** (`UAddPortCallbackProxy`, `URemovePortCallbackProxy`, `UGetPortListCallbackProxy`, `UPerformAllDevicesCallbackProxy`): these inherit `UBlueprintAsyncActionBase` and wrap the flow so that a **single Latent node call** in Blueprint — like `Add Port` or `Get Port List` — naturally carries through from "add/remove port" to a "success/failure" callback. The goal is to let a designer attach P2P opening with a single node, without needing to know the UPnP protocol.
- **`UPerformAllDevicesCallbackProxy`**: higher-level orchestration logic that iterates over every device when there are multiple IGDs on the network (e.g., a dual-router setup) and attempts port mapping on each.
- **`FSimpleUPNPInfo` / `FUPNPDeviceInfo`**: declared as `USTRUCT(BlueprintType)`, exposing the mapping info (protocol, port, internal IP, etc.) and the device list so they can be handled directly as values in the Blueprint graph.

```cpp
// AddPortCallbackProxy.cpp — An asynchronous flow that completes with a single Blueprint node
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

## 4. Key Feature Summary

- **Automatic IGD discovery**: discovers routers (IGDs) on the network via SSDP multicast, and supports multiple devices
- **Add/remove port mapping**: automatically opens TCP/UDP ports via the `AddPortMapping` / `DeletePortMapping` SOAP actions
- **External IP / connection status lookup**: checks the current public IP and line status via `GetExternalIPAddress`, `GetStatusInfo`, `GetCommonLinkProperties`, etc.
- **Current mapping list lookup**: `GetGenericPortMappingEntry` lists all currently registered port mappings
- **Blueprint support**: every feature is exposed as `BlueprintCallable` / a Latent node, usable without C++
- **Automatic retry & timeout**: automatically retries up to a maximum count when a request stage fails
- **Asynchronous thread handling**: keeps socket send/receive off the game thread so it runs without dropping frames

---

## 5. Tech Stack

- **Engine**: Unreal Engine (C++ plugin, built on `IModuleInterface`)
- **Networking**: UDP multicast socket (`FUdpSocketBuilder`), Unreal's `Http` module (SOAP/HTTP)
- **Concurrency**: `FRunnable` + `FRunnableThread`, with `FCriticalSection` synchronizing state across threads
- **Blueprint integration**: `UBlueprintAsyncActionBase`, `USTRUCT(BlueprintType)`, multicast delegates
- **Protocols**: SSDP (UPnP discovery), SOAP over HTTP (UPnP control), UPnP IGD:1/2 standards

---

## 6. Reference Specifications

- UPnP Device Architecture v1.1 / v2.0 — http://upnp.org/specs/arch/
- WANIPConnection:1 Service — http://upnp.org/specs/gw/UPnP-gw-WANIPConnection-v1-Service.pdf
- SSDP overview — https://wiki.wireshark.org/SSDP
