# OnlineSubsystemIcarus

**Unreal Engine Online Subsystem (OSS)** 기반으로 제작된 커스텀 게임 백엔드 연동 plugin입니다. **WebSocket(STOMP 유사 framing)** 과 **RabbitMQ/STOMP 로비 messaging**을 통해 UE의 `IOnlineSubsystem` 추상화 계층에 연결되며, Identity, Session/Matchmaking, User Cloud Storage, Lobby Queue 서비스를 자체 protocol 기반으로 제공합니다.

---

## Overview

`OnlineSubsystemIcarus`는 표준 Unreal OSS plugin 패턴(`FOnlineSubsystemIcarus : public FOnlineSubsystemIcarusGen`)을 따르지만, 실질적인 engineering 포인트는 다음 두 영역에 집중되어 있습니다.

1. **비동기 WebSocket RPC framework** (`IcarusWSFrame` / `IcarusConnectionComponentBase`) — Raw WebSocket transport 위에 요청/응답 semantics, timeout tracking, reconnect/backoff 로직을 layering.
2. **STOMP 기반 병렬 Lobby/Queue client** (`IcarusLobbyConnectionComponentBase`) — RabbitMQ를 통한 matchmaking 대기열 상태 업데이트를 담당하며, 메인 gateway connection과 완전히 분리되어 동작.

서브시스템은 표준 UE interface(`IOnlineIdentity`, `IOnlineSession`, `IOnlineUserCloud`, `IOnlineUser`)를 노출하지만, 내부적으로는 모든 백엔드 통신이 이 두 connection component를 통해 routing됩니다.

---

## Architecture

```
FOnlineSubsystemIcarus (FOnlineSubsystemIcarusGen)
│
├── FOnlineIdentityInterfaceIcarus     — 로그인/로그아웃, 계정/세션 토큰 lifecycle
├── FOnlineSessionIcarus               — matchmaking, 호스트 migration, connection string relay
├── FOnlineUserCloudIcarus             — zlib 압축 + SHA1 해시 기반 세이브 데이터 업/다운로드
├── FOnlineUserInterfaceIcarus         — 캐시된 온라인 유저 조회
├── FOnlineProfileIcarus               — 캐릭터 슬롯 유효성 검증 layer
├── FOnlineLobbyIcarus                 — 로비 RPC dispatch, JWT 토큰 갱신
│
├── UIcarusConnectionComponent (UIcarusConnectionComponentBase)
│   ├── WebSocket transport (ws/wss) — WebSocketsModule 기반
│   ├── FIcarusWSFrame — 커스텀 wire protocol (프레임 encoding/decoding)
│   ├── FIcarusConnectionPingManager — heartbeat/prospect keep-alive 전담 worker thread (FRunnable)
│   └── 프레임 기반 요청/응답 dispatch table (FrameHandler / FrameCommandPairs)
│
└── UIcarusLobbyConnectionComponent (UIcarusLobbyConnectionComponentBase)
    ├── STOMP client (RabbitMQ) — StompModule 기반
    ├── FLobbyWSFrame — IStompMessage를 공용 FIcarusWSFrame body 포맷으로 wrapping하는 adapter
    └── 대기열 위치 ETA 추정 (CalculateTimeLeft)
```

---

## Core Components

### `FIcarusWSFrame` — Wire Protocol
STOMP에서 착안한 자체 프레임 format(`COMMAND\nheader:value\n\nBODY`)을 수동 byte buffer parsing(`ReadValue`, `SkipNewlines`, 구분자 인식 escaping)으로 직접 구현했습니다.

