# OnlineSubsystemIcarus

A custom game backend integration plugin built on the **Unreal Engine Online Subsystem (OSS)**. It connects to UE's `IOnlineSubsystem` abstraction layer through **WebSocket (STOMP-like framing)** and **RabbitMQ/STOMP lobby messaging**, providing Identity, Session/Matchmaking, User Cloud Storage, and Lobby Queue services on top of its own custom protocol.

---

## Overview

`OnlineSubsystemIcarus` follows the standard Unreal OSS plugin pattern (`FOnlineSubsystemIcarus : public FOnlineSubsystemIcarusGen`), but the real engineering focus is concentrated in two areas:

1. **Asynchronous WebSocket RPC framework** (`IcarusWSFrame` / `IcarusConnectionComponentBase`) — layers request/response semantics, timeout tracking, and reconnect/backoff logic on top of a raw WebSocket transport.
2. **STOMP-based parallel Lobby/Queue client** (`IcarusLobbyConnectionComponentBase`) — handles matchmaking queue status updates over RabbitMQ, operating completely independently from the main gateway connection.

The subsystem exposes the standard UE interfaces (`IOnlineIdentity`, `IOnlineSession`, `IOnlineUserCloud`, `IOnlineUser`), but internally all backend communication is routed through these two connection components.

---

## Architecture

```
FOnlineSubsystemIcarus (FOnlineSubsystemIcarusGen)
│
├── FOnlineIdentityInterfaceIcarus     — Login/logout, account/session token lifecycle
├── FOnlineSessionIcarus               — Matchmaking, host migration, connection string relay
├── FOnlineUserCloudIcarus             — Save data upload/download with zlib compression + SHA1 hashing
├── FOnlineUserInterfaceIcarus         — Cached online user lookup
├── FOnlineProfileIcarus               — Character slot validation layer
├── FOnlineLobbyIcarus                 — Lobby RPC dispatch, JWT token refresh
│
├── UIcarusConnectionComponent (UIcarusConnectionComponentBase)
│   ├── WebSocket transport (ws/wss) — built on WebSocketsModule
│   ├── FIcarusWSFrame — custom wire protocol (frame encoding/decoding)
│   ├── FIcarusConnectionPingManager — dedicated heartbeat/prospect keep-alive worker thread (FRunnable)
│   └── Frame-based request/response dispatch table (FrameHandler / FrameCommandPairs)
│
└── UIcarusLobbyConnectionComponent (UIcarusLobbyConnectionComponentBase)
    ├── STOMP client (RabbitMQ) — built on StompModule
    ├── FLobbyWSFrame — adapter that wraps IStompMessage into the common FIcarusWSFrame body format
    └── Queue position ETA estimation (CalculateTimeLeft)
```

---

## Core Components

### `FIcarusWSFrame` — Wire Protocol
A custom frame format inspired by STOMP (`COMMAND\nheader:value\n\nBODY`), implemented directly with manual byte buffer parsing (`ReadValue`, `SkipNewlines`, delimiter-aware escaping).

- Supports STOMP-spec-compliant header escape encoding (`\`, `:`, `\n`, `\r`), with an exception for the `CONNECT` command for legacy compatibility.
- Uses a thread-safe (protected by `FCriticalSection`), monotonically increasing `FrameIndex` to correlate asynchronous requests and responses.
- Heartbeat frames (`IcarusHeartbeatCommand`) are encoded as a single `\n`.
- Automatically injects a JWT Bearer token (`WS_HEADER_JWT_TOKEN`) into every request frame.
- Supports offline-mode frame generation for the local/single-player fallback path.

```cpp
// IcarusWSFrame.cpp — Byte-level parser that follows the header escape rules
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

    // STOMP also allows \r\n as a line break, so trim any trailing \r
    if (Retval == '\n' && Buffer.Num() > 0 && Buffer[Buffer.Num() - 1] == '\r')
    {
        Buffer.RemoveAt(Buffer.Num() - 1);
    }

    Buffer.Add('\0');
    return Retval;
}
```

```cpp
// IcarusWSFrame.cpp — Constructor automatically injects a JWT into every online frame
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

`Encode`/`Decode` are the counterpart pair that serialize a frame to the wire format and parse it back. `Encode` writes `COMMAND\nframe-idx:N\nheader:value\n...\n\nBODY`, with a special case for heartbeat frames (a single `\n`, no headers/body), and — for the `CONNECT` command only — skips metacharacter escaping for backwards compatibility. `Decode` mirrors this: it skips leading newlines to detect a heartbeat, reads the command, then reads headers until it hits an empty line, and treats the remaining bytes as the body.

