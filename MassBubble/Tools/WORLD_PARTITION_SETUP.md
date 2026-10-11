# World Partition 맵 만들기와 서버 스트리밍 검증

월드 파티션 맵(`.umap`과 One-File-Per-Actor 패키지)은 바이너리 파일이므로 이 저장소에 포함되지 않습니다.
에디터에서 맵을 생성한 후 `Tools/PopulateTestWorld.py` 스크립트를 실행하여 테스트 콘텐츠를 자동으로 배치할 수 있습니다. (UE 5.8 UI 기준)

## 1. 맵 만들기 및 테스트 콘텐츠 배치 (약 5분)

1. 에디터 실행 → **File → New Level → Open World** (World Partition이 활성화된 템플릿 선택).

2. 템플릿에 기본 포함된 지형 및 샘플 액터는 필요 없으므로 **Outliner에서 제거**합니다. (또는 *Basic* 레벨로 생성한 뒤 **World Settings → World Partition Setup → Enable World Partition**을 체크하여 변환할 수도 있습니다.)

3. `/Game/Maps/L_MassBubbleWorld` 경로로 맵을 저장합니다. 이름과 경로가 `Config/DefaultEngine.ini`의 `GameDefaultMap`/`ServerDefaultMap`과 반드시 일치해야 합니다.

4. **World Settings** 항목 확인:

   * *Game Mode Override*는 비워 둡니다 (`AMassBubbleGameMode`가 프로젝트 기본값으로 설정되어 있습니다).

   * *World Partition → Runtime Settings*: 기본 `MainGrid`의 **Cell Size = 12800 (128 m)**, Loading Range = 25600 (256 m)로 설정합니다.
     (`UCrowdSettings::RegionSizeCm`(12800)과 셀 크기를 동일하게 유지하는 것이 이 프로젝트의 전제 조건입니다.)

   * *서버 스트리밍*: 서버 스트리밍 모드가 활성화되어 있는지 확인합니다. 프로젝트 기본값은 `DefaultEngine.ini`의 `wp.Runtime.EnableServerStreaming=1`입니다.

5. Tools → Execute Python Script…를 선택하고 `Tools/PopulateTestWorld.py`를 실행합니다.

   * **스크립트 주요 특징 (`PopulateTestWorld.py`):**

     * **백그라운드 틱 실행 (`tick` 모드):** 에디터가 멈추지(Freeze) 않도록 작업을 프레임 단위로 쪼개어 처리하며, Output Log를 통해 실시간 진행 상황을 확인할 수 있습니다.

     * **중단점 재개 (Resume) 지원:** 이미 생성된 액터(라벨 기준)는 건너뛰므로 스크립트가 중단되더라도 기존 데이터를 중복 생성하지 않고 이어 실행할 수 있습니다.

     * **체크포인트 저장:** 설정된 주기(`SAVE_EVERY = 1000`)마다 자동으로 패키지를 저장하여 안정성을 높입니다. (실행 중 다시 스크립트를 실행하면 기존 작업이 취소됩니다.)

     * **생성 결과:** 넓은 바닥 1장(`OptFloor`, `Is Spatially Loaded = false`), 약 2,600개의 스트리밍 대상 블록(`OptBlock_x_y`, 약 2km × 2km 범위), 그리고 원점에 `OptPlayerStart` 1개가 자동 생성됩니다.

6. 봇 궤도 반경 기본값이 600 m(`BotOrbitRadiusCm`)이므로 맵 전체 폭이 최소 1.2 km 이상이어야 합니다. 더 작은 맵을 사용 중이라면 *Project Settings → Game → MassBubble Crowd → Bots*에서 반경을 줄여주세요.

> **참고:** 군중은 레벨에 직접 배치되지 않습니다. Mass 엔티티는 서버 서브시스템 내부에서 관리되며, 맵은 지면과 스트리밍 대상 액터 역할만 담당합니다.

## 2. 서버 스트리밍 활성화 여부 확인

1. **Dedicated Server** 환경에서 확인합니다 (`README.md`의 "Dedicated Server로 직접 띄우기" 또는 PIE *Play As Client* 활용).

   * 리슨 서버(Listen Server) 환경에서는 서버가 호스트 클라이언트와 겸용되므로 "서버 스트리밍 on/off"에 따른 차이가 눈에 잘 드러나지 않습니다.

