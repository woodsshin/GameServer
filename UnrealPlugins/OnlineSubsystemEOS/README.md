# OnlineSubsystemEOS — Unreal Engine EOS Online Subsystem Plugin

Unreal Engine Marketplace에 등록된 **native code plugin**입니다.
Epic Online Services(EOS)를 언리얼 엔진의 표준 `OnlineSubsystem` 인터페이스로 wrapping하여, 기존에 Steam·PSN 등 다른 플랫폼을 대상으로 구현된 온라인 로직(로그인, 세션/matchmaking, 업적, 친구 등)을 **코드 수정 없이 EOS로 교체**할 수 있게 해주는 것이 목적입니다.

---

## 1. 왜 필요한가 — OnlineSubsystem 추상화와 EOS

Unreal Engine은 `IOnlineSubsystem`이라는 공통 인터페이스로 로그인, 세션, 친구, 업적, P2P 네트워킹 같은 온라인 기능을 추상화합니다. 게임 로직은 `Online::GetSubsystem()`으로 받아온 인터페이스만 호출하면 되고, 실제 구현체가 Steam인지 PSN인지는 몰라도 됩니다.

Epic Online Services는 플랫폼에 상관없이 crossplay 인증·matchmaking·P2P NAT 통과를 제공하는 Epic의 백엔드 서비스입니다. 이 플러그인은 EOS SDK를 이 `OnlineSubsystem` 규격에 맞춰 구현체(`FOnlineSubsystemEOS`)로 등록해서, 게임 코드 입장에서는 여느 플랫폼 서브시스템과 똑같이 다뤄지도록 만듭니다.

```
게임 로직 (Blueprint / C++)
        │  IOnlineSubsystem, IOnlineSession, IOnlineIdentity ...
        ▼
FOnlineSubsystemEOS  ─── 각 Interface 구현체 (Identity/Session/Friends/Achievements ...)
        │
        ▼
EOS SDK (EOS_Platform_*, EOS_P2P_*, EOS_Auth_* ...)
```

핵심 가치는 **이식성**입니다. `DefaultEngine.ini`에서 `DefaultPlatformService=EOS` 한 줄만 바꾸면 동일한 블루프린트/C++ 코드가 그대로 EOS 위에서 동작합니다.

---

## 2. 모듈/플러그인 초기화 흐름

### 2-1. 모듈 시작 — EOS SDK 동적 로딩

`FOnlineSubsystemEOSModule::StartupModule()`은 플러그인 로드 시점에 플랫폼별 EOS SDK 바이너리(`EOSSDK-Win64-Shipping.dll` 등)를 동적으로 로드하고, 팩토리를 `OnlineSubsystem` 모듈에 등록합니다.

```cpp
// OnlineSubsystemEOSModule.cpp
void FOnlineSubsystemEOSModule::StartupModule()
{
    bool bEnabled = true;
    GConfig->GetBool(TEXT("OnlineSubsystemEOS"), TEXT("bEnabled"), bEnabled, GEngineIni);
    if (!bEnabled) return;

    // 플랫폼별 EOS SDK 동적 라이브러리 로드
    const FString LibName = TEXT("EOSSDK-Win64-Shipping");
    if (!LoadDependency(LibDir, LibName, EOSSDKHandle))
    {
        UE_LOG(EOSOSSLog, Warning, TEXT("Failed to load UEOS plugin. Plug-in will not be functional"));
        FreeDependency(EOSSDKHandle);
    }

    EOSFactory = new FOnlineFactoryEOS();
    // "EOS"라는 이름으로 OnlineSubsystem 레지스트리에 팩토리 등록
    FOnlineSubsystemModule* OSS = FModuleManager::GetModulePtr<FOnlineSubsystemModule>("OnlineSubsystem");
    if (OSS)
        OSS->RegisterPlatformService(EOS_SUBSYSTEM, EOSFactory);
}
```

이렇게 등록해두면 `DefaultEngine.ini`의 `DefaultPlatformService=EOS` 설정만으로 엔진이 `FOnlineSubsystemEOS` 인스턴스를 만들어 씁니다.

### 2-2. EOS SDK 초기화 — 플랫폼 핸들 생성

`FOnlineSubsystemEOS::InitEOS()`는 EOS SDK를 초기화하고, 프로젝트 세팅에서 읽어온 ProductId/SandboxId/DeploymentId/ClientId로 `PlatformHandle`을 생성합니다. 이 핸들이 이후 모든 EOS API 호출(인증, 세션, P2P 등)의 진입점이 됩니다.