- STOMP spec에 준하는 헤더 escape encoding(`\`, `:`, `\n`, `\r`)을 지원하며, 레거시 호환을 위해 `CONNECT` command는 예외 처리.
- Thread-safe(`FCriticalSection`으로 보호)하게 순차적으로 증가하는 `FrameIndex`를 사용해 비동기 요청과 응답을 correlate시킴.
- Heartbeat 프레임(`IcarusHeartbeatCommand`)은 단일 `\n`으로 encoding.
- 모든 요청 프레임에 JWT Bearer 토큰(`WS_HEADER_JWT_TOKEN`)을 자동 주입.
- 로컬/싱글플레이어 fallback 경로를 위한 오프라인 모드 프레임 생성 지원.

```cpp
// IcarusWSFrame.cpp — 헤더 escape 규칙을 따르는 byte-level 파서
static uint8 ReadValue(const uint8* In, SIZE_T Length, SIZE_T& Index, FIcarusWSBuffer& Buffer,
                        const char* Delimiters = "\n", bool bAllowEscaping = true)
{
    bool bEscapeNext = false;
    uint8 Retval = '\0';
    for (; Index < Length; Index++)
    {
        if (!bEscapeNext)
        {
            if (bAllowEscaping && In[Index] == '\\')
            {
                bEscapeNext = true;
                continue;
            }
            const uint8* Found = MatchDelimiter(In[Index], Delimiters);
            if (Found != nullptr)
            {
                Retval = *Found;
                Index++;
                break;
            }
        }
        Buffer.Add(In[Index]);
        bEscapeNext = false;
    }

    // STOMP는 \r\n도 줄바꿈으로 허용하므로 끝에 남은 \r은 잘라낸다
    if (Retval == '\n' && Buffer.Num() > 0 && Buffer[Buffer.Num() - 1] == '\r')
    {
        Buffer.RemoveAt(Buffer.Num() - 1);
    }

    Buffer.Add('\0');
    return Retval;
}
```

```cpp
// IcarusWSFrame.cpp — 생성자에서 모든 온라인 프레임에 JWT를 자동 주입
FIcarusWSFrame::FIcarusWSFrame(const FIcarusWSCommand& InCommand, const FIcarusWSHeader& InHeader,
                                const FIcarusWSBuffer& InBody, bool bInOfflineFrame)
    : FrameIdx(INDEX_NONE)
    , Command(InCommand)
    , Header(InHeader)
    , Body(InBody)
    , bOfflineFrame(bInOfflineFrame)
{
    if (bInOfflineFrame)
    {
        if (Body.Num() > 0)
        {
            Body.Add('\0');
        }
        FScopeLock ScopeLock(&FIcarusWSFrame::FrameIndexLock);
        FrameIdx = FrameIndex++;
    }
    else
    {
        Header.Add(WS_HEADER_JWT_TOKEN, Token);
    }
}
```

### `UIcarusConnectionComponentBase` — Transport & RPC Layer
`IWebSocket` lifecycle을 소유하며 그 위에 요청/응답 프로토콜을 구현합니다.

- **Command→Response pairing**(`FrameCommandPairs`)을 통해 응답 없는 요청에 대한 자동 timeout 처리를 구현. 미응답 요청은 만료 timestamp와 함께 `ReqFrameBuffers`에 저장되고 `FTicker`로 0.2초마다 polling됨.
- **Send-with-retry**: `WriteFrameImpl`은 전송 실패 시 1초 간격으로 최대 60초까지 재시도한 뒤, 실제 서버 응답과 동일한 handler 경로로 "Backend connection lost" 실패 응답을 synthesize함 — 호출부에서 네트워크 장애와 실제 RPC 에러를 구분할 필요가 없도록 설계.
- **지수 backoff 재연결**(`Reconnect()`) — `MaxReconnectTime`으로 상한이 걸려 있으며 전용 `FTicker` delegate로 구동.
- **JWT 토큰 무효화 처리** — `ResTokenExpired` / `ResTokenNotSupplied` / `ResTokenInvalid`는 모두 `InvalidConnectionToken()`으로 수렴되어 캐시된 토큰을 폐기하고 clean 재연결을 강제함.

```cpp
// IcarusConnectionComponentBase.cpp — 0.2초마다 만료된 요청을 찾아 동일 handler 경로로 실패 응답 dispatch
bool UIcarusConnectionComponentBase::CheckTimeoutRequests(float DeltaSeconds)
{
	int64 CurrentTime = (FDateTime::UtcNow().GetTicks() - FDateTime(1970, 1, 1).GetTicks()) / ETimespan::TicksPerSecond;

	TArray<FFrameTimeout>::ElementType* ReqFound = Algo::FindByPredicate(ReqFrameBuffers, [CurrentTime](const FFrameTimeout& FrameTimeout)
	{
		return FrameTimeout.ExpireTime <= CurrentTime;
	});

	// Found timeout request
	if (ReqFound != nullptr)
	{
		FIcarusWSFrame Frame = FIcarusWSFrame(ReqFound->Buffer.GetData(), ReqFound->Buffer.Num());
		// Return failure callback
		if (ensure(FrameCommandPairs.Contains(Frame.GetCommand())))
		{
			if (const FIcarusWSCommand ResCommand = FrameCommandPairs[Frame.GetCommand()];
				ensure(FrameHandler.Contains(ResCommand)))
			{
				FIcarusWSHeader ResHeader;
				ResHeader.Add(WS_HEADER_ERROR, TEXT("1"));
				ResHeader.Add(WS_HEADER_ERROR_MESSAGE, TEXT("Backend request timeout"));
				const FIcarusWSFrameRef& ResFrame = MakeShareable(new FIcarusWSFrame(ResCommand, ResHeader));
				ResFrame->SetOfflineModeCommand(ResCommand, Frame.GetFrameIndex(), ResHeader);
				if (FrameHandler[ResCommand].IsBound())
				{
					FrameHandler[ResCommand].Execute(ResFrame);
				}
			}
		}
		// ... remove from ReqFrameBuffers
	}

	return true;
}
```

```cpp
// IcarusConnectionComponentBase.cpp — 전송 실패 시 1초 간격 재시도, 60초 초과 시 실패 응답 synthesize
void UIcarusConnectionComponentBase::WriteFrameImpl(const FIcarusWSFrame& Frame, const FIcarusWSBuffer& FrameData, float ElapsedRetryTime)
{
	auto TrySendFrame = [this](const FIcarusWSBuffer& FrameData)
	{
		if (IsConnected())
		{
			WebSocket->Send(FrameData.GetData(), FrameData.Num(), false);
			return true;
		}
		return false;
	};

	// Attempt to send a frame immediately, if it fails, we want to start a retry timer which will attempt to
	// send the frame each tick until it works or ElapsedRetryTime is met, and the request is responded to with a failure
	if (!TrySendFrame(FrameData))
	{
		constexpr float RetryDelay = 1.0f;
		FTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this, Frame = FIcarusWSFrame(FrameData.GetData(), FrameData.Num()), FrameData, ElapsedRetryTime, TrySendFrame, RetryDelay](float DeltaTime) mutable
		{
			ElapsedRetryTime += RetryDelay;

			constexpr float RetryTimeoutTime = 60.0f;
			if (ElapsedRetryTime < RetryTimeoutTime)
			{
				return !TrySendFrame(FrameData);
			}
			else
			{
				// Execute failure after retrying a bunch
				if (ensure(FrameCommandPairs.Contains(Frame.GetCommand())))
				{
					if (const FIcarusWSCommand ResCommand = FrameCommandPairs[Frame.GetCommand()];
						ensure(FrameHandler.Contains(ResCommand)))
					{
						FIcarusWSHeader ResHeader;
						ResHeader.Add(WS_HEADER_ERROR, TEXT("1"));
						ResHeader.Add(WS_HEADER_ERROR_MESSAGE, TEXT("Backend connection lost"));
						const FIcarusWSFrameRef& ResFrame = MakeShareable(new FIcarusWSFrame(ResCommand, ResHeader));
						ResFrame->SetOfflineModeCommand(ResCommand, Frame.GetFrameIndex(), ResHeader);
						if (FrameHandler[ResCommand].IsBound())
						{
							FrameHandler[ResCommand].Execute(ResFrame);
						}
					}
				}
			}
			return false;
		}), RetryDelay);
	}
}
```

```cpp
// IcarusConnectionComponentBase.cpp — 지수 backoff 재연결 (2^attempt초, MaxReconnectTime으로 상한)
void UIcarusConnectionComponentBase::Reconnect()
{
	ReconnectTimer = 1.0f;

	if (bEnabledReconnect)
	{
		const auto ReconnectTick = [this](float DeltaSeconds)
		{
			if (!IsValid(this))
			{
				return false;
			}

			ReconnectTimer = FMath::Max(ReconnectTimer - DeltaSeconds, 0.0f);
			if (ReconnectTimer <= 0.f)
			{
				if (ReconnectTimerDelegateHandle.IsValid())
				{
					FTicker::GetCoreTicker().RemoveTicker(ReconnectTimerDelegateHandle);
					ReconnectTimerDelegateHandle.Reset();
				}

				Connect(AccountCredentials);
			}
			return true;
		};

		ReconnectTimerDelegateHandle = FTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, ReconnectTick), 0.1f);
		ReconnectTimer = static_cast<float>(FMath::Min<int32>(FMath::Pow(2, ReconnectAttempts), MaxReconnectTime));
		++ReconnectAttempts;
	}
}
```

```cpp
// IcarusConnectionComponentBase.cpp — 3가지 토큰 오류 응답이 모두 같은 무효화 경로로 수렴
void UIcarusConnectionComponentBase::InvalidConnectionToken()
{
	Close();

	// flush token
	UOnlineSubsystemIcarusFunctionLibrary::SaveIcarusConnectionToken(TEXT(""));

	OnResInvalidTokenDelegate.Broadcast();
}

