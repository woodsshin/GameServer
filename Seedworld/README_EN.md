# Seedworld — Online & Multiplayer Systems (Unreal Engine 5)

This README compiles a selection of core code samples from the online/multiplayer backend integration modules designed and implemented for the **Seedworld**([https://x.com/SeedworldMeta](https://x.com/SeedworldMeta)) project, curated for portfolio review. It covers real-time multiplayer infrastructure spanning both client and server: AWS GameLift–based dedicated server orchestration, a gRPC-based matchmaking client, and a replication-driven team system.

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
   - [BTK Team System — Replication-Based In-Game Team Management](#6-btk-team-system--replication-based-in-game-team-management)
   - [Push Model Replication — Explicit Dirty-Marking Based Change Detection](#7-push-model-replication--explicit-dirty-marking-based-change-detection)
   - [Custom Replication Graph — Per-Class Node Routing](#8-custom-replication-graph--per-class-node-routing)
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

### 6. BTK Team System — Replication-Based In-Game Team Management

**Files**: `SeedworldBTKGameStateBase.cpp`, `SeedworldBTKPlayerState.cpp`, `SeedworldBTKTeamSubsystem.cpp`, `BTKTeam.cpp`

A server-authoritative (Server RPC) system for team creation, invitations, and role management. `GameStateBase` acts as the authoritative state for the team array (`BTKTeams`), and each team is spawned as its own replicated `ABTKTeam` Actor that owns its member list, roles, invitations, and team chat. `PlayerState` replicates its own team reference, and a local client subsystem (`USeedworldBTKTeamSubsystem`) broadcasts events to the UI.

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

**Team Actor: invitation → acceptance flow** (checks Captain/Lieutenant permission, guards against existing membership and duplicate invites, then adds to `PendingMembers`):

```cpp
void ABTKTeam::SendInvitation(APlayerState* NewMember, APlayerState* Requester)
{
    ASeedworldBTKPlayerState* RequesterState = Cast<ASeedworldBTKPlayerState>(Requester);
    if (RequesterState->GetTeamRole() != EBTKTeamRole::Captain && RequesterState->GetTeamRole() != EBTKTeamRole::Lieutenant)
    {
        UE_LOG(SeedworldBTKTeamLog, Warning, TEXT("ABTKTeam::SendInvitation : Not allowed requester : Team role %d"), RequesterState->GetTeamRole());
        return;
    }

    if (FindMember(NewMember) || FindPendingMember(NewMember))
    {
        return; // already a member, or an invitation is already pending
    }

    FBTKTeamMemberInfo NewMemberInfo;
    NewMemberInfo.UniqueIDString = NewMember->GetUniqueId()->ToString();
    NewMemberInfo.TeamRole = EBTKTeamRole::Member;
    NewMemberInfo.PlayerName = NewMember->GetPlayerName();
    PendingMembers.Add(NewMemberInfo);
    OnRep_PendingMembers();
}

EBTKTeamResult ABTKTeam::AcceptTeamInvitation(const FString& UniqueIDString)
{
    if (!HasAuthority()) return EBTKTeamResult::No_Authority;

    FBTKTeamMemberInfo* MemberInfo = FindPendingMemberByUniqueID(UniqueIDString);
    if (!MemberInfo) return EBTKTeamResult::Not_Found_Player;

    MemberInfo->Status = EBTKPlayerStatus::Online;
    Members.Add(*MemberInfo); // promote from pending to full member

    const int32 NewCount = Algo::RemoveIf(PendingMembers, [&](const FBTKTeamMemberInfo& Info)
    {
        return Info.UniqueIDString.Equals(UniqueIDString);
    });
    PendingMembers.SetNum(NewCount);
    OnRep_PendingMembers();

    return EBTKTeamResult::OK;
}
```

**Team Actor: kicking a member & transferring captaincy** (kicking clears the team reference on the target's `PlayerState`; captaincy transfer must be initiated by the current Captain, who is automatically demoted to Lieutenant):

```cpp
void ABTKTeam::KickMemberByUniqueId(const FString& KickUniqueIDString, APlayerState* Requester)
{
    ASeedworldBTKPlayerState* RequesterState = Cast<ASeedworldBTKPlayerState>(Requester);
    if (RequesterState->GetTeamRole() != EBTKTeamRole::Captain && RequesterState->GetTeamRole() != EBTKTeamRole::Lieutenant)
    {
        return; // only Captain/Lieutenant can kick
    }

    const int32 NewCount = Algo::RemoveIf(Members, [&](const FBTKTeamMemberInfo& MemberInfo)
    {
        return MemberInfo.UniqueIDString.Equals(KickUniqueIDString);
    });
    Members.SetNum(NewCount);
    OnRep_Members();
    ForceNetUpdate();

    // clear the team reference on the kicked player's PlayerState (found by scanning PlayerArray)
    for (APlayerState* KickPlayerState : GetWorld()->GetGameState<ASeedworldBTKGameStateBase>()->PlayerArray)
    {
        if (KickPlayerState && KickPlayerState->GetUniqueId().IsValid() &&
            KickPlayerState->GetUniqueId()->ToString().Equals(KickUniqueIDString))
        {
            if (ASeedworldBTKPlayerState* BTKPlayerState = Cast<ASeedworldBTKPlayerState>(KickPlayerState))
            {
                BTKPlayerState->SetTeam(nullptr);
            }
            break;
        }
    }
}

void ABTKTeam::SetRoleByUniqueId(const FString& UniqueIDString, EBTKTeamRole NewRole, APlayerState* Requester)
{
    ASeedworldBTKPlayerState* RequesterState = Cast<ASeedworldBTKPlayerState>(Requester);
    if (RequesterState->GetTeamRole() != EBTKTeamRole::Captain) return; // only Captain can change roles

    FBTKTeamMemberInfo* MemberInfo = FindMemberByUniqueID(UniqueIDString);
    if (!MemberInfo) return;
    MemberInfo->TeamRole = NewRole;
    // ... the same role is mirrored onto the target's PlayerState ...

    // when captaincy is transferred, the requester is automatically demoted to Lieutenant
    if (NewRole == EBTKTeamRole::Captain)
    {
        FBTKTeamMemberInfo* RequesterMemberInfo = FindMemberByUniqueID(RequesterState->GetUniqueId()->ToString());
        if (RequesterMemberInfo)
        {
            RequesterMemberInfo->TeamRole = EBTKTeamRole::Lieutenant;
            RequesterState->SetTeamRole(EBTKTeamRole::Lieutenant);
        }
    }
    OnRep_Members();
    ForceNetUpdate();
}
```

**Team Actor: team chat** (validates membership before appending a message; deletion is Captain-only):

```cpp
void ABTKTeam::SendTeamChatMessage(const FString& Message, APlayerState* Requester)
{
    if (!FindMember(Requester) || Message.IsEmpty()) return; // only team members can chat

    FBTKTeamChatMessage ChatMessage;
    ChatMessage.ChatIndex = ++NextChatIndex;
    ChatMessage.Message = Message;
    ChatMessage.SenderName = Requester->GetPlayerName();
    ChatMessage.Timestamp = FDateTime::UtcNow();
    ChatMessages.AddMessage(ChatMessage);
    OnRep_ChatMessages(); // reflect the new message in the client UI
}

void ABTKTeam::DeleteChatMessage(int32 MessageIndex, APlayerState* Requester)
{
    if (ASeedworldBTKPlayerState* RequesterState = Cast<ASeedworldBTKPlayerState>(Requester))
    {
        if (RequesterState->GetTeamRole() == EBTKTeamRole::Captain && ChatMessages.Messages.IsValidIndex(MessageIndex))
        {
            ChatMessages.RemoveMessage(MessageIndex);
            OnRep_ChatMessages();
        }
    }
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

### 7. Push Model Replication — Explicit Dirty-Marking Based Change Detection

**Files**: `BTKTeam.h/.cpp`, `SeedworldBTKGameStateBase.h/.cpp`, `SeedworldBTKPlayerState.h/.cpp`

Every replicated property in the BTK team system (`ABTKTeam::TeamID/TeamName/Members/PendingMembers`, `ASeedworldBTKGameStateBase::BTKTeams`, `ASeedworldBTKPlayerState::Team/TeamRole`) is registered under Push Model. Default replication scans every replicated property each frame and diffs it against its previous value to find what changed; Push Model skips that per-frame comparison entirely and instead has each write site report itself dirty directly. Since the per-frame diff cost grows with the number of actors and properties, this is a good fit for team data, which updates infrequently but from many different call sites.

**Registration — opting into Push Model via `DOREPLIFETIME_WITH_PARAMS_FAST` + `FDoRepLifetimeParams`:**

```cpp
void ABTKTeam::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    // Registering via the _WITH_PARAMS_FAST macro family excludes the property from the engine's
    // per-frame dirty-compare, so it depends solely on the explicit MARK_PROPERTY_DIRTY_FROM_NAME
    // calls made in code.
    FDoRepLifetimeParams SharedParams;
    SharedParams.Condition = COND_None;
    SharedParams.RepNotifyCondition = REPNOTIFY_Always;

    DOREPLIFETIME_WITH_PARAMS_FAST(ABTKTeam, TeamID, SharedParams);
    DOREPLIFETIME_WITH_PARAMS_FAST(ABTKTeam, TeamName, SharedParams);
    DOREPLIFETIME_WITH_PARAMS_FAST(ABTKTeam, Members, SharedParams);
    DOREPLIFETIME_WITH_PARAMS_FAST(ABTKTeam, PendingMembers, SharedParams);

    // ChatMessages uses FFastArraySerializer's own delta-serialization (NetDeltaSerialize) and runs
    // on a track independent of Push Model — MarkArrayDirty() already signals changes, so it keeps
    // the existing macro.
    DOREPLIFETIME_CONDITION_NOTIFY(ABTKTeam, ChatMessages, COND_None, REPNOTIFY_Always);
}
```

**Direct assignment — mark dirty in the same function, right after the write:**

```cpp
void ASeedworldBTKPlayerState::SetTeamRole_Implementation(EBTKTeamRole NewTeamRole)
{
    TeamRole = NewTeamRole;
    MARK_PROPERTY_DIRTY_FROM_NAME(ASeedworldBTKPlayerState, TeamRole, this);
    OnRep_TeamRole(); // OnRep isn't invoked automatically on the server itself, so it's called manually right away
}
```

**In-place mutation of array elements — when only a member field is changed through a raw pointer returned by `Find*`, Push Model has no way to detect the change, so it must be marked manually:**

```cpp
EBTKTeamResult ABTKTeam::AcceptTeamInvitation(const FString& UniqueIDString)
{
    FBTKTeamMemberInfo* MemberInfo = FindPendingMemberByUniqueID(UniqueIDString);
    if (!MemberInfo) return EBTKTeamResult::Not_Found_Player;

    // Modifying a field on an array element directly through a pointer — since this isn't an
    // Add/Remove on the array itself, the engine can't pick it up automatically. Mark
    // PendingMembers dirty explicitly.
    MemberInfo->Status = EBTKPlayerStatus::Online;
    MARK_PROPERTY_DIRTY_FROM_NAME(ABTKTeam, PendingMembers, this);

    Members.Add(*MemberInfo); // This one is an Add, so Members needs its own separate marking too
    MARK_PROPERTY_DIRTY_FROM_NAME(ABTKTeam, Members, this);
    OnRep_Members();
    ForceNetUpdate();
    // ...
}
```

`SetRoleByUniqueId` (which does two in-place edits when transferring captaincy — the target member, and the requester who gets demoted) and `SetPlayerStatus` (toggling online/offline status) follow the same pattern. Every point that patches a field through a pointer is paired with its own `MARK_PROPERTY_DIRTY_FROM_NAME` call.

**Add/Remove on the array itself — `GameStateBase` owns the source-of-truth team array and marks it dirty at team creation/teardown:**

```cpp
// When a new team is spawned
BTKTeams.Add(NewBTKTeam);
MARK_PROPERTY_DIRTY_FROM_NAME(ASeedworldBTKGameStateBase, BTKTeams, this);
OnRep_BTKTeams();
ForceNetUpdate();

// When the last member leaves and the team becomes empty
BTKTeams.Remove(BTKTeam);
MARK_PROPERTY_DIRTY_FROM_NAME(ASeedworldBTKGameStateBase, BTKTeams, this);
OnRep_BTKTeams();
ForceNetUpdate();
```

The `BTKTeams` array itself only needs to be reported dirty on Add/Remove; each individual `ABTKTeam` actor's own fields, like `TeamName` and `Members`, replicate independently through that actor's own Push Model marking. A change to an actor's internal state does not, by itself, dirty the array that holds it.

**Summary of the design rules:**

| Situation | Handling |
|---|---|
| Plain scalar assignment (`TeamID = ...`, `TeamRole = ...`) | Call `MARK_PROPERTY_DIRTY_FROM_NAME` right after the assignment, in the same spot |
| Array `Add`/`Remove`/`SetNum` | Mark the array property with `MARK_PROPERTY_DIRTY_FROM_NAME` right after the call |
| Editing an array element's field via a `Find*` pointer | Explicitly mark that array property at the point the value is changed through the pointer (the engine can't detect it) |
| `FFastArraySerializer`-based properties (`ChatMessages`) | Excluded from Push Model; keep `MarkArrayDirty()` plus the existing `DOREPLIFETIME_CONDITION_NOTIFY` |
| Refreshing the server's own UI/logic | Dirty marking only triggers network transmission, so call `OnRep_*` directly whenever the server needs the change reflected locally right away |

Each header also carries a comment directly above the property declaration — "every write site for this property must call `MARK_PROPERTY_DIRTY_FROM_NAME`" — so the convention sits right next to the code and isn't missed when a new write site gets added later.

---

### 8. Custom Replication Graph — Per-Class Node Routing

**Files**: `SeedworldReplicationGraph.h`, `SeedworldReplicationGraph.cpp`

A subclass of the stock `UReplicationGraph` that explicitly routes each actor class to a replication node. The class → node mapping is cached in an `enum`-based lookup table (`ClassRoutingMap`); on Add/Remove, that mapping is looked up and the actor is delegated to the matching node.

```cpp
UENUM()
enum class ESeedworldRepGraphClassNodeMapping : uint8
{
    NotRouted,              // unregistered class — anything left unrouted will not replicate
    AlwaysRelevant,         // always relevant to every connection, with no notion of space
    Spatialized_Dynamic,    // grid node, position refreshed every frame
    Spatialized_Static,     // grid node, actors that never move once placed
    Spatialized_Dormant,    // grid node + dormancy, no classes registered yet (reserved)
};
```

**Registering per-class routing — `bAlwaysRelevant` actors go to the non-spatial node, Pawns to the cull-distance-driven dynamic grid:**

```cpp
void USeedworldReplicationGraph::InitGlobalActorClassSettings()
{
    Super::InitGlobalActorClassSettings();
    ClassRoutingMap.Reset();

    // ABTKTeam: bAlwaysRelevant = true, no spatial meaning → non-spatial always-relevant node
    ClassRoutingMap.Add(ABTKTeam::StaticClass(), ESeedworldRepGraphClassNodeMapping::AlwaysRelevant);
    GlobalActorReplicationInfoMap.SetClassInfo(ABTKTeam::StaticClass(), FClassReplicationInfo());

    // ASeedworldCharacter_BTK (Pawn): moves continuously → Spatialized_Dynamic
    ClassRoutingMap.Add(ASeedworldCharacter_BTK::StaticClass(), ESeedworldRepGraphClassNodeMapping::Spatialized_Dynamic);
    const ASeedworldCharacter_BTK* CDO = GetDefault<ASeedworldCharacter_BTK>();

    FClassReplicationInfo ClassInfo;
    ClassInfo.ReplicationPeriodFrame = GetReplicationPeriodFrameForFrequency(CDO->GetNetUpdateFrequency());
    if (CDO->bAlwaysRelevant || CDO->bOnlyRelevantToOwner)
    {
        ClassInfo.SetCullDistanceSquared(0.f); // always-relevant actors ignore cull distance
    }
    else
    {
        ClassInfo.SetCullDistanceSquared(CDO->GetNetCullDistanceSquared());
    }
    GlobalActorReplicationInfoMap.SetClassInfo(ASeedworldCharacter_BTK::StaticClass(), ClassInfo);

    // ASeedworldDebugTeleportPoint: fixed once placed → Spatialized_Static
    ClassRoutingMap.Add(ASeedworldDebugTeleportPoint::StaticClass(), ESeedworldRepGraphClassNodeMapping::Spatialized_Static);
    GlobalActorReplicationInfoMap.SetClassInfo(ASeedworldDebugTeleportPoint::StaticClass(), FClassReplicationInfo());
}
```

**Add/Remove routing — delegates to `AlwaysRelevantNode` or `GridNode` (dynamic/static) according to the mapping; an unregistered class produces a warning log instead of failing silently:**

```cpp
void USeedworldReplicationGraph::RouteAddNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo)
{
    const ESeedworldRepGraphClassNodeMapping Mapping = GetClassNodeMapping(ActorInfo.Actor->GetClass());

    switch (Mapping)
    {
    case ESeedworldRepGraphClassNodeMapping::AlwaysRelevant:
        AlwaysRelevantNode->NotifyAddNetworkActor(ActorInfo);
        break;
    case ESeedworldRepGraphClassNodeMapping::Spatialized_Dynamic:
        GridNode->AddActor_Dynamic(ActorInfo, GlobalInfo);
        break;
    case ESeedworldRepGraphClassNodeMapping::Spatialized_Static:
        GridNode->AddActor_Static(ActorInfo, GlobalInfo);
        break;
    case ESeedworldRepGraphClassNodeMapping::NotRouted:
    default:
        // record that an actor which was never added to a node will not replicate
        UE_LOG(SeedworldRepGraphLog, Warning,
            TEXT("%s (class %s) has no registered routing and will NOT replicate. Add it to InitGlobalActorClassSettings."),
            *ActorInfo.Actor->GetName(), *ActorInfo.Actor->GetClass()->GetName());
        break;
    }
}
```

**Resolving the mapping by walking the inheritance chain — designed so a subclass inherits its parent's routing without a registration of its own:**

```cpp
ESeedworldRepGraphClassNodeMapping USeedworldReplicationGraph::GetClassNodeMapping(UClass* Class) const
{
    for (UClass* CurrentClass = Class; CurrentClass; CurrentClass = CurrentClass->GetSuperClass())
    {
        if (const ESeedworldRepGraphClassNodeMapping* Found = ClassRoutingMap.Find(CurrentClass))
        {
            return *Found;
        }
    }
    return ESeedworldRepGraphClassNodeMapping::NotRouted;
}
```

`PlayerState` and `PlayerController` need no class registration of their own — they are covered by the engine's per-connection `AlwaysRelevantForConnection` path (`USeedworldReplicationGraphNode_AlwaysRelevantForConnection`). Since that is owner-only replication, always relevant but only to the owning connection, it is deliberately kept on a separate track from the global grid and always-relevant nodes.

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
| **Push Model replication** | Team-related properties trade the per-frame diff compare for explicit `MARK_PROPERTY_DIRTY_FROM_NAME` marking — in particular, points that edit an array element in place through a raw pointer (role changes, status toggles, invitation acceptance) can't be picked up automatically by the engine, so each write site marks it by hand |
| **In-game team management** | Each team is spawned as its own replicated `Actor` (`ABTKTeam`), owning invitations/acceptance, kicking, captaincy transfer (with automatic Captain→Lieutenant demotion), and team chat (send/delete) — role-based permissions (Captain/Lieutenant/Member) are validated on every request |
| **Resource cleanup** | Every callback proxy explicitly clears its delegates in `BeginDestroy()` to prevent dangling bindings |
| **Replication Graph routing** | Class → node mappings are cached in an `enum` lookup table and resolved by walking up the inheritance chain, so subclasses inherit their parent's routing automatically — unregistered classes are surfaced through a warning log |

---

*This document is compiled from snippets extracted from an actual production codebase, summarized for portfolio review purposes.*