```cpp
// OnlineSubsystemEOS.cpp
EOS_EResult FOnlineSubsystemEOS::InitEOS()
{
    LoadConfigurations();

    EOS_InitializeOptions SDKOptions;
    SDKOptions.ApiVersion = EOS_INITIALIZE_API_LATEST;
    SDKOptions.ProductName = ProductNameUTF8.Get();
    SDKOptions.ProductVersion = ProductVersionUTF8.Get();
    EOS_Initialize(&SDKOptions);

    EOS_Platform_Options PlatformOptions;
    PlatformOptions.ProductId = ProductIdUTF8.Get();
    PlatformOptions.SandboxId = SandboxIdUTF8.Get();
    PlatformOptions.DeploymentId = DeploymentIdUTF8.Get();
    PlatformOptions.ClientCredentials.ClientId = ClientIdUTF8.Get();
    PlatformOptions.ClientCredentials.ClientSecret = ClientSecretUTF8.Get();
    PlatformOptions.bIsServer = IsRunningDedicatedServer() ? EOS_TRUE : EOS_FALSE;

    PlatformHandle = EOS_Platform_Create(&PlatformOptions);
    ...
    bEOSInitialized = true;
    return EOS_EResult::EOS_Success;
}
```

`FOnlineSubsystemEOS::Init()`은 이 핸들을 바탕으로 `IOnlineIdentity`, `IOnlineSession`, `IOnlineFriends`, `IOnlineAchievements` 등 각 EOS 인터페이스 구현체를 생성하고, 소켓 서브시스템(`CreateEOSSocketSubsystem`)까지 함께 초기화합니다. 매 틱마다 `EOS_Platform_Tick`을 호출해 SDK 내부 콜백 큐를 흘려보냅니다.

```cpp
bool FOnlineSubsystemEOS::Init()
{
    if (InitEOS() != EOS_EResult::EOS_Success) return false;

    IdentityInterface     = MakeShareable(new FOnlineIdentityInterfaceEOS(this));
    SessionInterface      = MakeShareable(new FOnlineSessionEOS(this));
    FriendsEOSPtr         = MakeShareable(new FOnlineFriendsEOS(this));
    AchievementsEOSPtr    = MakeShareable(new FOnlineAchievementsEOS(this));
    ...
    CreateEOSSocketSubsystem();
    return true;
}
```

---

## 3. EOS P2P 기반 네트워킹 — NAT 통과 & 릴레이 폴백

이 플러그인의 가장 큰 특징은 **UE의 소켓/네트워크 드라이버 계층 자체를 EOS P2P로 교체**한다는 점입니다. 즉 `Sockets`, `SocketSubsystem`, `NetDriver`, `NetConnection`까지 EOS 버전으로 구현해서, 게임 코드는 평소처럼 `ClientTravel`이나 리슨 서버 접속을 호출할 뿐인데 실제 패킷은 EOS의 NAT 통과 P2P(내부적으로 릴레이 폴백 포함)로 오갑니다.

### 3-1. 주소 체계 — IP 대신 EOS ID

일반 소켓은 IPv4 주소로 대상을 지정하지만, EOS P2P는 상대방을 `ProductUserId`로 식별합니다. `FInternetAddrEOS`는 엔진의 `FInternetAddr` 인터페이스를 그대로 흉내 내면서 내부적으로는 EOS ID + 채널(포트 역할)을 담습니다.

```cpp
// IPAddressEOS.cpp — "eos.<EOSID>:<채널>" 형태 문자열을 파싱
void FInternetAddrEOS::SetIp(const TCHAR* InAddr, bool& bIsValid)
{
    FString EOSIPAddrStr = InAddrStr.StartsWith(EOS_URL_PREFIX)
        ? InAddrStr.Mid(UE_ARRAY_COUNT(EOS_URL_PREFIX) - 1)
        : InAddrStr;

    FString EOSIPStr, EOSChannelStr;
    if (EOSIPAddrStr.Split(":", &EOSIPStr, &EOSChannelStr, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
    {
        EOSId = FUniqueNetIdEOS(EOSIPStr);
        EOSChannel = FCString::Atoi(*EOSChannelStr);
        bIsValid = true;
    }
    ...
}
```

`eos.` 접두사(`EOS_URL_PREFIX`)가 붙은 URL은 EOS 소켓 경로로, 그렇지 않으면 일반 IP 소켓(passthrough)으로 분기됩니다 — 이 판단은 `NetDriver`/`NetConnection` 양쪽에서 일관되게 이뤄집니다.

