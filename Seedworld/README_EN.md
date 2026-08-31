# Seedworld — Online & Multiplayer Systems (Unreal Engine 5)

This README compiles a selection of core code samples from the online/multiplayer backend integration modules designed and implemented for the **Seedworld** project, curated for portfolio review. It covers real-time multiplayer infrastructure spanning both client and server: AWS GameLift–based dedicated server orchestration, a gRPC-based matchmaking client, and a replication-driven team system.

> Engine: Unreal Engine 5 (C++) · Backend integrations: AWS GameLift, gRPC (TurboLink), EOS (Epic Online Services), AdvancedSessions Plugin

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Modules & Code Samples](#modules--code-samples)
   - [gRPC Subsystem — Service Discovery & Token Management](#1-grpc-subsystem--service-discovery--token-management)
   - [Matchmaking Callback Proxies — Async gRPC Wrappers](#2-matchmaking-callback-proxies--async-grpc-wrappers)
   - [GameLift Game Mode — Dedicated Server Session Orchestration](#3-gamelift-game-mode--dedicated-server-session-orchestration)
   - [GameLift Server Object — SDK Wrapping & Session State Management](#4-gamelift-server-object--sdk-wrapping--session-state-management)
   - [Online Game Mode — Session Create/Update](#5-online-game-mode--session-createupdate)
   - [BTK Team System — Replication-Based Team Management](#6-btk-team-system--replication-based-team-management)
3. [Design Highlights](#design-highlights)

---

## Architecture Overview

Seedworld is designed to support two matchmaking / server-deployment paths simultaneously.

- **P2P / Advanced Sessions path**: `ASeedworldOnlineGameModeBase` creates and updates online subsystem (EOS) sessions via `AdvancedSessionsLibrary`
- **AWS GameLift path**: `ASeedworldGameLiftGameMode` registers the dedicated server into a match using the GameLift SDK (`USeedworldGameLiftServerObject`) together with a custom gRPC matchmaking service (`StartMatchMakingDS`, `CreateGameSessionDS`)

Both paths share the following layered structure:

```
[Client/Server GameMode]
        │
        ▼
[Callback Proxy (UObject, BlueprintAsyncAction pattern)]
        │
        ▼
[SeedworldGrpcSubsystem / SeedworldDSGrpcSubsystem]  ← service discovery + auth token management
        │
        ▼
[TurboLink gRPC Client] → matchmaking / session management backend
```

The client-only (`USeedworldGrpcSubsystem`) and dedicated-server-only (`USeedworldDSGrpcSubsystem`) subsystems are created mutually exclusively via `ShouldCreateSubsystem`, and both follow the same "service discovery → authenticate → connect gRPC service" flow, differing only in authentication method (EOS ID token vs. OAuth client-credentials).

---

## Modules & Code Samples

### 1. gRPC Subsystem — Service Discovery & Token Management

**Files**: `SeedworldGrpcSubsystem.cpp`, `SeedworldDSGrpcSubsystem.cpp`

Per-client and per-server game instance subsystems that resolve gRPC endpoints at runtime via HTTP (service discovery) and lazily connect requested services through a queuing structure. Even when the same service is requested from multiple places concurrently, delegates are collected in a `TMultiMap` and all fired at once the moment the service reaches the `Ready` state.

```cpp
void USeedworldGrpcSubsystem::ConnectRequestedServices()
{
    UTurboLinkGrpcManager* TurboLinkManager = UTurboLinkGrpcUtilities::GetTurboLinkGrpcManager(GetWorld());
    auto TokenProviderFunc = [this]() { return GetAuthToken(); };
    TurboLinkManager->SetGlobalAccessTokenProvider(TokenProviderFunc);

    TArray<FString> RequestedServicesNames;
    RequestedServices.GetKeys(RequestedServicesNames);
    for (const FString& ServiceName : RequestedServicesNames)
    {
        if (Services.Find(ServiceName) != nullptr) continue; // connection already requested

        TObjectPtr<UGrpcService> RequestedServicePtr = TurboLinkManager->MakeService(ServiceName);
        RequestedServicePtr->SetAccessTokenProvider(TokenProviderFunc);
        Services.Add(ServiceName, RequestedServicePtr);
        RequestedServicePtr->Connect(); // non-blocking signal

        if (RequestedServicePtr->GetServiceState() == EGrpcServiceState::Ready)
        {
            ProcessConnectedService(ServiceName, RequestedServicePtr);
        }
        RequestedServicePtr->OnServiceStateChanged.AddDynamic(this, &USeedworldGrpcSubsystem::OnServiceStateChanged);
    }
}

void USeedworldGrpcSubsystem::ProcessConnectedService(const FString& ServiceName, TObjectPtr<UGrpcService> Service)
{
    TArray<FRequestServiceDelegate> Delegates;
    RequestedServices.MultiFind(ServiceName, Delegates);
    for (const auto& Delegate : Delegates)
    {
        Delegate.ExecuteIfBound(Service); // broadcast to every pending requester
    }
    RequestedServices.Remove(ServiceName);
}
```

On the dedicated-server side (`SeedworldDSGrpcSubsystem`), tokens are refreshed periodically via OAuth client-credentials, with an automatic refresh scheduled 60 seconds before expiry.

```cpp
void USeedworldDSGrpcSubsystem::OnRefreshTokenResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
    constexpr float ErrorRefreshInterval = 5.0f;
    if (!bWasSuccessful) { ScheduleRefresh(ErrorRefreshInterval); return; }

    if (EHttpResponseCodes::IsOk(Response->GetResponseCode()))
    {
        // parse JSON, then store access_token
        float NextRefreshInterval = DefaultRefreshTokenInterval;
        if (JsonObject->HasField(TEXT("expires_in")))
        {
            const int32 ExpireInSeconds = JsonObject->GetNumberField(TEXT("expires_in"));
            NextRefreshInterval = (float)ExpireInSeconds - 60.0f; // refresh proactively, 60s before expiry
        }
        AccessToken = JsonObject->GetStringField(TEXT("access_token"));
        ScheduleRefresh(NextRefreshInterval);
        ConnectRequestedServices();
    }
    else
    {
        ScheduleRefresh(ErrorRefreshInterval); // retry after 5 seconds on failure
    }
}
```

---

### 2. Matchmaking Callback Proxies — Async gRPC Wrappers

**Files**: `StartMatchMakingCallbackProxy.cpp`, `CreatePlayerSessionCallbackProxy.cpp`, `CreateGameSessionCallbackProxy.cpp`, `CreateGameSessionDSCallbackProxy.cpp`, `StartMatchMakingDSCallbackProxy.cpp`

A family of proxy objects that follow Unreal's `UBlueprintAsyncActionBase` pattern, wrapping asynchronous gRPC calls in `OnSuccess`/`OnFail` multicast delegates. Every proxy shares the same lifecycle: request service → create client → initialize RPC context → bind response → clean up (delegates cleared in `BeginDestroy`).

```cpp
void UStartMatchMakingCallbackProxy::OnGetMatchMakingSvc(UGrpcService* Service)
{
    MatchMakingSvc = Cast<UMatchMakingSvc>(Service);
    if (IsValid(MatchMakingSvc))
    {
        MatchMakingClient = MatchMakingSvc->MakeClient();
        HandleStartMatchMaking = MatchMakingClient->InitStartMatchMaking();
        MatchMakingClient->OnStartMatchMakingResponse.AddUniqueDynamic(this, &UStartMatchMakingCallbackProxy::OnStartMatchMakingResponse);

        FGrpcMatchMakingStartMatchMakingRequest Request;
        Request.PlayerId = ReqStartMatchMaking.PlayerId;
        Request.GameMode = ReqStartMatchMaking.GameMode;
        Request.LatencyInMs = ReqStartMatchMaking.LatencyInMs;
        MatchMakingClient->StartMatchMaking(HandleStartMatchMaking, Request);
    }
    else
    {
        OnFail.Broadcast({});
    }
}

void UStartMatchMakingCallbackProxy::OnStartMatchMakingResponse(FGrpcContextHandle Handle, const FGrpcResult& GrpcResult, const FGrpcMatchMakingStartMatchMakingReponse& GrpcResponse)
{
    ClearDelegate();
    if (GrpcResult.Code == EGrpcResultCode::Ok)
    {
        // pick out the player session that belongs to "me" from the matched ticket
        for (const auto& PlayerSession : GrpcResponse.Ticket.GameSessionConnectionInfo.MatchedPlayerSessions)
        {
            if (ReqStartMatchMaking.PlayerId.Equals(PlayerSession->PlayerId))
            {
                FResStartMatchMaking Response;
                Response.PlayerSessionId = PlayerSession->PlayerSessionId;
                Response.IpAddress = GrpcResponse.Ticket.GameSessionConnectionInfo.IpAddress;
                Response.Port = GrpcResponse.Ticket.GameSessionConnectionInfo.Port;
                OnSuccess.Broadcast(Response);
                return;
            }
        }
    }
    OnFail.Broadcast({});
}
```

In `WITH_EDITOR` builds, every proxy consistently short-circuits straight to `OnFail` without making a real backend call — this cuts off unnecessary network dependencies during editor PIE testing.

```cpp
void UCreatePlayerSessionCallbackProxy::Activate()
{
    UGameInstance* GameInstance = ((UGameEngine*)GEngine)->GameInstance;
    if (!GameInstance) { OnFail.Broadcast({}); return; }
#if WITH_EDITOR
    OnFail.Broadcast({});
#else
    auto SeedworldGrpc = GameInstance->GetSubsystem<USeedworldGrpcSubsystem>();
    SeedworldGrpc->RequestService<UMatchMakingSvc>(
        FRequestServiceDelegate::CreateUObject(this, &UCreatePlayerSessionCallbackProxy::OnGetMatchMakingSvc));
#endif
}
```

---

### 3. GameLift Game Mode — Dedicated Server Session Orchestration

**File**: `SeedworldGameLiftGameMode.cpp`

The game mode for the dedicated server that communicates with AWS GameLift. On boot, the server polls its own matchmaking backend to "register this server into a match," and validates the player's session with the GameLift SDK on connect.

**The server registers itself into the match pool — repeats polling until successful:**

```cpp
void ASeedworldGameLiftGameMode::RequestCreateGameSession()
{
    FString Region;
    if (!FParse::Value(FCommandLine::Get(), TEXT("region="), Region, true)) return;

    // retry on a fixed interval until this server is assigned
    GetWorld()->GetTimerManager().SetTimer(
        MatchMakingServivceConnectTimerHandle, this,
        &ASeedworldGameLiftGameMode::RequestCreateGameSession,
        MatchMakingDSWaitingTime, true);

    FGrpcMatchMakingCreateGameSessionDSRequest Request;
    Request.CreatorId = TEXT("Dedicated_Server");
    Request.MaximumPlayerSessionCount = MaxPlayers;
    Request.Region = Region;
    Request.GameProperties.Add(TEXT("maxplayers"), FString::Printf(TEXT("%d"), MaxPlayers));
    Request.GameProperties.Add(TEXT("region"), Region);

    UCreateGameSessionDSCallbackProxy* CreateGameSessionDS = UCreateGameSessionDSCallbackProxy::CreateGameSessionDS(Request);
    CreateGameSessionDS->OnSuccess.AddDynamic(this, &ASeedworldGameLiftGameMode::OnCreateGameSessionDSResponse);
    CreateGameSessionDS->OnFail.AddDynamic(this, &ASeedworldGameLiftGameMode::OnCreateGameSessionDSFailed);
    CreateGameSessionDS->Activate();
}
```

**Validating a connecting player's GameLift session ticket (acts as an auth gate in `PreLogin`):**

```cpp
void ASeedworldGameLiftGameMode::PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
    Super::PreLogin(Options, Address, UniqueId, ErrorMessage);
    if (!IsEnabledGameLift()) return;

    // connect URL: ip:port?playersessionid=psess-af9e4c27-...
    const FString LowerOptions = Options.ToLower();
    FString PlayerSessionId = UGameplayStatics::ParseOption(LowerOptions, TEXT("playersessionid"));

    EGameLiftErrorType ErrorType;
    if (SeedworldGameLiftServerObject->AcceptPlayerSession(PlayerSessionId, ErrorType, ErrorMessage))
    {
        PlayerSessions.Add(UniqueId, PlayerSessionId); // kept around to release the session on Logout
    }
    else
    {
        UE_LOG(SeedworldGameLiftGameModeLog, Error, TEXT("Failed to accept PlayerSession : ... ErrorType %d"), ErrorType);
    }
}
```

The key detail on disconnect is that `Logout` releases the GameLift session *before* the `PlayerState` is destroyed — i.e., while `UniqueId` can still be resolved.

---

### 4. GameLift Server Object — SDK Wrapping & Session State Management

**File**: `SeedworldGameLiftServerObject.cpp`

A thin wrapper around the GameLift Server SDK (`UGameLiftServerObject`) that absorbs the race condition where a "start session" request arrives before SDK initialization completes, using a pending flag.

```cpp
bool USeedworldGameLiftServerObject::StartGameSession(const FServerGameSession& Session)
{
    bAssignedGame = true;
    CachedGameSession = Session;
    bool bSuccess = Super::StartGameSession(Session);
    OnCreateGameSessionlegate.ExecuteIfBound(Session);
    return bSuccess;
}

void USeedworldGameLiftServerObject::CompleteStartSession(bool bSuccess)
{
    if (bSuccess)
    {
        if (bHasPendingCreateGameSession)
        {
            bHasPendingCreateGameSession = false;
            Super::StartGameSession(CachedGameSession); // replay the deferred session start
        }
    }
    else
    {
        DestroyGameLiftServer();
    }
}
```

---

### 5. Online Game Mode — Session Create/Update

**File**: `SeedworldOnlineGameModeBase.cpp`

The top-level game mode for the P2P/Advanced Sessions path. Its core logic assembles 20+ pieces of session metadata — `MaxPlayers`, map name, build version, region, public IP, and more — into an `FSessionPropertyKeyPair` array that gets tagged onto the EOS session. Session creation and update (`CreateGameSession` / `UpdateGameSession`) are designed to share the same property-assembly logic, minimizing duplication.

```cpp
void ASeedworldOnlineGameModeBase::CreateGameSession()
{
    if (CreateSessionCallbackProxyAdvanced) return; // guard against re-entry while already in progress

    TArray<FSessionPropertyKeyPair> ExtraSettings = GetExtraSessionSettings(); // per-subclass properties like GAMEMODE
    FGameDataDescription GameDataDescription = SeedworldHelperSystem->GetGameDataDescription();

    ExtraSettings.Add(UAdvancedSessionsLibrary::MakeLiteralSessionPropertyString(TEXT("MAPNAME"), MapName));
    // ... BUILDVERSION, PUBLICIP, REGION, GAME_SESSION_ID, and more are appended ...

    CreateSessionCallbackProxyAdvanced = UCreateSessionCallbackProxyAdvanced::CreateAdvancedSession(
        this, ExtraSettings, nullptr, ServerInformation.MaxPlayers, /*...*/);
    CreateSessionCallbackProxyAdvanced->OnSuccess.AddDynamic(this, &ASeedworldOnlineGameModeBase::OnCreateSessionSuccess);
    CreateSessionCallbackProxyAdvanced->OnFailure.AddDynamic(this, &ASeedworldOnlineGameModeBase::OnCreateSessionFailed);
    CreateSessionCallbackProxyAdvanced->Activate();
}
```

**Boot sequence from public IP lookup through to EOS login:**

```cpp
void ASeedworldOnlineGameModeBase::InitGameServer()
{
    ServerInformation.BuildVersion = Version.Version.ToString();
    ServerInformation.ConnectionString.Port = 27115;

    FString PublicIP = UOnlineSubsystemSeedworldFunctionLibrary::GetPublicIP();
    if (PublicIP.IsEmpty())
    {
        RequestGetPublicIP(); // GetPublicIPCallbackProxy → checkip.amazonaws.com
    }
    else
    {
        SetPublicIP(PublicIP);
#if !WITH_EDITOR
        LoginEOS();
#endif
    }
}
```

The public IP is fetched by a dedicated proxy (`UGetPublicIPCallbackProxy`) calling `https://checkip.amazonaws.com/`; if a cached value is already available, the lookup is skipped and the flow proceeds straight to login.

---

### 6. BTK Team System — Replication-Based Team Management

**Files**: `SeedworldBTKGameStateBase.cpp`, `SeedworldBTKPlayerState.cpp`, `SeedworldBTKTeamSubsystem.cpp`

A server-authoritative (Server RPC) system for team creation, invitations, and role management. `GameStateBase` acts as the source of truth for the team array (`BTKTeams`); `PlayerState` replicates its own team reference; and a local client subsystem (`USeedworldBTKTeamSubsystem`) broadcasts events to the UI.

**Server: validating a team-creation request (checks for existing membership/duplicate name before spawning):**

```cpp
void ASeedworldBTKGameStateBase::Server_CreateNewTeam_Implementation(const FString& BTKTeamsName, APlayerState* Creator)
{
    ASeedworldBTKPlayerState* BTKPlayerState = Cast<ASeedworldBTKPlayerState>(Creator);
    FString UniqueIDString = BTKPlayerState->GetUniqueId()->ToString();

    TObjectPtr<ABTKTeam> BTKTeam;
    if (FindTeamByUniqueID(UniqueIDString, BTKTeam, &MemberInfo))
    {
        BTKPlayerController->Client_CreateTeamResult(EBTKTeamResult::Already_Has_Team, nullptr);
        return;
    }
    if (FindTeamByTeamName(BTKTeamsName, BTKTeam))
    {
        BTKPlayerController->Client_CreateTeamResult(EBTKTeamResult::Existing_TeamName, nullptr);
        return;
    }

    ABTKTeam* NewBTKTeam = GetWorld()->SpawnActor<ABTKTeam>();
    NewBTKTeam->CreateTeam(BTKTeamsName, Creator);
    BTKTeams.Add(NewBTKTeam);
    OnRep_BTKTeams();
    ForceNetUpdate();

    BTKPlayerController->Client_CreateTeamResult(EBTKTeamResult::OK, NewBTKTeam);
}
```

**Client: `OnRep_Team` checks whether this is the local player before propagating the event to the UI subsystem** (avoids broadcast noise from other players' team changes):

```cpp
void ASeedworldBTKPlayerState::OnRep_Team()
{
    if (IsLocalPlayerState())
    {
        auto SeedworldBTKTeamSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<USeedworldBTKTeamSubsystem>();
        if (Team)
            SeedworldBTKTeamSubsystem->OnTeamJoined.Broadcast(EBTKTeamResult::OK, Team);
        else
            SeedworldBTKTeamSubsystem->OnTeamLeft.Broadcast();
    }
}

bool ASeedworldBTKPlayerState::IsLocalPlayerState()
{
    APlayerController* LocalPlayerController = GetWorld()->GetFirstPlayerController();
    return LocalPlayerController && LocalPlayerController->PlayerState == this;
}
```

**Client subsystem: looking up teammates' pawns** (loosely coupled via the team interface):

```cpp
TArray<APawn*> USeedworldBTKTeamSubsystem::GetTeamMemberPawns()
{
    TArray<APawn*> TeamPawns;
    ABTKTeam* MyTeam;
    if (!GetMyTeam(MyTeam) || !IsValid(MyTeam)) return TeamPawns;

    int32 LocalTeamID = MyTeam->GetTeamID();
    for (APlayerState* PlayerState : GetWorld()->GetGameState()->PlayerArray)
    {
        ASeedworldBTKPlayerState* OtherPlayerState = Cast<ASeedworldBTKPlayerState>(PlayerState);
        if (OtherPlayerState && ISeedworldBTKTeamInterface::Execute_GetTeamID(OtherPlayerState) == LocalTeamID)
        {
            if (APawn* PlayerPawn = PlayerState->GetPawn())
                TeamPawns.Add(PlayerPawn);
        }
    }
    return TeamPawns;
}
```

---

## Design Highlights

| Area | Design Point |
|---|---|
| **Async communication pattern** | Every gRPC/HTTP call is unified as a `UObject` proxy with `Activate()` plus `OnSuccess`/`OnFail` delegates → exposed to Blueprint, reusable across call sites |
| **Service discovery** | Endpoints are resolved via HTTP at boot rather than hardcoded, falling back to project-settings defaults on failure |
| **Auth separation** | Clients use an EOS ID token; dedicated servers use OAuth client-credentials — refreshed automatically 60s before expiry |
| **Editor safety net** | The `WITH_EDITOR` macro cuts off real backend calls during PIE testing at the source |
| **Race-condition guard** | If GameLift's `StartGameSession` arrives before SDK init completes, a pending flag absorbs it and replays it later |
| **Retry strategy** | Failed matchmaking/session-creation calls auto-retry via timer-based polling (at `MatchMakingDSWaitingTime` intervals) |
| **Network authority separation** | The team system mutates state only through Server RPCs; clients reflect the result via `OnRep_*` — a local-player check avoids unnecessary broadcasts |
| **Resource cleanup** | Every callback proxy explicitly clears its delegates in `BeginDestroy()` to prevent dangling bindings |

---

*This document is compiled from snippets extracted from an actual production codebase, summarized for portfolio review purposes.*
