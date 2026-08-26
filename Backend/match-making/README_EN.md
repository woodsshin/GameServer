# GameLift Matchmaking Service

A C#/.NET gRPC-based matchmaking backend. It receives requests from the game client and the dedicated server, searches for and places game sessions via AWS GameLift, and connects players to sessions with open slots.

## Overview

- **Role**: When a client requests matchmaking, the service first searches for an in-progress game session that already has an open slot, and if none exists, places a new session via GameLift Matchmaking (FlexMatch).
- **Communication**: gRPC (Protocol Buffers)
- **Authentication**: EOS (Epic Online Services) JWT for the client, S2S (Server-to-Server) JWT for the dedicated server — the two JWT bearer schemes are separated at the policy level
- **Infrastructure integration**: Session search, matchmaking ticket issuance/polling, and player session creation via the AWS GameLift SDK (`AmazonGameLiftClient`)

## Architecture

```
┌──────────────┐        EOS JWT         ┌────────────────────────┐        AWS SDK        ┌─────────────┐
│ Game Client  │ ─────────────────────▶ │                        │ ─────────────────────▶│             │
└──────────────┘   MatchMakingSvc       │   MatchMaking Service   │   StartMatchmaking     │ AWS GameLift │
                    (gRPC)              │   (ASP.NET Core /       │   SearchGameSessions    │  (FlexMatch, │
┌──────────────┐        S2S JWT         │    Kestrel + gRPC)      │   CreatePlayerSession   │   Fleets)    │
│ Dedicated    │ ─────────────────────▶ │                        │                        │             │
│ Server       │   MatchMakingSvcDS     └────────────────────────┘                        └─────────────┘
└──────────────┘
```

- `MatchMakingSvc`: Called by the game client, using the `EpicUser` policy (EOS authentication).
- `MatchMakingSvcDS`: Called by the dedicated server, using the `DedicatedServer` policy (S2S authentication + role).
- Both services are implemented in a single `MatchMakingService` class, with the scheme distinguished via the `[Authorize]` attribute and ASP.NET Core authorization policies.

## Key Features

### 1. Starting Matchmaking (`StartMatchMaking`)
1. The client requests matchmaking along with per-region latency (`LatencyInMs`).
2. The GameLift client is initialized based on the region with the lowest latency.
3. **Prioritize searching for open slots**: `SearchGameSessionAsync` searches, using a `hasAvailablePlayerSessions=true` filter, for an already-open session that has a free slot.
4. If an open session is found, the player is joined to it immediately via `CreatePlayerSession` (reducing the cost of new placements).
5. If none is found, GameLift's `StartMatchmakingAsync` (FlexMatch) is called to issue a new matchmaking ticket, which is polled until completion (`WaitMatchmakingAsync`).

```csharp
// Select the nearest region and initialize the client
InitializeGameLiftClient(bestRegion);

// Priority 1: search for an existing session with an open slot
var matchMakingReponse = await SearchGameSessionAsync(request, _configureName);
if (matchMakingReponse.Ticket != null)
{
    _logger.LogInformation("Found GameSession {@PlayerId} : {@ConnectionInfo}",
        new PlayerId(request.PlayerId), matchMakingReponse.Ticket.GameSessionConnectionInfo);
    return matchMakingReponse;
}

// Priority 2: if none found, start new matchmaking via FlexMatch and poll until completion
var response = await _gameliftclient.StartMatchmakingAsync(matchmakingRequest);
matchMakingReponse = await WaitMatchmakingAsync(response.MatchmakingTicket);
return matchMakingReponse;
```

**Searching for open slots (`SearchGameSessionAsync`)** — after searching with the `hasAvailablePlayerSessions=true` filter, the player is joined directly to a session that actually has a free slot (`MaximumPlayerSessionCount - CurrentPlayerSessionCount > 0`):

