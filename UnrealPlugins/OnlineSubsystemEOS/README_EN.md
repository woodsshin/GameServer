# OnlineSubsystemEOS — Unreal Engine EOS Online Subsystem Plugin

A **native code plugin** registered on the Unreal Engine Marketplace.
It wraps Epic Online Services (EOS) behind Unreal Engine's standard `OnlineSubsystem` interface, with the goal of letting online logic (login, session/matchmaking, achievements, friends, etc.) that was originally implemented for other platforms such as Steam or PSN **be swapped over to EOS with no code changes**.

---

## 1. Why This Is Needed — OnlineSubsystem Abstraction and EOS

Unreal Engine abstracts online functionality — login, sessions, friends, achievements, P2P networking — behind a common interface called `IOnlineSubsystem`. Game logic only needs to call the interface obtained via `Online::GetSubsystem()`, without needing to know whether the actual implementation underneath is Steam or PSN.

Epic Online Services is Epic's backend service that provides crossplay authentication, matchmaking, and P2P NAT traversal regardless of platform. This plugin registers the EOS SDK as an implementation (`FOnlineSubsystemEOS`) conforming to this `OnlineSubsystem` specification, so that from the game code's perspective it's treated exactly like any other platform subsystem.

```
Game Logic (Blueprint / C++)
        │  IOnlineSubsystem, IOnlineSession, IOnlineIdentity ...
        ▼
FOnlineSubsystemEOS  ─── individual interface implementations (Identity/Session/Friends/Achievements ...)
        │
        ▼
EOS SDK (EOS_Platform_*, EOS_P2P_*, EOS_Auth_* ...)
```

The core value here is **portability**. Changing a single line — `DefaultPlatformService=EOS` — in `DefaultEngine.ini` is enough for the exact same Blueprint/C++ code to run on top of EOS.

---

## 2. Module/Plugin Initialization Flow

### 2-1. Module Startup — Dynamically Loading the EOS SDK

`FOnlineSubsystemEOSModule::StartupModule()` dynamically loads the platform-specific EOS SDK binary (e.g., `EOSSDK-Win64-Shipping.dll`) at plugin load time, and registers a factory with the `OnlineSubsystem` module.

```cpp
// OnlineSubsystemEOSModule.cpp
void FOnlineSubsystemEOSModule::StartupModule()
{
    bool bEnabled = true;
    GConfig->GetBool(TEXT("OnlineSubsystemEOS"), TEXT("bEnabled"), bEnabled, GEngineIni);
    if (!bEnabled) return;

    // Load the platform-specific EOS SDK dynamic library
    const FString LibName = TEXT("EOSSDK-Win64-Shipping");
    if (!LoadDependency(LibDir, LibName, EOSSDKHandle))
    {
        UE_LOG(EOSOSSLog, Warning, TEXT("Failed to load UEOS plugin. Plug-in will not be functional"));
        FreeDependency(EOSSDKHandle);
    }

    EOSFactory = new FOnlineFactoryEOS();
    // Register the factory in the OnlineSubsystem registry under the name "EOS"
    FOnlineSubsystemModule* OSS = FModuleManager::GetModulePtr<FOnlineSubsystemModule>("OnlineSubsystem");
    if (OSS)
        OSS->RegisterPlatformService(EOS_SUBSYSTEM, EOSFactory);
}
```

With this registration in place, simply setting `DefaultPlatformService=EOS` in `DefaultEngine.ini` is enough for the engine to create and use an `FOnlineSubsystemEOS` instance.

### 2-2. EOS SDK Initialization — Creating the Platform Handle

`FOnlineSubsystemEOS::InitEOS()` initializes the EOS SDK and creates a `PlatformHandle` using the ProductId/SandboxId/DeploymentId/ClientId read from the project settings. This handle becomes the entry point for every subsequent EOS API call (authentication, sessions, P2P, etc.).

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

Based on this handle, `FOnlineSubsystemEOS::Init()` creates the implementations for each EOS interface — `IOnlineIdentity`, `IOnlineSession`, `IOnlineFriends`, `IOnlineAchievements`, and so on — and also initializes the socket subsystem (`CreateEOSSocketSubsystem`) alongside them. `EOS_Platform_Tick` is called every tick to flush the SDK's internal callback queue.

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

## 3. EOS P2P-Based Networking — NAT Traversal & Relay Fallback

The biggest characteristic of this plugin is that it **replaces UE's socket/network driver layer itself with EOS P2P**. In other words, `Sockets`, `SocketSubsystem`, `NetDriver`, and even `NetConnection` are all implemented as EOS-specific versions — so the game code just calls `ClientTravel` or connects to a listen server as usual, while the actual packets travel over EOS's NAT-traversing P2P (which internally includes relay fallback).

### 3-1. Addressing Scheme — EOS ID Instead of IP