```cpp
// IcarusWSFrame.cpp — Serializes a frame to the wire format (heartbeat vs. COMMAND\nheaders\n\nBODY)
void FIcarusWSFrame::Encode(FIcarusWSBuffer& Out) const
{
	// A heartbeat is just a newline and can't contain any data nor is it terminated with a \n byte
	if (Command == IcarusHeartbeatCommand)
	{
		Out.Add('\n');

		if(Header.Num() > 0)
		{
			UE_LOG(IcarusOSSLog, Warning, TEXT("Ignoring header fields for heartbeat frame."));
		}
		if(Body.Num() > 0)
		{
			UE_LOG(IcarusOSSLog, Warning, TEXT("Ignoring body for heartbeat frame."));
		}
	}
	// Else output COMMAND\nHeaders\n\nBody. Removed \0 for now. 
	else
	{
		// According to the spec, the CONNECT command should not escape metacharacters for backwards compatibility.
		bool bShouldEscapeFrameHeader = Command != IcarusConnectCommand;

		FString CommandString = Command.ToString();
		//CommandString.ToUpperInline();
		FTCHARToUTF8 CommandEncoded(*CommandString);
		AppendArray(Out, (uint8*)CommandEncoded.Get(), CommandEncoded.Length(), bShouldEscapeFrameHeader);
		Out.Add('\n');

		// encode frame index
		FTCHARToUTF8 HeaderFrameIdxEncoded(*WS_HEADER_FRAME_INDEX.ToString());
		AppendArray(Out, (uint8*)HeaderFrameIdxEncoded.Get(), HeaderFrameIdxEncoded.Length(), bShouldEscapeFrameHeader);
		Out.Add(':');
		if (!bOfflineFrame)
		{
			FScopeLock ScopeLock(&FIcarusWSFrame::FrameIndexLock);
			FrameIdx = FrameIndex++;
		}
		FTCHARToUTF8 HeaderFrameIdxValueEncoded(*FString::Printf(TEXT("%ld"), FrameIdx));
		AppendArray(Out, (uint8*)HeaderFrameIdxValueEncoded.Get(), HeaderFrameIdxValueEncoded.Length(), bShouldEscapeFrameHeader);
		Out.Add('\n');

		// encode header
		for (const TPair<FName, FString>& Element : Header)
		{
			FString ElementKeyString = Element.Key.ToString();
			ElementKeyString.ToLowerInline();
			FTCHARToUTF8 HeaderNameEncoded(*ElementKeyString);
			FTCHARToUTF8 HeaderValueEncoded(*Element.Value);
			AppendArray(Out, (uint8*)HeaderNameEncoded.Get(), HeaderNameEncoded.Length(), bShouldEscapeFrameHeader);
			Out.Add(':');
			AppendArray(Out, (uint8*)HeaderValueEncoded.Get(), HeaderValueEncoded.Length(), bShouldEscapeFrameHeader);
			Out.Add('\n');
		}

		Out.Add('\n');
		Out.Append(Body);
		//Out.Add('\0');
	}
}
```

```cpp
// IcarusWSFrame.cpp — Parses a raw byte buffer back into a frame (command, headers, frame index, body)
void FIcarusWSFrame::Decode(const uint8* In, SIZE_T Length)
{
	// Ignore terminating 0 if present
	if (Length > 0 && In[Length-1] == 0)
	{
		Length--;
	}

	FIcarusWSBuffer Buffer;
	SIZE_T Index = 0;
	// Trim off any initial newlines
	SkipNewlines(In, Length, Index);

	// Empty buffer after trimming newlines means this is a heartbeat packet
	if (Index >= Length)
	{
		Command = IcarusHeartbeatCommand;
		return;
	}

	// Read command
	ReadValue(In, Length, Index, Buffer);
	Command = UTF8_TO_TCHAR(Buffer.GetData());

	if (Index >= Length)
	{
		UE_LOG(IcarusOSSLog, Warning, TEXT("Stomp command '%s' received without any headers"), *Command.ToString().ToUpper());
		return;
	}

	// decode header
	while(Index < Length)
	{
		const uint8* Junk = In+Index;
		Buffer.Empty();
		uint8 Delimiter = ReadValue(In, Length, Index, Buffer, "\n:");
		FName HeaderName = UTF8_TO_TCHAR(Buffer.GetData());

		if (Delimiter == ':')
		{
			Buffer.Empty();
			ReadValue(In, Length, Index, Buffer);
			Header.Add(HeaderName, UTF8_TO_TCHAR(Buffer.GetData()));
		}
		else if (HeaderName == FName())
		{
			// Empty line marks the end of headers
			break;
		}
		else
		{
			UE_LOG(IcarusOSSLog, Warning, TEXT("Encountered header line with no colons, '%s'."), *HeaderName.ToString())
			Header.Add(HeaderName, TEXT(""));
		}
	}

	// decode frame index
	if (Header.Contains(WS_HEADER_FRAME_INDEX))
	{
		FrameIdx = FCString::Atoi64(*Header[WS_HEADER_FRAME_INDEX]);
	}

	Body.Append(In + Index, Length - Index);
	// Add string terminator at the end of the buffer
	Body.Add('\0');
}
```