void UIcarusConnectionComponentBase::OnResTokenNotSupplied(const FIcarusWSFrameRef& Frame) { InvalidConnectionToken(); }
void UIcarusConnectionComponentBase::OnResTokenExpired(const FIcarusWSFrameRef& Frame) { InvalidConnectionToken(); }
void UIcarusConnectionComponentBase::OnResTokenInvalid(const FIcarusWSFrameRef& Frame) { InvalidConnectionToken(); }
```

### `FIcarusConnectionPingManager` — Heartbeat Thread
게임 thread와 분리된 전용 `FRunnable` worker thread입니다.

- 부모 connection이 살아있는 동안 일정 주기로 `ReqPing`을 전송.
- 선택적으로 "prospect"(백엔드 상의 임시/예약 entity)를 `UpdateProspect()`로 heartbeat 처리하며, `FScopeLock`으로 보호되는 timer를 통해 thread 종료 없이 런타임 중 ON/OFF 전환 가능(`SetHeartbeatProspect` / `ClearHeartbeatProspect`).

```cpp
// IcarusConnectionPingManager.cpp — worker thread에서 ping/prospect heartbeat를 함께 처리
uint32 FIcarusConnectionPingManager::Run()
{
	while (bRunning && IcarusConnectionComponent && !IcarusConnectionComponent->IsPendingKill() && IcarusConnectionComponent->IsConnected())
	{
		if (PingTimer < LastPingElapsed)
		{
			LastPingElapsed = 0.0f;

			// Send Ping
			IcarusConnectionComponent->WriteFrame(ReqPingCommand);
		}

		if (!ProspectID.IsEmpty() && UpdateProspectPingTimer > 0.f)
		{
			FScopeLock Lock(&CriticalSectionUpdateProspect);
			if (UpdateProspectPingTimer < UpdateProspectLastPingElapsed)
			{
				UpdateProspectLastPingElapsed = 0.0f;

				// write prospect update frame
				UpdateProspect();
			}
			UpdateProspectLastPingElapsed += PingRequestFrequency;
		}

		LastPingElapsed += PingRequestFrequency;
		FPlatformProcess::Sleep(PingRequestFrequency);
	}
	return 0;
}
```

```cpp
// IcarusConnectionPingManager.cpp — 런타임 중 prospect heartbeat ON/OFF 전환
void FIcarusConnectionPingManager::SetHeartbeatProspect(const FString& NewProspectID)
{
	FScopeLock Lock(&CriticalSectionUpdateProspect);
	ProspectID = NewProspectID;
	UpdateProspectPingTimer = 10.f;
	UpdateProspectLastPingElapsed = 0.f;
}