A regular socket specifies its target with an IPv4 address, but EOS P2P identifies the other party by `ProductUserId`. `FInternetAddrEOS` fully mimics the engine's `FInternetAddr` interface while internally carrying an EOS ID plus a channel (acting as the port).

```cpp
// IPAddressEOS.cpp — parses a string in the form "eos.<EOSID>:<channel>"
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

A URL carrying the `eos.` prefix (`EOS_URL_PREFIX`) is routed down the EOS socket path; otherwise it branches to a regular IP socket (passthrough) — and this determination is made consistently on both the `NetDriver` and `NetConnection` sides.

### 3-2. NetDriver — Dual Paths for EOS Sockets and IP Sockets

`UEOSNetDriver` inherits from `UIpNetDriver` and chooses either an EOS socket or a regular IP socket depending on the connection URL. When the `bIsLanMatch` option is present, as with a LAN match, it's routed down the existing IP path (passthrough); otherwise an EOS P2P socket is created.

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
        bIsPassthrough = true; // The base class (UIpNetDriver) creates the socket
    }

    FSocketEOS* EOSSocket = (FSocketEOS*)GetSocket();
    if (EOSSocket)
        EOSSocket->SubscribeToConnectionRequests(); // Start listening for incoming P2P connection requests

    return Super::InitListen(InNotify, ListenURL, bReuseAddressAndPort, Error);
}
```

`GetSocketSubsystem()` also returns either `EOS_SUBSYSTEM` or `PLATFORM_SOCKETSUBSYSTEM` depending on `bIsPassthrough`, so that the higher-level engine code can treat both paths identically without needing to distinguish between them.

### 3-3. P2P Socket — SendPacket / ReceivePacket

`FSocketEOS::SendTo`/`RecvFrom` fully implement the standard UDP socket API signatures, but internally they call EOS's `EOS_P2P_SendPacket` / `EOS_P2P_ReceivePacket`. To handle the case where a socket may be created before login (such as when a listen server is opened in advance from the title screen), **delayed authentication** handling is included.

```cpp
// SocketsEOS.cpp
bool FSocketEOS::SendTo(const uint8* Data, int32 Count, int32& BytesSent, const FInternetAddr& Destination)
{
    // Accounts for a listen socket created before login — authentication state is finalized at the first send
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
    Options.bAllowDelayedDelivery = EOS_TRUE;      // Queues packets until NAT traversal completes
    Options.Reliability = EOS_EPacketReliability::EOS_PR_ReliableOrdered;
    Options.Data = Data;
    Options.DataLengthBytes = Count;

    return EOS_P2P_SendPacket(P2PHandle, &Options) == EOS_EResult::EOS_Success;
}
```

When a new connection request comes in on the receiving side, the `OnIncomingConnectionRequest` callback checks the socket name (`"GAME"`) and immediately accepts it via `EOS_P2P_AcceptConnection`. In environments where NAT traversal fails, an **automatic fallback through the EOS relay server** occurs based on the `[OnlineSubsystemEOS] bAllowP2PPacketRelay=true` setting, ensuring connections aren't dropped even on networks where pure P2P isn't possible.

### 3-4. Connection Lifecycle Management — Dead Connections & Linger

Because it needs to continuously track the P2P sessions/channels of multiple users, `FSocketSubsystemEOS` manages live connections (`AcceptedConnections`) and connections awaiting termination (`DeadConnections`) separately. When a connection drops, it isn't deleted immediately — a grace period (linger) of `P2PCleanupTimeout` is applied instead, preventing a race condition where a late-arriving packet accidentally kicks a user who has just reconnected.

```cpp
// SocketSubsystemEOS.cpp
bool FSocketSubsystemEOS::P2PTouch(EOS_HP2P P2PHandle, const FUniqueNetIdEOS& SessionId, int32 ChannelId)
{
    // Ignore updates coming from a session that is already pending removal
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
    // Prevents double handling when GC and normal shutdown overlap: only processed once actually removed
    if (EOSConnections.RemoveSingleSwap(ObjectPtr) == 1 && Connection->GetRemoteAddr().IsValid())
    {
        TSharedPtr<const FInternetAddrEOS> EOSAddr = StaticCastSharedPtr<const FInternetAddrEOS>(Connection->GetRemoteAddr());
        P2PRemove(EOSAddr->EOSId, EOSAddr->EOSChannel);
    }
}
```

`UEOSNetConnection` also determines whether it's in passthrough mode based on whether the connection URL has the `eos.` prefix, and when it's on the EOS path, it registers itself with the socket subsystem to be brought into the lifecycle management logic described above.

