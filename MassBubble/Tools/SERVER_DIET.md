# 서버 다이어트 (Server Diet)

데디케이트 서버가 어떤 모듈과 플러그인을 로드하는지, 그로 인해 발생하는 비용(메모리/시간)은 얼마인지, 실제로 사용 중인 곳이 있는지, 그리고 일부를 비활성화했을 때 어떤 변화가 생기는지 추적합니다. 출력 결과는 JSON 형식(실행당 하나의 보고서 생성)으로 제공되므로, 두 개의 빌드를 서로 직관적으로 비교할 수 있습니다.

## 파일 구성

| 파일 | 역할 |
| :--- | :--- |
| `Source/MassBubble/Core/MassBubbleDiet.h/.cpp` | 리포터(보고서 생성기) 본체입니다. `-OptDietReport`, `-OptDietModules`, `opt.diet.Report`, `opt.diet.Plugins` 옵션을 지원합니다. |
| `Tools/ServerDiet/Run-ServerDiet.ps1` | 서버를 N번 실행하고, 그 결과로 `runN.json` 및 `runN.log` 파일을 수집합니다. |
| `Tools/ServerDiet/Get-DietCandidates.ps1` | 어떤 플러그인을 제거할 수 있는지, 반대로 다른 플러그인들은 왜 유지해야 하는지 분석하여 `.uproject`에 작성할 항목을 찾아냅니다. |
| `Tools/ServerDiet/Compare-ServerDiet.ps1` | 다이어트 적용 전/후를 비교합니다. 모듈, 플러그인, `Module.*` 마일스톤, `ProcessMB`, 그리고 새로 발생한 경고(Warning) 등을 대조합니다. |

---

## 작업 흐름 (Workflow)

1. 현재 상태 그대로 서버를 스테이징(Staging)합니다. (Development 또는 Test 빌드는 콘솔 명령어를 유지하므로 편리하며, 해당 옵션과 보고서 기능은 Shipping 빌드에서도 정상 작동합니다).
2. `.\Run-ServerDiet.ps1 -Exe <staged>\MassBubbleServer.exe -Label before -Runs 5 -ExtraArgs '-OptBots=8'` 명령어를 실행합니다.
3. `.\Get-DietCandidates.ps1 -Report .\DietRuns\before -Uproject <project>\MassBubble.uproject -OutFile .\diet-plan.txt` 명령어를 실행합니다.
4. 출력된 항목들을 프로젝트의 `.uproject` 파일 내 `Plugins` 배열에 붙여넣은 뒤, 프로젝트를 다시 빌드하고 서버를 새로 스테이징합니다.
5. `.\Run-ServerDiet.ps1 -Exe <staged-after>\MassBubbleServer.exe -Label after -Runs 5 -ExtraArgs '-OptBots=8'` 명령어를 실행합니다.
6. `.\Compare-ServerDiet.ps1 -Before .\DietRuns\before -After .\DietRuns\after` 명령어로 전후 결과를 비교합니다.

> ⚠️ **주의:** 메모리 수치를 정확하게 비교하려면 두 실험 모두 동일한 맵과 동일한 `-ExtraArgs`를 사용해야 합니다.
> - 실행 중인 서버에서 수동으로 확인하려면: `-OptDietModules` 옵션으로 시작한 뒤, 콘솔에 `opt.boot.Modules 1` (로드된 모든 모듈과 해당 플러그인 표시), `opt.diet.Plugins 1`, `opt.diet.Report <label>`을 차례로 입력합니다.

---

## 보고서 포함 항목

- **milestones**: 부팅 타임라인 정보입니다. 첫 번째 마일스톤 및 프로세스 시작 시점(`GStartTime`) 이후 경과한 시간(ms)을 기록합니다. `-OptDietModules` 옵션을 켜면 MassBubble이 시작된 후 로드된 모든 모듈에 대해 `Module.<Name>.Loaded`가 추가되며, 플러그인 로딩 단계마다 `Module.Phase.<Phase>.Done`이 기록됩니다.
- **memory_samples / modules[].mb_at_load**: 프로세스 크기(`UsedPhysical` 수치로, OptCrowd/ProcessMB CSV 통계와 동일한 값)를 기록합니다. 훅(Hook)이 설치된 시점, 각 모듈이 로드될 때, `Engine.InitComplete` 시점, 그리고 보고서 작성 시점의 메모리를 각각 측정합니다.
- **modules[]**: 로드된 모듈 정보입니다. 소유 플러그인, 로드 시간, 등록된 uclasses, 그리고 해당 클래스의 라이브 객체 수(instances)를 포함합니다. MassBubble이 시작되기 전에 로드된 모듈은 로드 시간이 표시되지 않습니다(`loaded_before_hook`).
- **plugins[]**: 활성화된 플러그인 정보입니다. 로드된 모듈 수, 필수/선택적 플러그인 종속성, 플러그인 클래스의 라이브 객체 수, 마운트된 콘텐츠 내의 오브젝트 수를 보여줍니다.
- **objects, executable_mb, timing, memory** (안정화 상태 및 피크 수치).