### `UIcarusConnectionComponentBase` — Transport & RPC Layer
Owns the `IWebSocket` lifecycle and implements the request/response protocol on top of it.

- Implements automatic timeout handling for requests that receive no response, via **command→response pairing** (`FrameCommandPairs`). Unanswered requests are stored in `ReqFrameBuffers` with an expiration timestamp and polled every 0.2 seconds via `FTicker`.
- **Send-with-retry**: `WriteFrameImpl` retries every 1 second for up to 60 seconds on send failure, then synthesizes a "Backend connection lost" failure response through the same handler path used for real server responses — so callers never need to distinguish between a network failure and an actual RPC error.
- **Exponential backoff reconnect** (`Reconnect()`) — capped by `MaxReconnectTime` and driven by a dedicated `FTicker` delegate.
- **JWT token invalidation handling** — `ResTokenExpired`, `ResTokenNotSupplied`, and `ResTokenInvalid` all converge on `InvalidConnectionToken()`, which discards the cached token and forces a clean reconnect.

```cpp
// IcarusConnectionComponentBase.cpp — Every 0.2s, finds expired requests and dispatches a failure response through the same handler path
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
// IcarusConnectionComponentBase.cpp — Retries every 1s on send failure, synthesizes a failure response after 60s
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
// IcarusConnectionComponentBase.cpp — Exponential backoff reconnect (2^attempt seconds, capped by MaxReconnectTime)
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
// IcarusConnectionComponentBase.cpp — All three token error responses converge on the same invalidation path
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
A dedicated `FRunnable` worker thread separate from the game thread.

- Sends `ReqPing` at a fixed interval as long as the parent connection is alive.
- Optionally heartbeats a "prospect" (a temporary/reserved entity on the backend) via `UpdateProspect()`, with the timer protected by `FScopeLock` so it can be toggled ON/OFF at runtime (`SetHeartbeatProspect` / `ClearHeartbeatProspect`) without terminating the thread.

```cpp
// IcarusConnectionPingManager.cpp — Handles both ping and prospect heartbeats on the worker thread
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
// IcarusConnectionPingManager.cpp — Toggles prospect heartbeat ON/OFF at runtime
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
A second STOMP-over-RabbitMQ connection for matchmaking queue telemetry, independent from the main connection.

- Subscribes to a per-player relay queue (`/queue/{playerId}`) and a broadcast topic (`/topic/notice`), and triggers `OnLobbyConnect` once both subscriptions have completed (`bSubscribedRelayTo && bSubscribedTopic`).
- `UIcarusLobbyConnectionComponent::CalculateTimeLeft` (a derived class, not included in this upload) implements a **rolling-average-based throughput estimator** (last 10 samples) to smooth out noise in queue drain rate, and computes an ETA from the current queue depth and elapsed time.
- Reuses the cached JWT (`FIcarusWSFrame::Token`) from the main gateway connection to authenticate without a separate re-login, avoiding an extra login round-trip.