### 3-2. NetDriver — EOS 소켓과 IP 소켓의 이중 경로

`UEOSNetDriver`는 `UIpNetDriver`를 상속받아 접속 URL에 따라 EOS 소켓 또는 일반 IP 소켓 중 하나를 선택합니다. LAN 매치처럼 `bIsLanMatch` 옵션이 있으면 기존 IP 경로(passthrough)로 흘려보내고, 그 외에는 EOS P2P 소켓을 생성합니다.

```cpp
// EOSNetDriver.cpp
bool UEOSNetDriver::InitListen(FNetworkNotify* InNotify, FURL& ListenURL, bool bReuseAddressAndPort, FString& Error)
{
    ISocketSubsystem* EOSSockets = ISocketSubsystem::Get(EOS_SUBSYSTEM);
    if (EOSSockets && !ListenURL.HasOption(TEXT("bIsLanMatch")))
    {
        FName SocketTypeName = IsRunningDedicatedServer() ? FName(TEXT("EOSServerSocket")) : FName(TEXT("EOSClientSocket"));
        SetSocketAndLocalAddress(EOSSockets->CreateSocket(SocketTypeName, TEXT("Unreal server (EOS)"), FNetworkProtocolTypes::EOS));
    }
    else
    {
        bIsPassthrough = true; // 소켓은 베이스 클래스(UIpNetDriver)가 생성
    }

    FSocketEOS* EOSSocket = (FSocketEOS*)GetSocket();
    if (EOSSocket)
        EOSSocket->SubscribeToConnectionRequests(); // 들어오는 P2P 연결 요청 리스닝 시작

    return Super::InitListen(InNotify, ListenURL, bReuseAddressAndPort, Error);
}
```

`GetSocketSubsystem()`도 `bIsPassthrough` 여부에 따라 `EOS_SUBSYSTEM` 또는 `PLATFORM_SOCKETSUBSYSTEM`을 반환해, 상위 엔진 코드가 두 경로를 구분하지 않고 동일하게 다룰 수 있게 합니다.

### 3-3. P2P 소켓 — SendPacket / ReceivePacket

`FSocketEOS::SendTo`/`RecvFrom`은 UDP 소켓 API 시그니처를 그대로 구현하지만, 내부는 EOS의 `EOS_P2P_SendPacket` / `EOS_P2P_ReceivePacket`을 호출합니다. 로그인 전에 소켓이 먼저 만들어질 수 있는 상황(타이틀 화면에서 리슨 서버를 미리 여는 경우 등)을 위해 **지연 인증**(delayed authentication) 처리가 들어 있습니다.

```cpp
// SocketsEOS.cpp
bool FSocketEOS::SendTo(const uint8* Data, int32 Count, int32& BytesSent, const FInternetAddr& Destination)
{
    // 로그인 전 생성된 리슨 소켓 대비 - 최초 전송 시점에 인증 상태 확정
    if (!bAuthenticated)
    {
        FProductUserId LocalProductId = UOnlineSubsystemEOSFunctionLibrary::GetProductUserId();
        if (!LocalProductId.IsValid()) return false;
        LocalEOSId = FUniqueNetIdEOS(UOnlineSubsystemEOSFunctionLibrary::GetAccountId().ToString());
        bAuthenticated = true;
        SubscribeToConnectionRequests();
    }

    EOS_P2P_SendPacketOptions Options;
    Options.LocalUserId = UOnlineSubsystemEOSFunctionLibrary::GetProductUserId();
    Options.RemoteUserId = FProductUserId::FromString(EOSDest.ToString(false));
    Options.Channel = EOSDest.GetPort();
    Options.bAllowDelayedDelivery = EOS_TRUE;      // NAT 통과 완료 전까지 패킷을 큐잉
    Options.Reliability = EOS_EPacketReliability::EOS_PR_ReliableOrdered;
    Options.Data = Data;
    Options.DataLengthBytes = Count;

    return EOS_P2P_SendPacket(P2PHandle, &Options) == EOS_EResult::EOS_Success;
}
```

수신 측에서 새 연결 요청이 들어오면 `OnIncomingConnectionRequest` 콜백에서 소켓 이름(`"GAME"`)을 확인한 뒤 `EOS_P2P_AcceptConnection`으로 즉시 수락합니다. NAT 통과가 실패하는 환경에서는 `[OnlineSubsystemEOS] bAllowP2PPacketRelay=true` 설정에 따라 **EOS 릴레이 서버를 통한 자동 폴백**이 이뤄져, 순수 P2P가 불가능한 네트워크에서도 접속이 끊기지 않도록 합니다.