```csharp
var request = new SearchGameSessionsRequest
{
    AliasId = _aliasId,
    FilterExpression = "hasAvailablePlayerSessions=true",
    SortExpression = "playerSessionCount DESC",
    Limit = 10,
    Location = regionName
};

var response = await _gameliftclient.SearchGameSessionsAsync(request);

foreach (var gamesession in response.GameSessions)
{
    if (gamesession.MaximumPlayerSessionCount - gamesession.CurrentPlayerSessionCount > 0)
    {
        var matchedPlayerSession = await CreatePlayerSessionAsync(gamesession.GameSessionId, matchmakingRequest.PlayerId);
        // ... fill in connection info (GameSessionConnectionInfo), then return with COMPLETE status ...
    }
}
```

### 2. Creating a Player Session (`CreatePlayerSession`)
- Confirms the target game session is in `ACTIVE` status, then issues a player session.

### 3. Creating a Game Session (`CreateGameSession`)
- First searches for an existing session by `GameDataId` (for reuse); if none exists, creates a new session and waits until it reaches `ACTIVE` status (`CreateAndWaitGameSessionAsync`).
- Prevents duplicate requests using an `IdempotencyToken`.

```csharp
// First look for an existing session by GameDataId and reuse it
if (request.GameDataId != null)
{
    var gamession = await SearchGameSessionByGameDataIdAsync(request.GameDataId);
    if (gamession.GameSessionId != null)
    {
        var playersession = await CreatePlayerSessionAsync(gamession.GameSessionId, request.CreatorId);
        return new GrpcMatchMaking.CreateGameSessionResponse()
        {
            GameSessionId = gamession.GameSessionId,
            CreatorId = request.CreatorId,
            PlayerSessionId = playersession.PlayerSessionId,
            IpAddress = gamession.IpAddress,
            Port = gamession.Port,
        };
    }
}

// If none exists, create a new one, preventing duplicates via IdempotencyToken
if (request.IdempotencyToken != null && request.IdempotencyToken.Length > 0)
{
    createRequest.IdempotencyToken = request.IdempotencyToken;
}
var response = await CreateAndWaitGameSessionAsync(createRequest);
```

### 4. Dedicated-Server-Only APIs (`StartMatchMakingDS`, `CreateGameSessionDS`)
- Separate endpoints (using S2S authentication) that let the server directly specify a region to trigger matchmaking/session creation.

## Authentication Structure

```csharp
// Register two JWT bearer schemes
builder.Services.AddAuthentication(JwtBearerDefaults.AuthenticationScheme)
    .AddJwtBearer("EOS", options => { ... })   // Game client
    .AddJwtBearer("S2S", options => { ... });  // Dedicated server

// Separate via policies
options.AddPolicy(AuthPolicy.EpicUser, policy => {
    policy.AuthenticationSchemes.Add("EOS");
    policy.RequireAuthenticatedUser();
    policy.RequireClaim(ClaimTypes.NameIdentifier);
});
options.AddPolicy(AuthPolicy.DedicatedServer, policy => {
    policy.AuthenticationSchemes.Add("S2S");
    policy.RequireAuthenticatedUser();
    policy.RequireRole("DedicatedServer");
});
```

Each gRPC service class specifies which scheme it requires via `[Authorize(Policy = ...)]`, separating the client-facing and server-facing endpoints so that each has its own distinct trust boundary.

```csharp
[Authorize(Policy = AuthPolicy.EpicUser)]
public class MatchMakingService : MatchMakingSvc.MatchMakingSvcBase
{
    // Called by the game client: StartMatchMaking, CreatePlayerSession, CreateGameSession
}

[Authorize(Policy = AuthPolicy.DedicatedServer)]
public class MatchMakingServiceDS : MatchMakingSvcDS.MatchMakingSvcDSBase
{
    // Called by the dedicated server: StartMatchMakingDS, CreateGameSessionDS
}
```

## GameLift Integration Details

- **Region → Alias ID mapping**: `GameLiftSettings` maps region codes (`eu-central-1`, `ap-southeast-2`, etc.) to GameLift alias IDs, selecting the region with the lowest latency and routing to the corresponding alias.
- **Authentication method**: Switchable between `IAMRole` or an access key/secret key via configuration (`GameLiftSettings.AWSAuthenticationType`).

