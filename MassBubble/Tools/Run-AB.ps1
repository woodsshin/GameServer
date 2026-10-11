<#
 MassBubble A/B 매트릭스 러너 (PROFILING.md 3절)

 예)  .\Run-AB.ps1 -ServerExe D:\Pkg\WindowsServer\MassBubbleServer.exe `
                   -ClientExe D:\Pkg\Windows\MassBubble.exe `
                   -CsvDir   D:\Pkg\WindowsServer\MassBubble\Saved\Profiling\CSV `
                   -Cases A0,A1,A2

 케이스마다: 서버 기동 -> (csvprofile frames=N) -> 완료 대기 -> 종료 -> CSV에서 워밍업 제외 후 평균/p99 집계.
 결과: <Out>\summary.csv, <Out>\<케이스>_b<봇>_c<클라>.csv / .log

 반복 측정:  -Repeat 3                 선택한 모든 케이스를 3회씩 (파일명 끝에 _r1 _r2 _r3)
             -Cases A10_A3b_x3         A0 + A10_legacy + A3b_ts0_par1 + A3b_ts0_par0 를 3회씩 (약 12회 x 2.2분 = 약 27분)
 반복 실행은 케이스를 번갈아 돈다 (A0,A10,par1,par0, A0,A10,... ): 시간에 따른 드리프트가 한 케이스에만 몰리지 않게.
 결과: summary.csv (실행마다 1행) + summary_agg.csv (케이스별 mean/min/max/sd) + 비교 결과(콘솔)

 GC / PCG 케이스 (PROFILING.md 6.2의 후속 측정)
   Incremental GC : -Cases GC_x3 | GC_full_x3 | GC_churn_x3     (GC_inc_on 대 GC_inc_off, 구성요소별 분해, 시간 제한, 셀 교체가 잦은 부하)
   PCG 스폰 계획  : -Cases PCG_x3 | PCG_fill_x3 | PCG_burst_x3 -SpawnGraph /Game/PCG_MassAgentSpawner.PCG_MassAgentSpawner
   GC_* 케이스는 gc.* CVar를 -ExecCmds로 설정하고 같은 CVar를 다시 조회해서 서버 로그에 값을 남긴다.
   값이 로그에서 확인되지 않으면 경고한다 (LogMustHave). PCG 케이스도 "spawn graph ready"를 확인한다.
   OptCrowd/GCCount, PlansOk, PlanLatency*Ms 열은 MassBubble-GC-PCG-csv-stats.patch를 적용한 빌드에만 있다 (없으면 n/a).
#>
param(
	[Parameter(Mandatory)] [string]$ServerExe,
	[string]$ClientExe = "",                       # 비우면 클라이언트 없이 실행 (A4~A7은 클라이언트 필요!)
	[Parameter(Mandatory)] [string]$CsvDir,        # 서버가 CSV를 쓰는 Saved\Profiling\CSV
	[string]$Map = "/Game/Maps/L_MassBubbleWorld",
	[int]$Bots = 8,
	[int]$Clients = 1,
	[int]$Warmup = 30,                             # 초. 봇 램프(봇수 x BotRampSec)보다 길어야 함
	[int]$Duration = 60,                           # 초. 측정 구간
	[int]$ServerFps = 30,                          # 서버 틱레이트 (frames 계산용)
	[string[]]$Cases = @("A0"),
	[int]$Repeat = 1,                              # 각 케이스를 몇 번 반복할지 (그룹 케이스는 자체 반복 수 사용)
	[string]$Out = "ab_results",
	[string]$SpawnGraph = ""                       # PCG 케이스용 그래프 에셋 경로 (예: /Game/PCG_MassAgentSpawner.PCG_MassAgentSpawner). 비우면 DefaultGame.ini 값 사용
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path   # absolute: [IO.File] APIs do not follow the PowerShell location
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture

# Cmds: 서버 -ExecCmds (런타임 CVar) / Args: 서버 명령줄 (재시작이 필요한 항목, ini 오버라이드)
$GameIni = "-ini:Game:[/Script/MassBubble.CrowdSettings]"

# PCG 켜기: bUsePCG는 월드 시작 때 읽으므로 CVar가 아니라 서버 명령줄(ini 오버라이드)로 넘긴다.
$PcgArgs = @("${GameIni}:bUsePCG=True")
if ($SpawnGraph -ne "") { $PcgArgs += "${GameIni}:SpawnGraph=$SpawnGraph" }
$PcgHave    = @('\[PCG_MassAgentSpawner\] spawn graph ready', '\[PCG_MassAgentSpawner\] first plan arrived')
$PcgNotHave = @('did not finish within', 'run failed or was cancelled', 'could not be loaded', 'could not be requested', 'no Spawn Graph is set')

# Incremental GC 케이스. Reach / Gather / BeginDestroy는 0 또는 1.
#   gc.AllowIncrementalReachability, gc.AllowIncrementalGather, gc.IncrementalBeginDestroyEnabled (+ 시간 제한 CVar)
# 값 없이 CVar 이름만 실행하면 현재 값이 로그에 찍힌다. 스크립트가 그 줄을 찾아 설정이 실제로 적용됐는지 확인한다.
# opt.hitch.LogMs 40: 서버 프레임이 33.3 ms로 고정이라 그 이상일 때만 [Hitch] 줄이 남는다.
function New-GcCase([int]$Reach, [int]$Gather, [int]$BeginDestroy, $LimitSec = $null, [hashtable]$Extra = @{}) {
	$Cmds = @("gc.AllowIncrementalReachability $Reach", "gc.AllowIncrementalGather $Gather", "gc.IncrementalBeginDestroyEnabled $BeginDestroy")
	if ($null -ne $LimitSec) {
		$Cmds += "gc.IncrementalReachabilityTimeLimit $LimitSec"
		$Cmds += "gc.IncrementalBeginDestroyTimeLimit $LimitSec"
	}
	$Cmds += @("gc.AllowIncrementalReachability", "gc.AllowIncrementalGather", "gc.IncrementalBeginDestroyEnabled",
	           "gc.IncrementalReachabilityTimeLimit", "gc.IncrementalBeginDestroyTimeLimit", "opt.hitch.LogMs 40")
	$BdPattern = if ($BeginDestroy -ne 0) { '(1|True)' } else { '(0|False)' }
	$Case = @{
		Cmds     = $Cmds
		Args     = @()
		Duration = 90      # GC는 5-9초 간격이라 60초 구간에는 표본이 적다
		LogMustHave = @(
			('gc\.AllowIncrementalReachability\s*=\s*"?{0}\b' -f $Reach),
			('gc\.AllowIncrementalGather\s*=\s*"?{0}\b' -f $Gather),
			('gc\.IncrementalBeginDestroyEnabled\s*=\s*"?{0}\b' -f $BdPattern))
	}
	foreach ($K in $Extra.Keys) { $Case[$K] = $Extra[$K] }
	return $Case
}
$Matrix = [ordered]@{
	"A0"      = @{ Cmds = @();                              Args = @() }
	"A1"      = @{ Cmds = @("opt.crowd.TimeSlicing 0");     Args = @() }
	"A2"      = @{ Cmds = @("opt.crowd.LOD 0");             Args = @() }
	"A3"      = @{ Cmds = @("opt.crowd.ParallelMove 0");    Args = @() }
	"A4"      = @{ Cmds = @("opt.crowd.Replicate 0");       Args = @() }
	"A5"      = @{ Cmds = @("opt.crowd.DeadReckoning 0");   Args = @() }
	"A6_20Hz" = @{ Cmds = @("opt.crowd.ReplicationHz 20");  Args = @() }
	"A6_5Hz"  = @{ Cmds = @("opt.crowd.ReplicationHz 5");   Args = @() }
	"A7"      = @{ Cmds = @();                              Args = @("-ini:Engine:[SystemSettings]:net.IsPushModelEnabled=0") }
	"A8_800"  = @{ Cmds = @();                              Args = @("${GameIni}:AgentsPerRegion=800") }
	"A8_1600" = @{ Cmds = @();                              Args = @("${GameIni}:AgentsPerRegion=1600") }
	"A9_b4"   = @{ Cmds = @();                              Args = @(); Bots = 4 }
	"A9_b16"  = @{ Cmds = @();                              Args = @(); Bots = 16 }
	"A9_nobudget"  = @{ Cmds = @("opt.crowd.BudgetedPump 0"); Args = @("${GameIni}:MaxSpawnPerFrame=5000"); Bots = 16 }
	# ---- follow-up measurements (PROFILING.md 6.2) ----
	"A3b_ts0_par1" = @{ Cmds = @("opt.crowd.TimeSlicing 0");                              Args = @() }   # ParallelMove with 58k sim/frame
	"A3b_ts0_par0" = @{ Cmds = @("opt.crowd.TimeSlicing 0", "opt.crowd.ParallelMove 0");  Args = @() }
	"A9_b16_w0"    = @{ Cmds = @("opt.hitch.LogMs 50");                                    Args = @(); Bots = 16; Warmup = 0 }   # includes the initial spawn burst
	"A9_nobudget_w0" = @{ Cmds = @("opt.crowd.BudgetedPump 0", "opt.hitch.LogMs 50");      Args = @("${GameIni}:MaxSpawnPerFrame=5000"); Bots = 16; Warmup = 0 }
	"A10_legacy"   = @{ Cmds = @();                                                        Args = @("-UseIrisReplication=0") }    # legacy NetDriver instead of Iris
	"A11_c2"       = @{ Cmds = @();                                                        Args = @(); Clients = 2 }
	"A11_c4"       = @{ Cmds = @();                                                        Args = @(); Clients = 4 }
	# ---- 3rd round: spawn burst (all bots at once, the case the spawn budget is meant for) and push model under legacy ----
	"A9_burst_b_w0"  = @{ Cmds = @("opt.crowd.BotRampSec 0");                                 Args = @();                                   Bots = 16; Warmup = 0 }
	"A9_burst_nb_w0" = @{ Cmds = @("opt.crowd.BotRampSec 0", "opt.crowd.BudgetedPump 0");     Args = @("${GameIni}:MaxSpawnPerFrame=5000"); Bots = 16; Warmup = 0 }
	"A7_legacy"      = @{ Cmds = @(); Args = @("-UseIrisReplication=0", "-ini:Engine:[SystemSettings]:net.IsPushModelEnabled=0") }
	"A9_snapall"   = @{ Cmds = @("opt.crowd.SnapshotScope 0"); Args = @() }

	# ---- Incremental GC (엔진 gc.* CVar. 프로젝트 기본값은 DefaultEngine.ini에서 전부 켜짐) ----
	# GC_inc_on은 "켜짐"을 명시적으로 다시 설정한 기준선이다: ini 값에 기대지 않고 같은 조건으로 비교한다.
	"GC_inc_on"     = (New-GcCase 1 1 1)
	"GC_inc_off"    = (New-GcCase 0 0 0)                 # 증분 도달성 분석 + 증분 수집 + 증분 BeginDestroy 전부 끔
	"GC_off_reach"  = (New-GcCase 0 1 1)                 # 분해: 증분 도달성 분석만 끔
	"GC_off_gather" = (New-GcCase 1 0 1)                 # 분해: 증분 수집만 끔
	"GC_off_bd"     = (New-GcCase 1 1 0)                 # 분해: 증분 BeginDestroy만 끔
	"GC_tl_05ms"    = (New-GcCase 1 1 1 0.0005)          # 시간 제한 0.5 ms (기본 2 ms)
	"GC_tl_8ms"     = (New-GcCase 1 1 1 0.008)           # 시간 제한 8 ms
	# 셀 로드/언로드가 잦은 부하: 봇을 빠르게 돌려 GC 횟수를 늘린다
	"GC_churn_on"   = (New-GcCase 1 1 1 $null @{ Bots = 16; Args = @("${GameIni}:BotSpeedCmPerSec=4000") })
	"GC_churn_off"  = (New-GcCase 0 0 0 $null @{ Bots = 16; Args = @("${GameIni}:BotSpeedCmPerSec=4000") })

	# ---- PCG 스폰 계획 (PCG_MassAgentSpawnerPlanner). P0 = 랜덤 배치(플래너 없음), P1 = PCG 그래프 ----
	"P0_random"     = @{ Cmds = @(); Args = @("${GameIni}:bUsePCG=False"); LogMustNotHave = @('\\[PCG_MassAgentSpawner\\] loading spawn graph') }
	"P1_pcg"        = @{ Cmds = @(); Args = $PcgArgs;                                              LogMustHave = $PcgHave; LogMustNotHave = $PcgNotHave }
	"P1_pcg_c1"     = @{ Cmds = @(); Args = ($PcgArgs + @("${GameIni}:PCGMaxConcurrent=1"));       LogMustHave = $PcgHave; LogMustNotHave = $PcgNotHave }
	"P1_pcg_c8"     = @{ Cmds = @(); Args = ($PcgArgs + @("${GameIni}:PCGMaxConcurrent=8"));       LogMustHave = $PcgHave; LogMustNotHave = $PcgNotHave }
	# 워밍업 0: 시작 구간(에이전트가 채워지는 시간, Agents T90/T99)을 본다. 램프된 봇 16이 채워지는 데 약 42초
	"P0_random_w0"  = @{ Cmds = @(); Args = @("${GameIni}:bUsePCG=False"); Bots = 16; Warmup = 0; Duration = 90; LogMustNotHave = @('\\[PCG_MassAgentSpawner\\] loading spawn graph') }
	"P1_pcg_w0"     = @{ Cmds = @(); Args = $PcgArgs;                                              Bots = 16; Warmup = 0; Duration = 90; LogMustHave = $PcgHave; LogMustNotHave = $PcgNotHave }
	"P1_pcg_c1_w0"  = @{ Cmds = @(); Args = ($PcgArgs + @("${GameIni}:PCGMaxConcurrent=1"));       Bots = 16; Warmup = 0; Duration = 90; LogMustHave = $PcgHave; LogMustNotHave = $PcgNotHave }
	"P1_pcg_c8_w0"  = @{ Cmds = @(); Args = ($PcgArgs + @("${GameIni}:PCGMaxConcurrent=8"));       Bots = 16; Warmup = 0; Duration = 90; LogMustHave = $PcgHave; LogMustNotHave = $PcgNotHave }
	# 봇 16을 한꺼번에: 요청이 한 번에 쌓일 때 플래너 대기열과 스폰 지연
	"P0_random_burst_w0" = @{ Cmds = @("opt.crowd.BotRampSec 0"); Args = @("${GameIni}:bUsePCG=False"); Bots = 16; Warmup = 0; Duration = 90; LogMustNotHave = @('\\[PCG_MassAgentSpawner\\] loading spawn graph') }
	"P1_pcg_burst_w0"    = @{ Cmds = @("opt.crowd.BotRampSec 0"); Args = $PcgArgs;                      Bots = 16; Warmup = 0; Duration = 90; LogMustHave = $PcgHave; LogMustNotHave = $PcgNotHave }
}

# Group names expand into several cases plus a repeat count:  .\Run-AB.ps1 ... -Cases A10_A3b_x3
# A0 is part of the group because A10_legacy is only meaningful against a baseline measured the same number of times.
$Groups = @{
	"A10_A3b_x3" = @{ Cases = @("A0", "A10_legacy", "A3b_ts0_par1", "A3b_ts0_par0"); Repeat = 3 }
	# Incremental GC. 실행 시간 대략: x3 = 2 x 3 x 3.3분 = 약 20분, full_x3 = 7 x 3 x 3.3분 = 약 70분
	"GC_x3"       = @{ Cases = @("GC_inc_on", "GC_inc_off"); Repeat = 3 }
	"GC_full_x3"  = @{ Cases = @("GC_inc_on", "GC_inc_off", "GC_off_reach", "GC_off_gather", "GC_off_bd", "GC_tl_05ms", "GC_tl_8ms"); Repeat = 3 }
	"GC_churn_x3" = @{ Cases = @("GC_churn_on", "GC_churn_off"); Repeat = 3 }
	# PCG (-SpawnGraph 필요). fill = 시작 구간, burst = 봇 일시 투입
	"PCG_x3"       = @{ Cases = @("P0_random", "P1_pcg"); Repeat = 3 }
	"PCG_fill_x3"  = @{ Cases = @("P0_random_w0", "P1_pcg_w0", "P1_pcg_c1_w0", "P1_pcg_c8_w0"); Repeat = 3 }
	"PCG_burst_x3" = @{ Cases = @("P0_random_burst_w0", "P1_pcg_burst_w0"); Repeat = 3 }
}

# Pairs compared after repeated runs: target -> baseline
$Baselines = @{
	"A10_legacy"   = "A0"
	"A3b_ts0_par0" = "A3b_ts0_par1"
	"GC_inc_off"    = "GC_inc_on"
	"GC_off_reach"  = "GC_inc_on"
	"GC_off_gather" = "GC_inc_on"
	"GC_off_bd"     = "GC_inc_on"
	"GC_tl_05ms"    = "GC_inc_on"
	"GC_tl_8ms"     = "GC_inc_on"
	"GC_churn_off"  = "GC_churn_on"
	"P1_pcg"        = "P0_random"
	"P1_pcg_c1"     = "P1_pcg"
	"P1_pcg_c8"     = "P1_pcg"
	"P1_pcg_w0"     = "P0_random_w0"
	"P1_pcg_c1_w0"  = "P1_pcg_w0"
	"P1_pcg_c8_w0"  = "P1_pcg_w0"
	"P1_pcg_burst_w0" = "P0_random_burst_w0"
}

# 반복 측정 뒤 비교하는 지표 (Agg의 "<지표> mean/min/max" 열). 해당 열이 없는 쌍에서는 건너뛴다.
$CompareMetrics = "GT_ExclusiveSum avg", "GT_ExclusiveSum max", "OptCrowd/GameThread/Move avg", "GTFrames_gt10ms",
                  "Exclusive/GameThread/GarbageCollection avg", "Exclusive/GameThread/GarbageCollection max",
                  "OptCrowd/GameThread/Plan avg", "Agents T90 s"

function Get-Stat($rows, $col) {
	if (-not ($rows[0].PSObject.Properties.Name -contains $col)) { return @("n/a", "n/a", "n/a") }
	$v = $rows | ForEach-Object { $_.$col -as [double] } | Where-Object { $null -ne $_ } | Sort-Object
	if (-not $v) { return @("n/a", "n/a", "n/a") }
	$avg = ($v | Measure-Object -Average).Average
	$p99 = $v[[Math]::Min($v.Count - 1, [int][Math]::Floor($v.Count * 0.99))]
	return @([Math]::Round($avg, 3), [Math]::Round($p99, 3), [Math]::Round($v[-1], 3))
}

# 에이전트 수가 최종값(캡처 중 최댓값)의 Frac에 처음 도달한 시각(초). 워밍업 0 케이스에서 의미가 있다.
function Get-FillTime($All, $Fps, $Frac) {
	$Col = 'OptCrowd/Agents'
	if (-not ($All[0].PSObject.Properties.Name -contains $Col)) { return "n/a" }
	$Max = ($All | ForEach-Object { $_.$Col -as [double] } | Measure-Object -Maximum).Maximum
	if (-not $Max -or $Max -le 0) { return "n/a" }
	for ($i = 0; $i -lt $All.Count; $i++) {
		if (($All[$i].$Col -as [double]) -ge $Frac * $Max) { return [Math]::Round($i / $Fps, 1) }
	}
	return "n/a"
}

# 누적 카운터의 마지막 행 값 (PlansOk, PlanLatencyAvgMs ...)
function Get-LastValue($Rows, $Col) {
	if (-not ($Rows[0].PSObject.Properties.Name -contains $Col)) { return "n/a" }
	$V = $Rows[$Rows.Count - 1].$Col -as [double]
	if ($null -eq $V) { return "n/a" }
	return [Math]::Round($V, 3)
}

# 누적 카운터의 측정 구간 증가량 (마지막 행 - 첫 행). 예: 구간 안에서 GC가 몇 번 돌았는가
function Get-Delta($Rows, $Col) {
	if (-not ($Rows[0].PSObject.Properties.Name -contains $Col)) { return "n/a" }
	$First = $Rows[0].$Col -as [double]
	$Last = $Rows[$Rows.Count - 1].$Col -as [double]
	if ($null -eq $First -or $null -eq $Last) { return "n/a" }
	return [Math]::Round($Last - $First, 3)
}

# 서버 로그 검사: 케이스가 이름대로의 조건으로 돌았는지 (CVar가 적용됐는지, PCG 그래프가 로드됐는지) 확인하고 [Hitch] 줄 수를 센다.
function Test-CaseLog($Name, $Case, $LogPath) {
	$Result = [pscustomobject]@{ Hitches = "n/a" }
	if (-not (Test-Path -LiteralPath $LogPath)) { Write-Warning "$Name : server log not found: $LogPath (log checks skipped)"; return $Result }
	try { $Text = Get-Content -LiteralPath $LogPath -Raw -ErrorAction Stop }
	catch { Write-Warning "$Name : could not read the server log: $_"; return $Result }
	if ($null -eq $Text) { $Text = "" }

	$Result.Hitches = [regex]::Matches($Text, '\[Hitch\]').Count
	if ($Case.ContainsKey("LogMustHave")) {
		foreach ($P in $Case.LogMustHave) {
			if ($Text -notmatch $P) { Write-Warning ("{0} : expected log line NOT found: /{1}/ - the case may not measure what its name says (see the server log)" -f $Name, $P) }
		}
	}
	if ($Case.ContainsKey("LogMustNotHave")) {
		foreach ($P in $Case.LogMustNotHave) {
			if ($Text -match $P) { Write-Warning ("{0} : unexpected log line found: /{1}/ - see the server log" -f $Name, $P) }
		}
	}
	return $Result
}

# Kill a process AND its children. Packaged UE exes can be launcher stubs that spawn the real
# process (e.g. MassBubbleServer.exe -> MassBubbleServer-Win64-Test.exe); Stop-Process alone leaves the child alive.
function Stop-Tree($Proc) {
	if ($Proc -and -not $Proc.HasExited) {
		& taskkill /PID $Proc.Id /T /F 2>&1 | Out-Null
		$Proc.WaitForExit(15000) | Out-Null
	}
}

# CsvProfiler csv reader. The real header row can be at the END of the file (metadata [HasHeaderRowAtEnd]),
# so Import-Csv would take a placeholder line as header. Find the line that contains the "FrameTime" column.
function Read-ProfileCsv($Path) {
	$Lines = [IO.File]::ReadAllLines($Path)
	$H = -1
	for ($i = 0; $i -lt $Lines.Count; $i++) {
		if (($Lines[$i] -split ',') -contains 'FrameTime') { $H = $i; break }
	}
	if ($H -lt 0) { Write-Warning "no header row with FrameTime in $Path"; return @() }

	$Header = $Lines[$H] -split ','
	if ($H -eq 0) { $Data = $Lines[1..($Lines.Count - 1)] }      # header first
	else          { $Data = $Lines[0..($H - 1)] }                  # header at the end
	$Data = $Data | Where-Object { $_ -ne '' -and -not $_.StartsWith('[') }

	$Rows = $Data | ConvertFrom-Csv -Header $Header
	# keep only real data rows (numeric FrameTime): drops placeholder / metadata lines
	return @($Rows | Where-Object { ($null -ne ($_.FrameTime -as [double])) })
}

# Last line of defense: anything still running that is named like the server / client exe.
function Stop-Leftovers {
	foreach ($Exe in @($ServerExe, $ClientExe)) {
		if ($Exe -ne "") {
			$Base = [IO.Path]::GetFileNameWithoutExtension($Exe)
			Get-Process -Name "$Base*" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
		}
	}
	Start-Sleep -Seconds 3   # let the OS release the UDP port (7777) before the next case
}

# Names as written by CsvProfiler (checked against a real capture): timing stats carry the thread, counters do not.
# FrameTime of a dedicated server is capped by the tick rate (~33.3 ms at 30 fps) and says nothing about cost:
# GT_ExclusiveSum = sum of all Exclusive/GameThread/* columns = game thread BUSY ms per frame (computed below).
$Columns = "GT_ExclusiveSum", "FrameTime",
           "OptCrowd/GameThread/Director", "OptCrowd/GameThread/LOD", "OptCrowd/GameThread/Move",
           "OptCrowd/GameThread/Snapshot", "OptCrowd/GameThread/Bubble",
           "OptCrowd/GameThread/SpawnSlice", "OptCrowd/GameThread/DespawnSlice",
           "Exclusive/GameThread/TickActors", "Exclusive/GameThread/ServerReplicateActors",
           "OptCrowd/Simulated", "OptCrowd/BubbleItems", "OptCrowd/BubbleDirty",
           "OptCrowd/Agents", "OptCrowd/ActiveRegions", "OptCrowd/StreamingBusyCells",
           "Exclusive/GameThread/GarbageCollection", "Exclusive/GameThread/WorldTickMisc", "OptCrowd/GCSpanMs",
           "OptCrowd/GameThread/Plan", "OptCrowd/PlansPending"

$Summary = @()

# Expand group names, decide how many rounds to run
$RunCases = @()
$EffRepeat = [Math]::Max($Repeat, 1)
foreach ($N in $Cases) {
	if ($Groups.ContainsKey($N)) {
		$RunCases += $Groups[$N].Cases
		$EffRepeat = [Math]::Max($EffRepeat, $Groups[$N].Repeat)
	}
	else { $RunCases += $N }
}
$RunCases = @($RunCases | Select-Object -Unique)
Write-Host ("{0} case(s) x {1} run(s) = {2} run(s)" -f $RunCases.Count, $EffRepeat, ($RunCases.Count * $EffRepeat)) -ForegroundColor Green

Stop-Leftovers   # clean start: no server / client from an earlier run

# Rounds are interleaved (A0, A10, par1, par0, A0, A10, ...) so slow drift (thermal, background load) is spread over all cases.
for ($Run = 1; $Run -le $EffRepeat; $Run++) {
foreach ($Name in $RunCases) {
	if (-not $Matrix.Contains($Name)) { Write-Warning "unknown case $Name"; continue }
	$Case = $Matrix[$Name]
	$CaseBots = if ($Case.ContainsKey("Bots")) { $Case.Bots } else { $Bots }
	$CaseWarmup  = if ($Case.ContainsKey("Warmup"))  { $Case.Warmup }  else { $Warmup }
	$CaseClients = if ($Case.ContainsKey("Clients")) { $Case.Clients } else { $Clients }
	$CaseDuration = if ($Case.ContainsKey("Duration")) { $Case.Duration } else { $Duration }
	$Frames = ($CaseWarmup + $CaseDuration) * $ServerFps
	$Tag = "{0}_b{1}_c{2}" -f $Name, $CaseBots, $CaseClients   # goes into every file name of this case
	if ($EffRepeat -gt 1) { $Tag += "_r$Run" }

	$Cmds = @($Case.Cmds) + @("csvprofile frames=$Frames")
	$ServerArgs = @($Map, "-log", "-abslog=$(Resolve-Path $Out)\$Tag.log", "-OptBots=$CaseBots",
	                "-csvCategories=OptCrowd,Basic", "-ExecCmds=`"$($Cmds -join ',')`"") + $Case.Args

	Write-Host "== $Name [run $Run/$EffRepeat] : bots=$CaseBots clients=$CaseClients frames=$Frames" -ForegroundColor Cyan
	Stop-Leftovers
	$Start = Get-Date
	$Server = $null
	$ClientProcs = @()
	$Csv = $null

	try {
		$Server = Start-Process -FilePath $ServerExe -ArgumentList $ServerArgs -PassThru

		# Clients (the bubble is created in PostLogin, so replication cost needs at least one)
		if ($ClientExe -ne "" -and $CaseClients -gt 0) {
			Start-Sleep -Seconds 10
			for ($i = 0; $i -lt $CaseClients; $i++) {
				$ClientProcs += Start-Process -FilePath $ClientExe -PassThru `
					-ArgumentList @("127.0.0.1", "-nullrhi", "-nosound", "-log", "-abslog=$(Resolve-Path $Out)\${Tag}_client$i.log")
				Start-Sleep -Seconds 4   # stagger the logins: joins are the most expensive moment of a run
			}
		}

		# Wait until the capture is COMPLETE. CsvProfiler keeps the rows in memory and writes the file when the
		# capture ends, so the csv stays at 0 bytes during the whole run. Done = size > 0 and unchanged for 10 s.
		$Deadline = $Start.AddSeconds($CaseWarmup + $CaseDuration + 180)
		$LastSize = -1
		$StableSince = Get-Date
		$Complete = $false
		while ((Get-Date) -lt $Deadline -and -not $Complete) {
			Start-Sleep -Seconds 5
			if ($Server.HasExited) { Write-Warning "$Name : server process exited" }
			$Csv = Get-ChildItem $CsvDir -Filter *.csv -ErrorAction SilentlyContinue |
				Where-Object { $_.LastWriteTime -gt $Start } | Sort-Object LastWriteTime | Select-Object -Last 1
			$Elapsed = ((Get-Date) - $Start).TotalSeconds
			if ($null -eq $Csv) { Write-Host ("   [{0,4:N0}s] waiting for csv ..." -f $Elapsed); continue }

			$Csv.Refresh()
			if ($Csv.Length -ne $LastSize) { $LastSize = $Csv.Length; $StableSince = Get-Date }
			$StableSec = ((Get-Date) - $StableSince).TotalSeconds
			if ($Csv.Length -gt 0) { Write-Host ("   [{0,4:N0}s] {1}: {2:N0} KB, unchanged for {3:N0}s" -f $Elapsed, $Csv.Name, ($Csv.Length / 1KB), $StableSec) }

			if ($Csv.Length -gt 0 -and $StableSec -ge 10) { $Complete = $true }
		}
		if (-not $Complete) { Write-Warning "$Name : capture did not finish in time (check the server log for CsvProfiler lines)" }
	}
	finally {
		# Always runs, also on Ctrl+C / errors: nothing of this case survives into the next one.
		foreach ($P in $ClientProcs) { Stop-Tree $P }
		Stop-Tree $Server
		Stop-Leftovers
	}

	$LogInfo = Test-CaseLog $Name $Case "$Out\$Tag.log"

	if ($null -eq $Csv) { Write-Warning "$Name : CSV not found in $CsvDir"; continue }

	# Case name in the file name: copy into $Out and also tag the original in $CsvDir
	Copy-Item $Csv.FullName "$Out\$Tag.csv" -Force
	try { Rename-Item $Csv.FullName "${Tag}_$($Csv.Name)" -ErrorAction Stop } catch { Write-Warning "could not rename original csv: $_" }

	$All = Read-ProfileCsv "$Out\$Tag.csv"
	if ($All.Count -eq 0) { Write-Warning "$Name : no data rows in csv"; continue }
	if ($All.Count -lt ($Frames * 0.9)) { Write-Warning "$Name : csv has only $($All.Count) of $Frames frames - capture was cut short (server crash / stall?). See $Out\$Tag.log" }
	$ColNames = @(($All[0].PSObject.Properties.Name) | Where-Object { $_ -like "OptCrowd*" -or $_ -eq "FrameTime" })
	Write-Host ("   csv rows: {0} | columns found: {1}" -f $All.Count, ($ColNames -join ", "))
	$ExCols = @($All[0].PSObject.Properties.Name | Where-Object { $_ -like 'Exclusive/GameThread/*' })
	foreach ($R in $All) {
		$Sum = 0.0
		foreach ($C in $ExCols) { $Sum += ($R.$C -as [double]) }
		Add-Member -InputObject $R -NotePropertyName 'GT_ExclusiveSum' -NotePropertyValue $Sum -Force
	}
	$Rows = $All | Select-Object -Skip ($CaseWarmup * $ServerFps)
	if ($Rows.Count -lt 100) { Write-Warning "$Name : only $($Rows.Count) rows after warm-up"; continue }

	$Line = [ordered]@{ Case = $Name; Run = $Run; Bots = $CaseBots; Clients = $CaseClients; Warmup = $CaseWarmup; Rows = $Rows.Count }
	foreach ($C in $Columns) {
		$s = Get-Stat $Rows $C
		$Line["$C avg"] = $s[0]
		$Line["$C p99"] = $s[1]
		$Line["$C max"] = $s[2]
	}

	# ---- extra metrics (GC / PCG cases; n/a when the column is not in the csv) ----
	$GcCol = 'Exclusive/GameThread/GarbageCollection'
	$HasGc = $Rows[0].PSObject.Properties.Name -contains $GcCol
	$Line["GTFrames_gt10ms"] = @($Rows | Where-Object { $_.GT_ExclusiveSum -gt 10 }).Count     # frames where the game thread was busy > 10 ms
	$Line["GTFrames_gt33ms"] = @($Rows | Where-Object { $_.GT_ExclusiveSum -gt 33.3 }).Count   # busy longer than the 30 fps tick: a real tick overrun
	$Line["GCFrames_gt1ms"]  = if ($HasGc) { @($Rows | Where-Object { ($_.$GcCol -as [double]) -gt 1 }).Count } else { "n/a" }
	$Line["GCCount_window"]  = Get-Delta $Rows "OptCrowd/GCCount"
	$Line["Agents T90 s"]    = Get-FillTime $All $ServerFps 0.90
	$Line["Agents T99 s"]    = Get-FillTime $All $ServerFps 0.99
	$Line["PlansOk end"]     = Get-LastValue $All "OptCrowd/PlansOk"
	$Line["PlansFailed end"] = Get-LastValue $All "OptCrowd/PlansFailed"
	$Line["PlanLatAvgMs end"] = Get-LastValue $All "OptCrowd/PlanLatencyAvgMs"
	$Line["PlanLatMaxMs end"] = Get-LastValue $All "OptCrowd/PlanLatencyMaxMs"
	$Line["HitchLog n"]      = $LogInfo.Hitches          # [Hitch] lines in the whole run (warm-up included)
	$Summary += [pscustomobject]$Line
}   # foreach case
}   # for run

if ($Summary.Count -gt 0) {
	$Summary | Export-Csv "$Out\summary.csv" -NoTypeInformation -Encoding UTF8
	$Summary | Format-Table Case, Run, Bots, Clients, "GT_ExclusiveSum avg", "GT_ExclusiveSum p99",
		"OptCrowd/GameThread/Move avg", "OptCrowd/GameThread/Bubble avg", "OptCrowd/GameThread/Snapshot avg",
		"GT_ExclusiveSum max", "OptCrowd/GameThread/SpawnSlice max", "OptCrowd/GameThread/DespawnSlice max",
		"OptCrowd/Simulated avg", "OptCrowd/BubbleDirty avg" -AutoSize

	if (@($Summary | Where-Object { $_.Case -like "GC_*" }).Count -gt 0) {
		Write-Host "== GC cases ==" -ForegroundColor Green
		$Summary | Where-Object { $_.Case -like "GC_*" } | Format-Table Case, Run, "GT_ExclusiveSum avg", "GT_ExclusiveSum p99", "GT_ExclusiveSum max",
			"GTFrames_gt10ms", "GTFrames_gt33ms", "GCFrames_gt1ms", "Exclusive/GameThread/GarbageCollection avg", "Exclusive/GameThread/GarbageCollection max",
			"GCCount_window", "HitchLog n" -AutoSize
	}
	if (@($Summary | Where-Object { $_.Case -match "^P[01]_" }).Count -gt 0) {
		Write-Host "== PCG cases ==" -ForegroundColor Green
		$Summary | Where-Object { $_.Case -match "^P[01]_" } | Format-Table Case, Run, "GT_ExclusiveSum avg", "GT_ExclusiveSum max", "OptCrowd/GameThread/Plan avg",
			"OptCrowd/GameThread/SpawnSlice max", "Agents T90 s", "Agents T99 s", "PlansOk end", "PlansFailed end", "PlanLatAvgMs end", "PlanLatMaxMs end" -AutoSize
	}
	Write-Host "saved: $Out\summary.csv"
}

# ---- repeated runs: mean / min / max / sd per case, then target-vs-baseline comparison ----
$AggMetrics = "GT_ExclusiveSum avg", "GT_ExclusiveSum p99", "OptCrowd/GameThread/Move avg",
              "OptCrowd/GameThread/Bubble avg", "Exclusive/GameThread/ServerReplicateActors avg",
              "GT_ExclusiveSum max", "GTFrames_gt10ms", "GTFrames_gt33ms",
              "Exclusive/GameThread/GarbageCollection avg", "Exclusive/GameThread/GarbageCollection max",
              "OptCrowd/GameThread/Plan avg", "Agents T90 s", "PlanLatAvgMs end"
$Agg = @()
foreach ($G in ($Summary | Group-Object Case)) {
	if ($G.Count -lt 2) { continue }
	$Row = [ordered]@{ Case = $G.Name; N = $G.Count }
	foreach ($M in $AggMetrics) {
		$V = @($G.Group | ForEach-Object { $_.$M -as [double] } | Where-Object { $null -ne $_ })
		if ($V.Count -eq 0) {   # keep every column in every row: Export-Csv takes its columns from the first row
			$Row["$M mean"] = "n/a"; $Row["$M min"] = "n/a"; $Row["$M max"] = "n/a"; $Row["$M sd"] = "n/a"
			continue
		}
		$Mean = ($V | Measure-Object -Average).Average
		$SqSum = 0.0
		foreach ($X in $V) { $SqSum += ($X - $Mean) * ($X - $Mean) }
		$Sd = if ($V.Count -gt 1) { [Math]::Sqrt($SqSum / ($V.Count - 1)) } else { 0 }
		$Row["$M mean"] = [Math]::Round($Mean, 3)
		$Row["$M min"]  = [Math]::Round(($V | Measure-Object -Minimum).Minimum, 3)
		$Row["$M max"]  = [Math]::Round(($V | Measure-Object -Maximum).Maximum, 3)
		$Row["$M sd"]   = [Math]::Round($Sd, 3)
	}
	$Agg += [pscustomobject]$Row
}

if ($Agg.Count -gt 0) {
	$Agg | Export-Csv "$Out\summary_agg.csv" -NoTypeInformation -Encoding UTF8
	Write-Host ""
	Write-Host "== repeated runs (game thread busy ms/frame) ==" -ForegroundColor Green
	$Agg | Format-Table Case, N,
		"GT_ExclusiveSum avg mean", "GT_ExclusiveSum avg min", "GT_ExclusiveSum avg max", "GT_ExclusiveSum avg sd",
		"OptCrowd/GameThread/Move avg mean", "OptCrowd/GameThread/Move avg sd",
		"OptCrowd/GameThread/Bubble avg mean" -AutoSize
	$Agg | Format-Table Case, N, "GT_ExclusiveSum max mean", "GTFrames_gt10ms mean", "Exclusive/GameThread/GarbageCollection avg mean",
		"Exclusive/GameThread/GarbageCollection max mean", "OptCrowd/GameThread/Plan avg mean", "Agents T90 s mean", "PlanLatAvgMs end mean" -AutoSize
	Write-Host "saved: $Out\summary_agg.csv"

	foreach ($Pair in $Baselines.GetEnumerator()) {
		$T = $Agg | Where-Object { $_.Case -eq $Pair.Key }
		$B = $Agg | Where-Object { $_.Case -eq $Pair.Value }
		if ($null -eq $T -or $null -eq $B) { continue }
		foreach ($M in $CompareMetrics) {
			$TM = $T."$M mean"; $BM = $B."$M mean"
			if ($null -eq $TM -or $null -eq $BM -or $TM -is [string] -or $BM -is [string]) { continue }   # metric not in this pair's csv
			$Delta = $T."$M mean" - $B."$M mean"
			$Apart = ($T."$M max" -lt $B."$M min") -or ($T."$M min" -gt $B."$M max")
			$Verdict = if ($Apart) { "run ranges do NOT overlap -> larger than the run-to-run spread" } else { "run ranges overlap -> not distinguishable from noise" }
			Write-Host ("{0} vs {1} | {2}: {3:+0.000;-0.000} ms (mean {4:N3} vs {5:N3}) | {6}" -f `
				$Pair.Key, $Pair.Value, $M, $Delta, $T."$M mean", $B."$M mean", $Verdict)
		}
	}
}