```cpp
// IcarusLobbyConnectionComponentBase.cpp — Lobby connection is only considered complete once both subscriptions finish
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
// IcarusLobbyConnectionComponentBase.cpp — Reuses the main gateway JWT to authenticate without a separate login
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
A thin UObject-based pub/sub bridge that re-broadcasts low-level connection component delegates (`OnConnect`, `OnMatchUpdate`, `OnChatMessage`, etc.) as multicast delegates for the game layer, decoupling gameplay code from the OSS interface's internal implementation.

### `FOnlineIdentityInterfaceIcarus` — Login Lifecycle
Implements the standard `IOnlineIdentity` interface, unifying platform-native login (Steam/EOS) with Icarus backend login.

- `Login()` first cleans up any in-progress login or existing session via `Logout()` to prevent duplicate logins, then triggers the WebSocket handshake through `UIcarusConnectionComponent::Connect()`.
- Because login success actually depends on waiting for a WebSocket response (`ResUserTicket`), `Login()` itself only returns whether the handshake was successfully initiated, and the final result is reported later via `TriggerOnLoginCompleteDelegates` in the `OnResUserTicket` callback.
- **Offline mode fallback**: if no matching local user is found from the backend response and the game is in offline mode, a dummy account is created and registered in `UserAccounts`/`UserIds` — ensuring identity-related APIs still work correctly in single-player situations.
- `IsConnected()` always returns `true` in offline mode; in online mode it checks both the WebSocket connection state and the login status (the `LoginStatus` user attribute).

```cpp
// OnlineIdentityInterfaceIcarus.cpp — Login initiation: clean up any existing session, then start the WebSocket handshake
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

	// ... register cached account ...

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
// OnlineIdentityInterfaceIcarus.cpp — Creates an offline dummy account when no local user matches the backend response
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
A thin override layer on top of the auto-generated `FOnlineProfileIcarusGen` base class, adding a common `ChrSlot != INDEX_NONE` validity check to character-slot-related requests (unlock, progress update, delete, reset).

```cpp
// OnlineProfileIcarus.cpp — Validates the character slot, then delegates to the base implementation
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

The same pattern is repeated in `ValidateUpdateCharacterProgress`, `ValidateDeleteCharacter`, and `ValidateResetCharacter` — only the slot validation is intercepted at this layer, while the rest of the RPC logic is delegated to the codegen-based base implementation.



## Key Design Details

- **Interface hot-swap on mode switch**: `SwitchOnlineMode()` switches between `UIcarusConnectionComponent` (online) and `UIcarusOfflineConnectionComponentGen` (offline), re-binding the callbacks of all dependent interfaces — `Identity`, `Session`, `UserCloud`, `MessageListeners`, etc. — enabling smooth online/offline fallback without reinitializing the subsystem. (Only the entry point is shown below as an example; the actual hot-swap implementation in `FOnlineSubsystemIcarus.cpp` is not included in this upload.)

  ```cpp
  // OnlineSubsystemIcarusOfflineFunctionLibrary.cpp — Entry point called from Blueprint
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

- **Unified failure handling path**: both expired timeouts (`CheckTimeoutRequests`) and unrecoverable send failures (`WriteFrameImpl`) synthesize a response frame via `FIcarusWSFrame::SetOfflineModeCommand`, then dispatch it through the *same* `FrameHandler` table used for real server responses — so RPC callers only ever need to handle a single failure code path, regardless of the underlying cause (see the `UIcarusConnectionComponentBase` section above for the implementation).

- **Authentication via WebSocket upgrade headers**: `Connect()` sends the platform type, user ID, AppId, platform-native auth token, and client/data version in the upgrade headers of the standard WebSocket handshake, performing authentication at the handshake stage without a separate login RPC.

  ```cpp
  // IcarusConnectionComponentBase.cpp — Injects auth/version info into the WebSocket handshake headers
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

- **File upload pipeline** (`FOnlineUserCloudIcarus::WriteUserFile`): handles SHA-1 hashing for integrity verification, zlib compression (`FCompression::CompressMemory`), and metadata such as compressed/uncompressed length, hash, and progress key — all sent together in the header as a single request.

  ```cpp
  // OnlineUserCloudIcarus.cpp — Sends compression + hash + metadata header in a single WriteFrame request
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

  Compression ratio is also logged:

  ```cpp
  UE_LOG_ONLINE_CLOUD(Warning, TEXT("WriteUserFile %s %d/%d - %.2f%%"),
      *FileName, CompressedSize, FileContents.Num(),
      (double)(FileContents.Num() - CompressedSize) / (double)FileContents.Num() * 100);
  ```

- **Thread safety boundary**: the ping manager runs on its own thread and protects shared state (`ProspectID`, `UpdateProspectPingTimer`) with `FScopeLock`. WebSocket I/O and UObject delegate broadcasts are kept on the game thread via `FTicker`.

---

## Tech Stack

- **Engine**: Unreal Engine (Online Subsystem plugin architecture)
- **Transport**: WebSockets (`WebSocketsModule`), STOMP over RabbitMQ (`StompModule`)
- **Serialization**: `FJsonObjectConverter` (UStruct ⇄ JSON)
- **Concurrency**: `FRunnable`/`FRunnableThread`, `FTicker`, `FCriticalSection`/`FScopeLock`
- **Auth**: JWT Bearer tokens, platform-native authentication (Steam/EOS) passed via upgrade headers