---

## 결과 해석 시 유의사항

- **"사용 중(In use)"의 의미**: 라이브 객체나 콘텐츠 오브젝트가 실제로 존재함을 뜻합니다. 오브젝트가 0개라고 해서 해당 플러그인이 절대 사용되지 않는다고 단정할 수는 없습니다 (아직 로드되지 않은 맵에서 사용 중일 수 있음). 따라서 `Compare-ServerDiet.ps1`을 통한 로그 차이(diff) 비교가 중요합니다. 누락된 클래스나 로드 실패 오류가 이 로그에 가시적으로 나타나기 때문입니다.
- 측정 방식의 특성상 연속적인 로드 이벤트 간의 차이값만을 계산하므로, 모듈 로드 사이에 실행된 다른 작업의 비용까지 해당 모듈의 비용으로 산정될 수 있습니다. 또한, 모놀리식(Monolithic) 빌드에서는 링크만 되고 실제 실행되지 않은 모듈의 경우 로드 이벤트 자체가 발생하지 않지만, 엔진 시작 시점에 UClasses, UObjects, 실행 파일 크기(MB) 등의 데이터에는 여전히 등록되어 메모리를 차지합니다.
- `Enabled:false` 설정과 함께 `TargetAllowList:["Server"]`를 선언하면 오직 서버에서만 해당 플러그인을 비활성화 할 수 있습니다. 프로젝트가 명시적으로 활성화한 플러그인의 경우에는 `TargetDenyList:["Server"]`를 사용해 서버에서 제외합니다. 만약 유지하려는 다른 플러그인이 꺼진 플러그인을 필요로 한다면 UBT(Unreal Build Tool)가 에러를 내며 멈추므로, 종속성 꼬임을 안전하게 방지할 수 있습니다.

---

## 📊 측정 결과 (Measured result)

스테이징된 데디케이트 서버 환경에서 `-OptBots=8` 옵션을 적용해 테스트를 진행했습니다. 첫 번째 웜업(Warm-up) 실행 결과는 제외하고 라벨당 5번의 유효한 실행 결과를 수집했습니다 (`Run-ServerDiet.ps1 -Runs 5 -ExtraArgs '-OptBots=8'`). 두 라벨 모두 동일한 실행 파일 경로와 인자를 사용했습니다. 

서버에서만 플러그인을 비활성화한 결과(`Enabled:false + TargetAllowList:["Server"]`), 전체 192개 플러그인 중 107개를 꺼버릴 수 있었습니다. 제거된 플러그인 목록은 `Compare-ServerDiet.ps1` 결과의 `Plugins only in BEFORE` 블록에서 확인할 수 있습니다.

### 테스트 실행 로그 (Runs)

| 실행 항목 | 종료 코드 | 실소요 시간 (초) | 로드된 모듈 수 | 초기화 시간 (ms) | 프로세스 메모리 (MB) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| before, run 1 | 0 | 16.9 | 279 | 1,194 | 352.1 |
| before, run 2 | 0 | 16.9 | 279 | 1,191 | 351.9 |
| before, run 3 | 0 | 16.9 | 279 | 1,184 | 352.0 |
| before, run 4 | 0 | 16.9 | 279 | 1,186 | 352.3 |
| before, run 5 | 0 | 16.9 | 279 | 1,215 | 351.9 |
| after, run 1 | 0 | 16.4 | 206 | 1,063 | 336.6 |
| after, run 2 | 0 | 16.4 | 206 | 1,042 | 335.9 |
| after, run 3 | 0 | 16.5 | 206 | 1,051 | 336.2 |
| after, run 4 | 0 | 16.5 | 206 | 1,053 | 335.8 |
| after, run 5 | 0 | 16.5 | 206 | 1,075 | 336.4 |

각 실행 결과는 매우 일치합니다. 라벨별 `process_mb` 오차 범위는 0.5 MB 미만이며 모듈 수도 일정하므로, 아래 요약된 중간값(Median) 데이터는 신뢰할 수 있습니다. 통계에서 제외된 초기 웜업 실행(적용 전: 1,303 ms / 352.2 MB, 적용 후: 1,112 ms / 335.9 MB)은 초기화(Init) 단계만 다소 느리게 측정되었습니다.

### 주요 지표 요약 (Headline numbers - 중간값 기준)