### 3-4. 연결 수명 관리 — Dead Connection & Linger

여러 유저의 P2P 세션/채널을 계속 추적해야 하므로, `FSocketSubsystemEOS`는 살아있는 연결(`AcceptedConnections`)과 종료 대기 중인 연결(`DeadConnections`)을 분리 관리합니다. 연결이 끊기면 즉시 삭제하지 않고 `P2PCleanupTimeout` 동안 유예(linger)를 둬서, 뒤늦게 도착하는 패킷 때문에 막 재접속한 유저가 실수로 킥되는 경쟁 상태를 방지합니다.

```cpp
// SocketSubsystemEOS.cpp
bool FSocketSubsystemEOS::P2PTouch(EOS_HP2P P2PHandle, const FUniqueNetIdEOS& SessionId, int32 ChannelId)
{
    // 종료 대기 중인 세션에서 온 갱신은 무시
    if (!IsConnectionPendingRemoval(SessionId, ChannelId))
    {
        FEOSP2PConnectionInfo& ChannelUpdate = AcceptedConnections.FindOrAdd(SessionId);
        ChannelUpdate.P2PHandle = P2PHandle;
        if (ChannelId != -1)
            ChannelUpdate.AddOrUpdateChannel(ChannelId, FPlatformTime::Seconds());
        return true;
    }
    return false;
}

void FSocketSubsystemEOS::UnregisterConnection(UEOSNetConnection* Connection)
{
    // GC와 정상 종료가 겹쳐 두 번 호출되는 경우를 방지: 실제로 제거됐을 때만 처리
    if (EOSConnections.RemoveSingleSwap(ObjectPtr) == 1 && Connection->GetRemoteAddr().IsValid())
    {
        TSharedPtr<const FInternetAddrEOS> EOSAddr = StaticCastSharedPtr<const FInternetAddrEOS>(Connection->GetRemoteAddr());
        P2PRemove(EOSAddr->EOSId, EOSAddr->EOSChannel);
    }
}
```

`UEOSNetConnection`도 접속 URL이 `eos.` 접두사인지에 따라 passthrough 여부를 판단하고, EOS 경로일 때는 소켓 서브시스템에 자신을 등록해 위 수명 관리 로직에 편입시킵니다.

```cpp
// EOSNetConnection.cpp
void UEOSNetConnection::InitLocalConnection(UNetDriver* InDriver, FSocket* InSocket, const FURL& InURL, ...)
{
    bIsPassthrough = InURL.Host.StartsWith(EOS_URL_PREFIX) ? false : true;
    if (!bIsPassthrough)
        DisableAddressResolution(); // EOS ID는 DNS 확인 대상이 아님

    Super::InitLocalConnection(InDriver, InSocket, InURL, InState, InMaxPacket, InPacketOverhead);
    if (!bIsPassthrough && RemoteAddr.IsValid())
    {
        FSocketSubsystemEOS* SocketSubsystem = (FSocketSubsystemEOS*)ISocketSubsystem::Get(EOS_SUBSYSTEM);
        if (SocketSubsystem)
            SocketSubsystem->RegisterConnection(this);
    }
}
```

---

## 4. 세션(matchmaking)과 P2P 주소의 결합

EOS Sessions 인터페이스는 EOS Metrics와 연동되어 matchmaking에 쓰이며, 세션 검색 결과에는 접속에 필요한 두 종류의 주소가 함께 실립니다.

| 세션 키 | 의미 |
|---|---|
| `BUCKETID` | 세션 검색을 위한 최상위 필터링 정보(맵/모드 등) |
| `HOSTADDRESS` | 호스트의 공인 IP (passthrough 접속용) |
| `P2PADDRESS` | `eos.<EOSID>:<채널>` 형식의 EOS P2P 접속 문자열 |

클라이언트는 세션에서 받은 `P2PADDRESS`를 그대로 `eos.` URL로 사용해 `ClientTravel`을 호출하면, 위에서 설명한 `UEOSNetDriver`/`FSocketEOS` 경로를 타고 NAT 통과 P2P로 접속하게 됩니다. 별도의 포트포워딩이나 전용 릴레이 서버 구축 없이도 크로스플랫폼 P2P 세션이 성립하는 구조입니다.

---

## 5. 블루프린트 지원

`OnlineSubsystemEOS`는 기존 `OnlineSubsystem` 인터페이스를 오버라이드하는 방식이라, 서드파티 **AdvancedSessionsPlugin** 등 기존 블루프린트 세션 노드들을 그대로 재사용할 수 있습니다. 추가로 로그인 상태 확인처럼 자주 쓰는 기능은 최소한의 블루프린트 함수로 노출했습니다.