```cpp
// EOSNetConnection.cpp
void UEOSNetConnection::InitLocalConnection(UNetDriver* InDriver, FSocket* InSocket, const FURL& InURL, ...)
{
    bIsPassthrough = InURL.Host.StartsWith(EOS_URL_PREFIX) ? false : true;
    if (!bIsPassthrough)
        DisableAddressResolution(); // An EOS ID is not subject to DNS resolution

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

## 4. Combining Sessions (Matchmaking) with P2P Addresses

The EOS Sessions interface is used for matchmaking in conjunction with EOS Metrics, and session search results carry two kinds of addresses needed for connecting.

| Session Key | Meaning |
|---|---|
| `BUCKETID` | Top-level filtering information for session search (map/mode, etc.) |
| `HOSTADDRESS` | The host's public IP (for passthrough connections) |
| `P2PADDRESS` | An EOS P2P connection string in the form `eos.<EOSID>:<channel>` |

When a client takes the `P2PADDRESS` received from a session and uses it directly as an `eos.` URL to call `ClientTravel`, it connects via NAT-traversing P2P, following the `UEOSNetDriver`/`FSocketEOS` path described above. This establishes a structure for cross-platform P2P sessions without needing separate port forwarding or a dedicated relay server.

---

## 5. Blueprint Support

Because `OnlineSubsystemEOS` works by overriding the existing `OnlineSubsystem` interface, existing Blueprint session nodes — such as those from the third-party **AdvancedSessionsPlugin** — can be reused as-is. In addition, frequently used features such as checking login status are exposed through a minimal set of Blueprint functions.

```cpp
// OnlineSubsystemEOSFunctionLibrary.cpp
bool UOnlineSubsystemEOSFunctionLibrary::IsAuthorised()
{
    FOnlineIdentityInterfaceEOS* OnlineIdentityEOSPtr =
        (FOnlineIdentityInterfaceEOS*) Online::GetIdentityInterface(EOS_SUBSYSTEM).Get();
    return OnlineIdentityEOSPtr ? OnlineIdentityEOSPtr->IsAuthorised() : false;
}
```

- `Login User` (supports Dev Auth Tool / Password / ExchangeCode / DeviceCode / AccountPortal) → check status with `Is Authorised`
- On successful login, `Set Cached Unique Net Id` reflects the EOS unique ID onto the `PlayerController`/`PlayerState`
- Supported interfaces: Authentication, Connect, Metrics, Friends, NAT P2P, Presence, Sessions, User Info

---

## 6. Configuration (`DefaultEngine.ini`)

```ini
[OnlineSubsystem]
DefaultPlatformService=EOS

[OnlineSubsystemEOS]
bEnabled=true
bAllowP2PPacketRelay=true      ; Automatically fall back to EOS relay when NAT traversal fails
P2PConnectionTimeout=90
P2PCleanupTimeout=1.5
bUseEOSNetworking=true
LoginCredentialType=DevTool

[/Script/Engine.GameEngine]
+NetDriverDefinitions=(DefName="GameNetDriver",DriverClassName="OnlineSubsystemEOS.EOSNetDriver",DriverClassNameFallback="OnlineSubsystemEOS.EOSNetDriver")

[/Script/OnlineSubsystemEOS.EOSNetDriver]
NetConnectionClassName=OnlineSubsystemEOS.EOSNetConnection
```

`ProductId` / `SandboxId` / `DeploymentId` / `ClientId` / `ClientSecret` are filled in with the values issued by the EOS Dev Portal, entered in the **Epic Online Service** panel under the project settings.

---

## 7. Architecture Summary

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
                    │            EOS_P2P_AddNotifyPeerConnectionRequest (accepting incoming connections)
                    ▼
            UEOSNetDriver / UEOSNetConnection (inherits UIpNetDriver/UIpConnection)
                    │
                    ▼
            Game session connection — NAT-traversing P2P, with automatic fallback to EOS relay on failure
```

- **`FOnlineSubsystemEOS`**: Owns the EOS SDK platform handle and acts as the factory for each online interface
- **`FSocketSubsystemEOS` / `FSocketEOS`**: Maps the UE socket API onto EOS P2P, and manages connection lifecycle (connect/disconnect/linger)
- **`UEOSNetDriver` / `UEOSNetConnection`**: Inherits from `UIpNetDriver`/`UIpConnection` and automatically branches between the EOS path and the regular IP path (passthrough) based on the URL
- **`FInternetAddrEOS`**: An address representation carrying an EOS ID plus channel instead of an IP, compatible with the `FInternetAddr` interface

---

## 8. Supported Versions

| Plugin Version | EOS SDK Version |
|---|---|
| UE 4.25 | EOS SDK 1.7 |
| UE 4.24 | EOS SDK 1.7 |
| UE 4.23 | EOS SDK 1.7 |

---

## 9. Reference Links

- EOS SDK ELoginCredentialType — https://dev.epicgames.com/docs/services/en-US/API/EOS/EOS/EOS_ELoginCredentialType/index.html
- EOS SDK Dev Auth Tool — https://dev.epicgames.com/docs/services/en-US/DeveloperAuthenticationTool/index.html
- Epic Account Services — https://dev.epicgames.com/docs/services/en-US/EpicAccountServices/index.html
