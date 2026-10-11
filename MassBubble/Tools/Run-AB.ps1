<#
 MassBubble A/B matrix runner (PROFILING.md section 3)

 e.g.  .\Run-AB.ps1 -ServerExe D:\Pkg\WindowsServer\MassBubbleServer.exe `
                   -ClientExe D:\Pkg\Windows\MassBubble.exe `
                   -CsvDir   D:\Pkg\WindowsServer\MassBubble\Saved\Profiling\CSV `
                   -Cases A0,A1,A2

 For each case: start server -> (csvprofile frames=N) -> wait for completion -> shut down -> exclude warmup from the CSV, then aggregate mean/p99.
 Output: <Out>\summary.csv, <Out>\<case>_b<bots>_c<clients>.csv / .log

 Repeating:  -Repeat 3                 Runs every selected case 3 times (suffix _r1 _r2 _r3 on file names)
             -Cases A10_A3b_x3         A0 + A10_legacy + A3b_ts0_par1 + A3b_ts0_par0, 3 times each (about 12 runs x 2.2 min = about 27 min)
 Repeated runs interleave the cases (A0,A10,par1,par0, A0,A10,... ) so that drift over time does not concentrate on a single case.
 Output: summary.csv (one row per run) + summary_agg.csv (mean/min/max/sd per case) + comparison results (console)

 GC / PCG cases (follow-up measurements to PROFILING.md 6.2)
   Incremental GC : -Cases GC_x3 | GC_full_x3 | GC_churn_x3     (GC_inc_on vs GC_inc_off, per-component breakdown, time limit, load with frequent cell swaps)
   PCG spawn plan : -Cases PCG_x3 | PCG_fill_x3 | PCG_burst_x3 -SpawnGraph /Game/PCG_MassAgentSpawner.PCG_MassAgentSpawner
   GC_* cases set the gc.* CVars via -ExecCmds and query the same CVars again so the values are written to the server log.
   If a value cannot be confirmed in the log, a warning is issued (LogMustHave). PCG cases also check for "spawn graph ready".
   OptCrowd/GCCount, PlansOk, PlanLatency*Ms columns exist only in builds with MassBubble-GC-PCG-csv-stats.patch applied (n/a otherwise).
#>
param(
	[Parameter(Mandatory)] [string]$ServerExe,
	[string]$ClientExe = "",                       # If empty, run without a client (A4~A7 need a client!)
	[Parameter(Mandatory)] [string]$CsvDir,        # Saved\Profiling\CSV, where the server writes CSVs
	[string]$Map = "/Game/Maps/L_MassBubbleWorld",
	[int]$Bots = 8,
	[int]$Clients = 1,
	[int]$Warmup = 30,                             # Seconds. Must be longer than the bot ramp (bot count x BotRampSec)
	[int]$Duration = 60,                           # Seconds. Measurement window
	[int]$ServerFps = 30,                          # Server tick rate (used to compute frames)
	[string[]]$Cases = @("A0"),
	[int]$Repeat = 1,                              # How many times to repeat each case (group cases use their own repeat count)
	[string]$Out = "ab_results",
	[string]$SpawnGraph = ""                       # Graph asset path for PCG cases (e.g. /Game/PCG_MassAgentSpawner.PCG_MassAgentSpawner). If empty, the DefaultGame.ini value is used
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path   # absolute: [IO.File] APIs do not follow the PowerShell location
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture

# Cmds: server -ExecCmds (runtime CVars) / Args: server command line (items that need a restart, ini overrides)
$GameIni = "-ini:Game:[/Script/MassBubble.CrowdSettings]"

# PCG enabling: bUsePCG is read at world start, so pass it via the server command line (ini override), not a CVar.
$PcgArgs = @("${GameIni}:bUsePCG=True")
if ($SpawnGraph -ne "") { $PcgArgs += "${GameIni}:SpawnGraph=$SpawnGraph" }
$PcgHave    = @('\[PCG_MassAgentSpawner\] spawn graph ready', '\[PCG_MassAgentSpawner\] first plan arrived')
$PcgNotHave = @('did not finish within', 'run failed or was cancelled', 'could not be loaded', 'could not be requested', 'no Spawn Graph is set')

# Incremental GC cases. Reach / Gather / BeginDestroy are 0 or 1.
#   gc.AllowIncrementalReachability, gc.AllowIncrementalGather, gc.IncrementalBeginDestroyEnabled (+ time-limit CVar)
# Running just the CVar name without a value prints its current value to the log. The script finds that line to verify the setting was actually applied.
# opt.hitch.LogMs 40: the server frame is fixed at 33.3 ms, so a [Hitch] line is logged only for frames at or above that threshold.
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
		Duration = 90      # GCGC runs every 5-9 seconds, so a 60-second window gives few samples
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

	# ---- Incremental GC (engine gc.* CVars. The project defaults are all enabled in DefaultEngine.ini) ----
	# GC_inc_onis a baseline with "on" explicitly set again: it does not rely on the ini values, so cases are compared under the same conditions.
	"GC_inc_on"     = (New-GcCase 1 1 1)
	"GC_inc_off"    = (New-GcCase 0 0 0)                 # Turn off incremental reachability analysis + incremental gather + incremental BeginDestroy (all)
	"GC_off_reach"  = (New-GcCase 0 1 1)                 # Breakdown: turn off only incremental reachability analysis
	"GC_off_gather" = (New-GcCase 1 0 1)                 # Breakdown: turn off only incremental gather
	"GC_off_bd"     = (New-GcCase 1 1 0)                 # Breakdown: turn off only incremental BeginDestroy
	"GC_tl_05ms"    = (New-GcCase 1 1 1 0.0005)          # Time limit 0.5 ms (default 2 ms)
	"GC_tl_8ms"     = (New-GcCase 1 1 1 0.008)           # Time limit 8 ms
	# Load with frequent cell load/unload: run the bots quickly to increase the number of GC runs
	"GC_churn_on"   = (New-GcCase 1 1 1 $null @{ Bots = 16; Args = @("${GameIni}:BotSpeedCmPerSec=4000") })
	"GC_churn_off"  = (New-GcCase 0 0 0 $null @{ Bots = 16; Args = @("${GameIni}:BotSpeedCmPerSec=4000") })

	# ---- PCG spawn plan (PCG_MassAgentSpawnerPlanner). P0 = random placement (no planner), P1 = PCG graph ----
	"P0_random"     = @{ Cmds = @(); Args = @("${GameIni}:bUsePCG=False"); LogMustNotHave = @('\\[PCG_MassAgentSpawner\\] loading spawn graph') }
	"P1_pcg"        = @{ Cmds = @(); Args = $PcgArgs;                                              LogMustHave = $PcgHave; LogMustNotHave = $PcgNotHave }
	"P1_pcg_c1"     = @{ Cmds = @(); Args = ($PcgArgs + @("${GameIni}:PCGMaxConcurrent=1"));       LogMustHave = $PcgHave; LogMustNotHave = $PcgNotHave }
	"P1_pcg_c8"     = @{ Cmds = @(); Args = ($PcgArgs + @("${GameIni}:PCGMaxConcurrent=8"));       LogMustHave = $PcgHave; LogMustNotHave = $PcgNotHave }
	# Warmup 0: look at the startup phase (time for agents to fill in, Agents T90/T99). Filling with the ramped 16 bots takes about 42 seconds
	"P0_random_w0"  = @{ Cmds = @(); Args = @("${GameIni}:bUsePCG=False"); Bots = 16; Warmup = 0; Duration = 90; LogMustNotHave = @('\\[PCG_MassAgentSpawner\\] loading spawn graph') }
	"P1_pcg_w0"     = @{ Cmds = @(); Args = $PcgArgs;                                              Bots = 16; Warmup = 0; Duration = 90; LogMustHave = $PcgHave; LogMustNotHave = $PcgNotHave }
	"P1_pcg_c1_w0"  = @{ Cmds = @(); Args = ($PcgArgs + @("${GameIni}:PCGMaxConcurrent=1"));       Bots = 16; Warmup = 0; Duration = 90; LogMustHave = $PcgHave; LogMustNotHave = $PcgNotHave }
	"P1_pcg_c8_w0"  = @{ Cmds = @(); Args = ($PcgArgs + @("${GameIni}:PCGMaxConcurrent=8"));       Bots = 16; Warmup = 0; Duration = 90; LogMustHave = $PcgHave; LogMustNotHave = $PcgNotHave }
	# All 16 bots at once: planner queue and spawn latency when requests pile up at the same time
	"P0_random_burst_w0" = @{ Cmds = @("opt.crowd.BotRampSec 0"); Args = @("${GameIni}:bUsePCG=False"); Bots = 16; Warmup = 0; Duration = 90; LogMustNotHave = @('\\[PCG_MassAgentSpawner\\] loading spawn graph') }
	"P1_pcg_burst_w0"    = @{ Cmds = @("opt.crowd.BotRampSec 0"); Args = $PcgArgs;                      Bots = 16; Warmup = 0; Duration = 90; LogMustHave = $PcgHave; LogMustNotHave = $PcgNotHave }
}

# Group names expand into several cases plus a repeat count:  .\Run-AB.ps1 ... -Cases A10_A3b_x3
# A0 is part of the group because A10_legacy is only meaningful against a baseline measured the same number of times.
$Groups = @{
	"A10_A3b_x3" = @{ Cases = @("A0", "A10_legacy", "A3b_ts0_par1", "A3b_ts0_par0"); Repeat = 3 }
	# Incremental GC. Approximate run time: x3 = 2 x 3 x 3.3 min = about 20 min, full_x3 = 7 x 3 x 3.3 min = about 70 min
	"GC_x3"       = @{ Cases = @("GC_inc_on", "GC_inc_off"); Repeat = 3 }
	"GC_full_x3"  = @{ Cases = @("GC_inc_on", "GC_inc_off", "GC_off_reach", "GC_off_gather", "GC_off_bd", "GC_tl_05ms", "GC_tl_8ms"); Repeat = 3 }
	"GC_churn_x3" = @{ Cases = @("GC_churn_on", "GC_churn_off"); Repeat = 3 }
	# PCG (-SpawnGraph required). fill = startup phase, burst = bots injected all at once
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

# Metrics compared after repeated measurements (the "<metric> mean/min/max" columns of Agg). Skipped for pairs that lack those columns.
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

# Time (seconds) at which the agent count first reached Frac of its final value (the maximum during capture). Meaningful for warmup-0 cases.
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

# Value of the last row of a cumulative counter (PlansOk, PlanLatencyAvgMs ...)
function Get-LastValue($Rows, $Col) {
	if (-not ($Rows[0].PSObject.Properties.Name -contains $Col)) { return "n/a" }
	$V = $Rows[$Rows.Count - 1].$Col -as [double]
	if ($null -eq $V) { return "n/a" }
	return [Math]::Round($V, 3)
}

# Increase of a cumulative counter over the measurement window (last row - first row). E.g. how many times GC ran within the window
function Get-Delta($Rows, $Col) {
	if (-not ($Rows[0].PSObject.Properties.Name -contains $Col)) { return "n/a" }
	$First = $Rows[0].$Col -as [double]
	$Last = $Rows[$Rows.Count - 1].$Col -as [double]
	if ($null -eq $First -or $null -eq $Last) { return "n/a" }
	return [Math]::Round($Last - $First, 3)
}

# Server log check: verify the case ran under the conditions its name implies (CVars applied, PCG graph loaded) and count [Hitch] lines.
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