2. 서버 콘솔(Dedicated Server 로그 창) 또는 `-ExecCmds` 명령줄 입력:

   ```
   wp.Runtime.EnableServerStreaming        # 현재 값 출력 (1이어야 함)
   wp.Runtime.ToggleDrawRuntimeHash2D      # 런타임 해시 셀 시각화 (버전에 따라 명령어가 다를 수 있음)
   
   ```

3. 플레이어를 셀 경계 바깥으로 이동시켜 봅니다 (`AMassBubbleCharacter`는 WASD/EQ/Space 비행 가능, 최대 30 m/s).

   * 서버 월드 기준으로 멀어진 셀의 블록들이 **정상적으로 언로드되는지** 확인합니다. 서버 스트리밍이 꺼져 있다면 모든 셀이 메모리에 유지됩니다.

## 3. 군중과 스트리밍 소스 연동 확인

서버 콘솔에 다음 명령어를 입력하여 테스트합니다:

```
opt.crowd.SpawnBots 8
opt.crowd.Stats

```

| 검증 항목 | 기대 결과 | 
| ----- | ----- | 
| **봇 이동 궤도** | 봇 위치 주변의 월드 파티션 셀이 서버에 로드됨 (`UWorldPartitionStreamingSourceComponent`가 서버 스트리밍 소스로 자동 등록됨) | 
| **`Stats`의 영역 정보** (`regions(... active=…)`) | 플레이어 1명당 최대 25개 + 봇 1명당 최대 25개 (영역이 겹칠 경우 합산됨) | 
| **LOD 단계** (`LOD tiers`) | 봇과 플레이어 근처는 High/Medium, 먼 거리는 Off로 전환됨 | 
| **봇 제거 테스트** (`opt.crowd.ClearBots` 후 10초 이상 대기) | `despawning` → `inactive` 상태로 전환되며 `alive` 수치가 감소함 | 
| **재방문 테스트** | 봇이 지나갔던 자리에 다시 돌아왔을 때 동일한 NetId 및 위치 부근으로 복원됨 (`Saved` 경로 확인) | 

> **실패 예시 검증:** 일부러 `wp.Runtime.EnableServerStreaming 0`으로 설정한 뒤 재시작해 보면, 서버가 모든 셀을 강제로 로드하여 메모리 사용량과 로딩 시간이 늘어나며 스트리밍 소스 앵커가 무력화되는 현상을 확인할 수 있습니다.

## 4. 배치형 스트리밍 앵커 (선택 사항)

레벨에 `AMassBubbleStreamingAnchor` 액터를 직접 배치하면 특정 중요 지점(예: 마을 광장)의 인구와 셀을 상시 로드 상태로 유지할 수 있습니다.

* Details 패널에서 **World Partition → Is Spatially Loaded = false**로 반드시 변경해야 합니다. 켜둘 경우 앵커 자체가 로딩 대상 셀 안으로 들어가 순환되면서 의도대로 동작하지 않을 수 있습니다.

* 클라이언트 사본에서는 `BeginPlay` 시점에 스트리밍 소스가 자동으로 비활성화됩니다. (클라이언트는 자신의 플레이어 위치 기준으로만 스트리밍해야 하기 때문입니다.)

## 5. 문제 해결 (Troubleshooting)

| 증상 | 원인 후보 및 조치 방법 | 
| ----- | ----- | 
| **에디터 시작 시 맵을 찾지 못함** | 맵 저장 경로가 `/Game/Maps/L_MassBubbleWorld`가 아닐 수 있습니다. 저장 위치를 확인하거나 `DefaultEngine.ini`의 맵 설정을 수정하세요. | 
| **군중이 화면에 보이지 않음** | 클라이언트 콘솔에서 `opt.crowd.RenderStats 1` 실행 후 `bubble=0`이면 버블 미도착 상태입니다 (서버 로그에서 `CrowdBubble … created` 확인). `bubble>0`인데 안 보인다면 바닥 아래에 생성되었거나 메시 설정에 문제가 있는 것입니다. | 
| **`SpawnBots` 명령이 무시됨** | 클라이언트 창 콘솔에서 실행했을 가능성이 높습니다. **서버 월드** 콘솔(리슨 서버 창 또는 Dedicated Server 콘솔)에서 실행해야 합니다. | 
| **서버 로그에 군중 서브시스템이 안 보임** | Game/PIE 월드가 아니거나 세션이 `NM_Client` 상태인지 확인하세요. | 
| **블록이 서버에서 언로드되지 않음** | 서버 스트리밍이 꺼져 있거나, 봇 또는 앵커가 해당 셀을 스트리밍 소스 범위 내에 계속 유지하고 있기 때문입니다. | 