```csharp
private void InitializeGameLiftClient(RegionEndpoint region)
{
    _region = region;
    _aliasId = _gameLiftSettings.GetRegionAliasId(_region.SystemName);

    if (_gameLiftSettings.AWSAuthenticationType == "IAMRole")
    {
        _gameliftclient = new AmazonGameLiftClient(_region);
    }
    else
    {
        var credentials = new BasicAWSCredentials(_accessKey, _secretKey);
        _gameliftclient = new AmazonGameLiftClient(credentials, _region);
    }
}
```

- **Waiting for session placement**: After creating a session, `DescribeGameSessionsAsync` is polled every 5 seconds while waiting for `ACTIVE` status; `TIMED_OUT`/`FAILED`/`CANCELLED`/`TERMINATED` are treated as failures.

```csharp
var hasPlaced = false;
var failedPlace = false;
while (!hasPlaced && !failedPlace)
{
    await Task.Delay(5000);
    var describeResponse = await _gameliftclient.DescribeGameSessionsAsync(describeRequest);

    foreach (var gamesession in describeResponse.GameSessions)
    {
        switch (gamesession.Status)
        {
            case "ACTIVE": hasPlaced = true; break;
            case "ACTIVATING": break;
            case "TIMED_OUT":
            case "FAILED":
            case "CANCELLED":
            case "TERMINATED": failedPlace = true; break;
        }
    }
}
```

- **Polling the matchmaking ticket**: `DescribeMatchmakingAsync` is used to check status (`COMPLETED`/`SEARCHING`/`FAILED`, etc.) while waiting for completion or failure (currently polled every 5 seconds; a TODO remains in the code to switch to SNS due to concerns about API call volume).

```csharp
// TODO : Pulling the same calls exceed your API limit, which results in errors.
// Need to configure Amazon Simple Notification Service (SNS)
var foundmatch = false;
var failedmatch = false;
while (!foundmatch && !failedmatch)
{
    await Task.Delay(5000);
    var describeMatchmakingResponse = await _gameliftclient.DescribeMatchmakingAsync(describeMatchmakingRequest);

    foreach (var foundTiket in describeMatchmakingResponse.TicketList)
    {
        switch (foundTiket?.Status)
        {
            case "COMPLETED":
                foundmatch = true;
                SetGameSessionInfo(foundTiket, ref matchMakingReponse);
                break;
            case "PLACING":
            case "QUEUED":
            case "SEARCHING":
                break;
            case "TIMED_OUT":
            case "FAILED":
            case "CANCELLED":
                failedmatch = true;
                break;
        }
        break;
    }
}
```

## Protocol Definition (`match_making.proto`)

| Service | RPC | Description |
|---|---|---|
| `MatchMakingSvc` | `StartMatchMaking` | Starts matchmaking for the client |
| | `CreatePlayerSession` | Joins a player to a specific session |
| | `CreateGameSession` | Creates a new (or reused) game session |
| `MatchMakingSvcDS` | `StartMatchMakingDS` | Dedicated server matchmaking |
| | `CreateGameSessionDS` | Dedicated server session creation |

The GameLift API response structures — session connection info (`GameSessionConnectionInfo`), the matchmaking ticket (`MatchmakingTicket`), and player attributes (`PlayerAttribute`, including a latency map) — are mirrored directly as gRPC messages, so clients can use the connection information immediately.

## Tech Stack

- **Language/Framework**: C# / ASP.NET Core (gRPC), .NET
- **Cloud**: AWS GameLift (FlexMatch, Fleets, Aliases)
- **Authentication**: JWT Bearer (EOS OpenID Connect, S2S OpenID Connect)
- **Serialization**: Protocol Buffers
- **Other**: gRPC health checks, forwarded headers (for reverse proxy support), custom Kestrel timeout configuration

## Design Points

- **Cost optimization**: Searches for an existing session with an open slot before starting new matchmaking (FlexMatch), reducing unnecessary session creation.
- **Region-aware routing**: Automatically selects the nearest GameLift region/alias based on the per-region latency values sent by the client.
- **Dual trust boundary**: Clearly separates the client (EOS) and dedicated server (S2S) at the authentication scheme/policy level, enforcing that server-only APIs can never be called with a regular user token.
- **Asynchronous polling-based state management**: Waits for GameLift to complete session placement/matchmaking using `Task.Delay`-based polling (SNS integration is not yet in place, with a TODO in the code noting a planned future move to an event-driven approach).