void FIcarusConnectionPingManager::ClearHeartbeatProspect()
{
	FScopeLock Lock(&CriticalSectionUpdateProspect);
	ProspectID.Empty();
	UpdateProspectPingTimer = 0.f;
	UpdateProspectLastPingElapsed = 0.f;
}
```

### `UIcarusLobbyConnectionComponentBase` — Queue/Lobby Messaging
Matchmaking 대기열 telemetry를 위한, 메인 connection과 독립적인 두 번째 STOMP-over-RabbitMQ connection입니다.

- 플레이어별 relay queue(`/queue/{playerId}`)와 broadcast topic(`/topic/notice`)을 구독하며, 두 구독이 모두 완료된 상태(`bSubscribedRelayTo && bSubscribedTopic`)를 확인한 뒤 `OnLobbyConnect`를 trigger.
- `UIcarusLobbyConnectionComponent::CalculateTimeLeft`(파생 클래스, 본 업로드에는 미포함)는 **rolling average 기반 처리율 estimator**(최근 10개 sample)를 구현하여 큐 소진 속도의 노이즈를 완화하고, 현재 큐 깊이와 경과 시간으로부터 ETA를 산출.
- 메인 gateway connection에서 캐시된 JWT(`FIcarusWSFrame::Token`)를 재사용하여 별도 재로그인 없이 인증을 처리, 추가 로그인 round-trip을 회피.

```cpp
// IcarusLobbyConnectionComponentBase.cpp — 두 구독이 모두 끝나야 로비 연결 완료로 간주
void UIcarusLobbyConnectionComponentBase::Subscribe()
{
	if (StompClient.IsValid())
	{
		RelayToSubscriptionID = StompClient->Subscribe(RelayToDestination,
			FStompSubscriptionEvent::CreateLambda([this](const IStompMessage& Message)->void {
			HandleIncomingFrame(&Message);
		}),
			FStompRequestCompleted::CreateLambda([this](bool bSuccess, const FString& Error)->void {
			bSubscribedRelayTo = bSuccess;
			OnCompletedSubScribe(bSuccess, Error);
		})
			);

		TopicSubscriptionID = StompClient->Subscribe(TopicDestination,
			FStompSubscriptionEvent::CreateLambda([this](const IStompMessage& Message)->void {
			HandleIncomingFrame(&Message);
		}),
			FStompRequestCompleted::CreateLambda([this](bool bSuccess, const FString& Error)->void {
			bSubscribedTopic = bSuccess;
			OnCompletedSubScribe(bSuccess, Error);
		})
			);
	}
}