| 측정 항목 (Metric) | 다이어트 전 (Before) | 다이어트 후 (After) | 차이 (Delta) | 변화율 (Delta %) |
| :--- | :---: | :---: | :---: | :---: |
| 로드된 모듈 수 (modules loaded) | 279 | 206 | -73 | -26.2% |
| 활성화된 플러그인 수 (plugins enabled) | 192 | 85 | -107 | -55.7% |
| 코드가 로드된 플러그인 수 (plugins with code loaded) | 133 | 71 | -62 | -46.6% |
| Module.*.Loaded 발생 이벤트 수 | 141 | 105 | -36 | -25.5% |
| 모듈 로드 시간 범위 (module load span, ms) | 257 | 229 | -28 | -10.8% |
| Module.Phase.Default.Done (프로세스 시작 후 ms) | 1,047 | 913 | -134 | -12.8% |
| Engine.InitComplete (프로세스 시작 후 ms) | 1,191 | 1,053 | -137 | -11.5% |
| 훅 설치 시점 ProcessMB | 273.9 | 258.5 | -15.4 | -5.6% |
| Engine.InitComplete 시점 ProcessMB | 296.5 | 278.3 | -18.2 | -6.1% |
| 보고서 출력 시점 ProcessMB (안정화) | 352.0 | 336.2 | -15.9 | -4.5% |
| 피크 물리 메모리 (peak physical, MB) | 767.6 | 751.6 | -16.0 | -2.1% |
| 가상 메모리 (virtual, MB) | 251.3 | 244.6 | -6.7 | -2.7% |
| 생성된 UObjects 개수 | 38,782 | 36,622 | -2,160 | -5.6% |
| 생성된 UClasses 개수 | 5,811 | 5,573 | -238 | -4.1% |
| 바이너리 실행 파일 크기 (executable, MB) | 305.9 | 289.9 | -16.0 | -5.2% |

### 부팅 마일스톤 상세 (Startup milestones - 중간값 기준, ms)

| 마일스톤 단계 | 다이어트 전 (Before) | 다이어트 후 (After) | 단축 시간 (Delta) |
| :--- | :---: | :---: | :---: |
| Module.MassBubble.Startup | 980 | 866 | -114 |
| Module.Phase.Default.Done | 1,047 | 913 | -134 |
| Module.Phase.PostDefault.Done | 1,053 | 919 | -134 |
| Module.Phase.PostEngineInit.Done | 1,111 | 974 | -137 |
| Engine.InitComplete | 1,191 | 1,053 | -137 |
| Diet.ReportDue | 16,216 | 16,084 | -132 |

성능 이득의 대부분은 MassBubble이 시작되기도 전(-114 ms 시점)에 이미 확보되었습니다. 불필요한 플러그인을 제거함으로써 초기 모듈 및 플러그인 로딩 연산 자체가 생략되었기 때문입니다. 이후 발생하는 `Module.<Name>.Loaded` 마일스톤들은 평균 137~145 ms씩 앞당겨졌습니다. 이 단축 효과는 누적(Cumulative)되므로, 앞선 모듈이 생략될수록 뒤에 오는 모듈들의 로드 시점도 함께 당겨집니다.

---

## 제거된 플러그인의 비용 분석

- **기능적 손실 없음**: 다이어트 적용 후의 로그를 비교했을 때 `Plugins only in AFTER` 및 `Modules only in AFTER` 항목은 비어 있었으며, 새롭게 발생한 Warning이나 Error 로그는 전/후 모두 0건으로 완벽하게 안전했습니다.
- **라이브 객체 낭비 제거**: 제거된 플러그인들이 생성하던 실시간 객체(Live objects)는 거의 전무한 수준이었습니다 (Mutable 15개, PhysicsControl 2개, 그 외 플러그인은 모두 0개). 에셋 콘텐츠 오브젝트 역시 모두 0개였습니다.
- 개별 모듈 로드 샘플링 상으로는 약 1.4 MB 및 19 ms 정도의 절약만 보였으나, 실제 측정된 전체 결과로는 안정화 메모리 15.9 MB 감소, `Engine.InitComplete`까지 137 ms 단축이라는 더 큰 이득을 보였습니다. 단순 샘플링 방식으로는 링크는 되었으나 실행되지 않은 모듈의 숨은 비용을 찾아내지 못합니다. 이 모듈들은 시작 시점에 여전히 클래스를 등록하고(-238 UClasses, -2,160 UObjects) 바이너리 용량을 차지하기 때문입니다(-16.0 MB). 따라서 최적화 성과를 산정할 때는 샘플들의 단순 합산이 아니라, 최종 측정된 델타(Delta) 값을 지표로 삼아야 합니다.

---

## 수치 분석 결론

1. **실행 파일 크기(`executable (MB)`)와 안정화된 프로세스 메모리(`ProcessMB settled`)가 동일하게 16 MB 감소**했습니다. 이는 메모리 절감 효과가 런타임에 동적으로 늘어나는 힙(Heap) 메모리가 아니라, 컴파일 단계에서 링크된 코드 자체와 초기 등록 비용이 줄어들었음을 증명합니다.
2. **물리 메모리 피크치(`peak physical`) 역시 동일하게 16.0 MB 감소**한 것으로 보아, 8개의 봇(Bot)이 구동 중인 부하 상황에서도 최적화 효율이 그대로 유지됨을 알 수 있습니다.
3. 전체 프로세스 구동 시간(`wall_s`)은 보고서 타이머 규격(`Diet.ReportDue` 약 16초 고정)에 종속되므로 큰 차이가 없어 보일 수 있습니다. 따라서 구동 속도 개선 여부를 판단할 때는 `wall_s` 대신 `init_ms` 및 `Engine.InitComplete` 수치를 대조하는 것이 정확합니다.