```cpp
// OnlineSubsystemEOSFunctionLibrary.cpp
bool UOnlineSubsystemEOSFunctionLibrary::IsAuthorised()
{
    FOnlineIdentityInterfaceEOS* OnlineIdentityEOSPtr =
        (FOnlineIdentityInterfaceEOS*) Online::GetIdentityInterface(EOS_SUBSYSTEM).Get();
    return OnlineIdentityEOSPtr ? OnlineIdentityEOSPtr->IsAuthorised() : false;
}
```

- `Login User` (Dev Auth Tool / Password / ExchangeCode / DeviceCode / AccountPortal 지원) → `Is Authorised`로 상태 확인
- 로그인 성공 시 `Set Cached Unique Net Id`로 `PlayerController`/`PlayerState`에 EOS 고유 ID 반영
- 지원 인터페이스: Authentication, Connect, Metrics, Friends, NAT P2P, Presence, Sessions, User Info

---

## 6. 설정 (`DefaultEngine.ini`)

```ini
[OnlineSubsystem]
DefaultPlatformService=EOS

[OnlineSubsystemEOS]
bEnabled=true
bAllowP2PPacketRelay=true      ; NAT 통과 실패 시 EOS 릴레이로 자동 폴백
P2PConnectionTimeout=90
P2PCleanupTimeout=1.5
bUseEOSNetworking=true
LoginCredentialType=DevTool

[/Script/Engine.GameEngine]
+NetDriverDefinitions=(DefName="GameNetDriver",DriverClassName="OnlineSubsystemEOS.EOSNetDriver",DriverClassNameFallback="OnlineSubsystemEOS.EOSNetDriver")

[/Script/OnlineSubsystemEOS.EOSNetDriver]
NetConnectionClassName=OnlineSubsystemEOS.EOSNetConnection
```

`ProductId` / `SandboxId` / `DeploymentId` / `ClientId` / `ClientSecret`은 프로젝트 세팅의 **Epic Online Service** 패널에서 EOS Dev Portal 발급 값으로 채워 넣습니다.

---

## 7. 아키텍처 요약

```
Blueprint / Game Code
        │  IOnlineSubsystem::Get("EOS")
        ▼
FOnlineSubsystemEOS ── Identity / Session / Friends / Achievements / UserCloud / Stats ...
        │
        ├─▶ EOS SDK (EOS_Platform_*, EOS_Auth_*, EOS_Sessions_* ...)
        │
        └─▶ FSocketSubsystemEOS ── FSocketEOS (EOS_P2P_SendPacket/ReceivePacket)
                    │                    │
                    │                    ▼
                    │            EOS_P2P_AddNotifyPeerConnectionRequest (수신 연결 승인)
                    ▼
            UEOSNetDriver / UEOSNetConnection (UIpNetDriver/UIpConnection 상속)
                    │
                    ▼
            게임 세션 접속 — NAT 통과 P2P, 실패 시 EOS 릴레이로 자동 폴백
```

- **`FOnlineSubsystemEOS`**: EOS SDK 플랫폼 핸들 소유, 각 온라인 인터페이스 팩토리 역할
- **`FSocketSubsystemEOS` / `FSocketEOS`**: UE 소켓 API를 EOS P2P로 매핑, 연결 수명(연결/해제/유예) 관리
- **`UEOSNetDriver` / `UEOSNetConnection`**: `UIpNetDriver`/`UIpConnection`을 상속해 EOS 경로와 일반 IP 경로(passthrough)를 URL 기준으로 자동 분기
- **`FInternetAddrEOS`**: IP 대신 EOS ID + 채널을 담는 주소 표현, `FInternetAddr` 인터페이스 호환

---

## 8. 지원 버전

| 플러그인 버전 | EOS SDK 버전 |
|---|---|
| UE 4.25 | EOS SDK 1.7 |
| UE 4.24 | EOS SDK 1.7 |
| UE 4.23 | EOS SDK 1.7 |

---

## 9. 참고 링크

- EOS SDK ELoginCredentialType — https://dev.epicgames.com/docs/services/en-US/API/EOS/EOS/EOS_ELoginCredentialType/index.html
- EOS SDK Dev Auth Tool — https://dev.epicgames.com/docs/services/en-US/DeveloperAuthenticationTool/index.html
- Epic Account Services — https://dev.epicgames.com/docs/services/en-US/EpicAccountServices/index.html