bool UIcarusLobbyConnectionComponentBase::InitializedSubscriptions()
{
	return bSubscribedRelayTo && bSubscribedTopic;
}
```

```cpp
// IcarusLobbyConnectionComponentBase.cpp — 메인 게이트웨이 JWT를 재사용해 별도 로그인 없이 인증
void UIcarusLobbyConnectionComponentBase::OnLoginComplete(int32 LocalUserNum, bool bWasSuccessful,
                                                           const FUniqueNetId& UserId, const FString& Error)
{
	// ...
	FString Token;
	// reuse token
	if (UOnlineSubsystemIcarusFunctionLibrary::GetIcarusConnectionToken(Token))
	{
		FIcarusWSFrame::Token = Token;
		OnLobbyHasValidToken.Broadcast();
	}
	else
	{
		Connect();
	}
}
```

### `IcarusMessageListeners` — Event Fan-out
저수준 connection component delegate(`OnConnect`, `OnMatchUpdate`, `OnChatMessage` 등)를 게임 layer용 multicast delegate로 다시 broadcast하는 UObject 기반의 얇은 pub/sub bridge로, 게임플레이 코드를 OSS interface 내부 구현으로부터 분리시킵니다.

### `FOnlineIdentityInterfaceIcarus` — Login Lifecycle
표준 `IOnlineIdentity`를 구현하며, 플랫폼 native 로그인(Steam/EOS)과 Icarus 백엔드 로그인을 단일화 합니다.

- `Login()`은 중복 로그인을 방지하기 위해 진행 중인 로그인이나 기존 세션이 있으면 먼저 `Logout()`으로 정리한 뒤, `UIcarusConnectionComponent::Connect()`로 WebSocket handshake를 트리거.
- 로그인 성공 여부는 실제로는 WebSocket 응답(`ResUserTicket`)을 기다려야 하므로, `Login()` 자체는 handshake 개시 성공 여부만 반환하고 최종 결과는 `OnResUserTicket` 콜백에서 `TriggerOnLoginCompleteDelegates`로 통지.
- **오프라인 모드 fallback**: 백엔드로부터 매칭되는 로컬 유저를 찾지 못했는데 오프라인 모드인 경우, dummy 계정을 생성해 `UserAccounts`/`UserIds`에 등록 — 싱글플레이어 상황에서도 identity 관련 API가 정상 동작하도록 보장.
- `IsConnected()`는 오프라인 모드에서는 항상 `true`를 반환하고, 온라인 모드에서는 WebSocket 연결 여부와 로그인 상태(`LoginStatus` user attribute)를 모두 확인.

```cpp
// OnlineIdentityInterfaceIcarus.cpp — 로그인 개시: 기존 세션 정리 후 WebSocket handshake 시작
bool FOnlineIdentityInterfaceIcarus::Login(int32 LocalUserNum, const FOnlineAccountCredentials& AccountCredentials)
{
	if (bHasLoginOutstanding)
	{
		// force to logout
		Logout(LocalUserNum);
	}

	if (UserIds.Num() > 0)
	{
		// force to logout
		Logout(LocalUserNum);
	}

	LocalUserNumPendingLogin = LocalUserNum;
	bHasLoginOutstanding = true;

	// ... 캐시 계정 등록 ...

	if (UIcarusConnectionComponent* ICC = IcarusSubsystem->GetIcarusConnectionComponent())
	{
		if (!ICC->Connect(AccountCredentials))
		{
			bHasLoginOutstanding = false;
			Logout(LocalUserNum);
			TriggerOnLoginCompleteDelegates(LocalUserNum, false, FUniqueNetIdString(TEXT("")), TEXT("Failed to connect to Icarus backend"));
			return false;
		}
	}

	return true;
}
```

```cpp
// OnlineIdentityInterfaceIcarus.cpp — 백엔드 응답에 매칭되는 로컬 유저가 없을 때 오프라인 dummy 계정 생성
else
{
	UE_LOG(IcarusOSSLog, Error, TEXT("OnResUserTicket : No player found"));

	// Add dummy player to OnlineSubSystem in offline play
	if (!UOnlineSubsystemIcarusOfflineFunctionLibrary::IsOnlineMode())
	{
		FOnlineAccountCredentials AccountCredentials = UOnlineSubsystemIcarusSessionFunctionLibrary::GetAccountCredentials();
		TSharedRef<FUserOnlineAccountIcarus> UserAccountPtr = MakeShareable(new FUserOnlineAccountIcarus(AccountCredentials.Id, AccountCredentials.Token, AccountCredentials.Type));

		UserAccountPtr->SetUserAttribute(TEXT("LoginStatus"), TEXT("Connecting"));
		UserAccounts.Add(AccountCredentials.Id, UserAccountPtr);
		UserIds.Add(LocalUserNumPendingLogin, UserAccountPtr->GetUserId());
	}
}
```

### `FOnlineProfileIcarus` — Character Slot Validation
자동 생성된 `FOnlineProfileIcarusGen` 기반 클래스 위에, 캐릭터 슬롯 관련 요청(잠금 해제, 진행도 갱신, 삭제, 리셋)에 공통적으로 `ChrSlot != INDEX_NONE` 유효성 검증을 추가하는 얇은 override layer입니다.

```cpp
// OnlineProfileIcarus.cpp — 캐릭터 슬롯 유효성 검증 후 base 구현에 위임
bool FOnlineProfileIcarus::ValidateUnlockCharacterFlags(const FReqUnlockCharacterFlags& Request)
{
	if (Request.ChrSlot == INDEX_NONE)
	{
		UE_LOG(LogOnlineProfile, Error, TEXT("UnlockCharacterFlags failed to Invalid character slot"));
		return false;
	}
	return FOnlineProfileIcarusGen::ValidateUnlockCharacterFlags(Request);
}
```

동일한 패턴이 `ValidateUpdateCharacterProgress`, `ValidateDeleteCharacter`, `ValidateResetCharacter`에도 반복됩니다 — 슬롯 검증만 상위 layer에서 가로채고 나머지 RPC 로직은 코드젠 기반 base 구현에 위임하는 구조.



## 주요 설계 세부 사항

- **모드 전환 시 interface hot-swap**: `SwitchOnlineMode()`는 `UIcarusConnectionComponent`(온라인)와 `UIcarusOfflineConnectionComponentGen`(오프라인) 사이를 전환하며, `Identity`, `Session`, `UserCloud`, `MessageListeners` 등 종속된 모든 interface의 callback을 다시 bind — 서브시스템 재초기화 없이 온라인/오프라인 fallback을 매끄럽게 지원. (진입점만 아래 예시로 포함; 실제 hot-swap 구현부인 `FOnlineSubsystemIcarus.cpp`는 이번 업로드에는 없습니다.)

  ```cpp
  // OnlineSubsystemIcarusOfflineFunctionLibrary.cpp — Blueprint에서 호출하는 진입점
  bool UOnlineSubsystemIcarusOfflineFunctionLibrary::SwitchOnlineMode(bool bOnlineMode)
  {
  	FOnlineSubsystemIcarus* IcarusSubsystem = (FOnlineSubsystemIcarus*)IOnlineSubsystem::Get(ICARUS_SUBSYSTEM);
  	if (!IcarusSubsystem)
  	{
  		return false;
  	}

  	IcarusSubsystem->SwitchOnlineMode(bOnlineMode);

  	return true;
  }
  ```

- **통합된 실패 처리 경로**: timeout 만료(`CheckTimeoutRequests`)와 복구 불가능한 전송 실패(`WriteFrameImpl`) 모두 `FIcarusWSFrame::SetOfflineModeCommand`를 통해 응답 프레임을 synthesize한 뒤, 실제 서버 응답이 사용하는 것과 *동일한* `FrameHandler` table로 dispatch — RPC 호출부는 원인과 무관하게 단일한 실패 처리 코드 경로만 다루면 됨 (구현은 위 `UIcarusConnectionComponentBase` 섹션 참고).

- **WebSocket 업그레이드 헤더 기반 인증**: `Connect()`는 표준 WebSocket handshake의 upgrade header에 플랫폼 타입, 유저 ID, AppId, 플랫폼 native auth token, 그리고 클라이언트/데이터 버전을 실어 보내 별도의 로그인 RPC 없이 handshake 단계에서 인증을 수행.

  ```cpp
  // IcarusConnectionComponentBase.cpp — WebSocket handshake header에 인증/버전 정보 주입
  TMap<FString, FString> UpgradeHeaders;
  UpgradeHeaders.Add(TEXT("Type"), DefaultPlatformService);
  UpgradeHeaders.Add(TEXT("UserId"), AccountCredentials.Id);
  UpgradeHeaders.Add(TEXT("AppId"), AppId);
  UpgradeHeaders.Add(TEXT("AuthToken"), AccountCredentials.Token);
  UpgradeHeaders.Add(WS_GAME_DATA_FILE_VERSION, FString::Printf(TEXT("%d"), Version.Data.Changelist));
  UpgradeHeaders.Add(WS_GAME_CLIENT_VERSION, FString::Printf(TEXT("%d.%d.%d.%d"),
      Version.Version.Major, Version.Version.Minor, Version.Version.Patch, Version.Version.Changelist));

  WebSocket = FWebSocketsModule::Get().CreateWebSocket(AddressAndPort, Protocols, UpgradeHeaders);
  SetupCallbacks();
  WebSocket->Connect();
  ```

- **파일 업로드 pipeline** (`FOnlineUserCloudIcarus::WriteUserFile`): 무결성 검증을 위한 SHA-1 hashing, zlib 압축(`FCompression::CompressMemory`), 그리고 헤더에 압축/비압축 길이·hash·progress key 등의 metadata를 함께 실어 단일 요청으로 처리.

  ```cpp
  // OnlineUserCloudIcarus.cpp — 압축 + 해시 + metadata header를 한 번의 WriteFrame 요청으로 전송
  bool FOnlineUserCloudIcarus::WriteUserFile(const FUniqueNetId& UserId, const FString& FileName,
                                              TArray<uint8>& FileContents, bool bCompressBeforeUpload)
  {
  	// compress
  	TArray<uint8> OutCompressedData;
  	int32 CompressedSize = FCompression::CompressMemoryBound(NAME_Zlib, FileContents.Num());
  	OutCompressedData.SetNumUninitialized(CompressedSize);
  	if (FCompression::CompressMemory(NAME_Zlib, OutCompressedData.GetData(), CompressedSize, FileContents.GetData(), FileContents.Num()))
  	{
  		OutCompressedData.SetNum(CompressedSize, false);
  	}
  	else
  	{
  		UE_LOG(IcarusOSSLog, Error, TEXT("Failed CompressMemory length : %d"), FileContents.Num());
  		TriggerOnWriteUserFileCompleteDelegates(false, UserId, FileName);
  		return false;
  	}

  	if (UIcarusConnectionComponent* ICC = IcarusSubsystem->GetIcarusConnectionComponent())
  	{
  		FString SH1Hash;
  		SH1Hash = UOnlineSubsystemIcarusFunctionLibrary::GetHash(FileContents);

  		FString Length = FString::Printf(TEXT("%d"), OutCompressedData.Num());
  		FIcarusWSHeader Header;
  		Header.Add(WS_HEADER_HASH, SH1Hash);
  		Header.Add(WS_HEADER_TOTAL_LENGTH, Length);
  		Header.Add(WS_HEADER_DATA_LENGTH, Length);
  		Header.Add(WS_HEADER_PROGRESS_KEY, FileName);

  		FString UncompressLength = FString::Printf(TEXT("%d"), FileContents.Num());
  		Header.Add(WS_HEADER_UNCOMPRESSED_LENGTH, UncompressLength);

  		if (ICC->WriteFrame(ReqWriteStorageCommand, OutCompressedData, Header))
  		{
  			PendingStorageData.Key = FileName;
  			PendingStorageData.Hash = SH1Hash;
  			PendingStorageData.HashType = TEXT("sha1");
  			PendingStorageData.UncompressedLength = OutCompressedData.Num();
  			return true;
  		}
  	}

  	TriggerOnWriteUserFileCompleteDelegates(false, UserId, FileName);
  	return false;
  }
  ```

  압축률 로깅도 함께 남깁니다:

  ```cpp
  UE_LOG_ONLINE_CLOUD(Warning, TEXT("WriteUserFile %s %d/%d - %.2f%%"),
      *FileName, CompressedSize, FileContents.Num(),
      (double)(FileContents.Num() - CompressedSize) / (double)FileContents.Num() * 100);
  ```

- **Thread 안전성 경계**: ping manager는 자체 thread에서 동작하며 공유 상태(`ProspectID`, `UpdateProspectPingTimer`)를 `FScopeLock`으로 보호. WebSocket I/O 및 UObject delegate broadcast는 `FTicker`를 통해 게임 thread에 유지.

---

## Tech Stack

- **Engine**: Unreal Engine (Online Subsystem plugin architecture)
- **Transport**: WebSockets (`WebSocketsModule`), STOMP over RabbitMQ (`StompModule`)
- **Serialization**: `FJsonObjectConverter` (UStruct ⇄ JSON)
- **Concurrency**: `FRunnable`/`FRunnableThread`, `FTicker`, `FCriticalSection`/`FScopeLock`
- **Auth**: JWT Bearer 토큰, 플랫폼 native 인증(Steam/EOS)을 upgrade header로 전달
