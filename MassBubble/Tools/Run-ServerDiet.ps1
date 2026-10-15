<#
.SYNOPSIS
	Starts a dedicated server several times and collects its module / plugin report (-OptDietReport) and its log.

.DESCRIPTION
	Every run: start the server, wait until it has settled (-SettleSec after Engine.InitComplete), let it write
	<OutDir>\<Label>\runN.json and quit by itself (-OptDietQuit). The first -Warmup runs are discarded: a freshly built
	executable is read from a cold disk cache and scanned by the virus checker.

	Run it once for the build as it is (Label 'before'), take the plugin list from Get-DietCandidates.ps1, rebuild with the
	plugins removed and run it again (Label 'after'), then compare with Compare-ServerDiet.ps1.

.EXAMPLE
	.\Run-ServerDiet.ps1 -Exe D:\Staged\before\WindowsServer\MassBubbleServer.exe -Label before -Runs 5 -ExtraArgs '-OptBots=8'

.EXAMPLE
	# A Shipping server prints no console commands, but the command line options and the report work there too.
	.\Run-ServerDiet.ps1 -Exe D:\Staged\after\WindowsServer\MassBubbleServer.exe -Label after -Runs 5 -ExtraArgs '-OptBots=8'

.NOTES
	Use the same -Map / -ExtraArgs for both labels, or the memory figures are not comparable.
	Staging a server (adapt the paths):
		RunUAT.bat BuildCookRun -project="<path>\MassBubble.uproject" -platform=Win64 -server -noclient -serverconfig=Development
			-build -cook -stage -pak -archive -archivedirectory="D:\Staged\before"
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$Exe,
	[Parameter(Mandatory = $true)][string]$Label,
	[string]$OutDir = (Join-Path (Get-Location) 'DietRuns'),
	[int]$Runs = 5,
	[int]$Warmup = 1,
	[double]$SettleSec = 15,
	[string]$Map = '/Game/Maps/L_MassBubbleWorld',
	[string[]]$ExtraArgs = @(),
	[int]$BasePort = 17777,
	[int]$TimeoutSec = 0
)

. "$PSScriptRoot\DietCommon.ps1"

if (-not (Test-Path -LiteralPath $Exe)) { throw "Server executable not found: $Exe" }
$Exe = (Resolve-Path -LiteralPath $Exe).Path
if ($TimeoutSec -le 0) { $TimeoutSec = [int]($SettleSec + 240) }

$dir = Join-Path $OutDir $Label
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$dir = (Resolve-Path -LiteralPath $dir).Path
$invariant = [System.Globalization.CultureInfo]::InvariantCulture

function ConvertTo-ArgString {
	param([string[]]$Items)
	($Items | ForEach-Object {
		if ($_ -match '^-\w+="') { $_ }                                  # already quoted: -OptDietOut="C:\a b\x.json"
		elseif ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' }
		else { $_ }
	}) -join ' '
}

function Invoke-DietRun {
	param([string]$Name, [int]$Port)

	$json = Join-Path $dir "$Name.json"
	$log = Join-Path $dir "$Name.log"
	$err = Join-Path $dir "$Name.err.log"
	Remove-Item -LiteralPath $json, $log, $err -ErrorAction SilentlyContinue

	$serverArgs = @(
		$Map, '-log', '-stdout', '-FullStdOutLogOutput', '-unattended', '-nosplash',
		"-port=$Port",
		"-OptDietReport=$Label", "-OptDietOut=`"$json`"", ('-OptDietSettleSec=' + $SettleSec.ToString($invariant)), '-OptDietQuit'
	) + @($ExtraArgs)

	$sw = [System.Diagnostics.Stopwatch]::StartNew()
	$p = Start-Process -FilePath $Exe -ArgumentList (ConvertTo-ArgString $serverArgs) -WorkingDirectory (Split-Path -Parent $Exe) `
		-RedirectStandardOutput $log -RedirectStandardError $err -NoNewWindow -PassThru
	$null = $p.Handle   # keeps the exit code readable after the process ended

	$timedOut = $false
	if (-not $p.WaitForExit($TimeoutSec * 1000)) {
		$timedOut = $true
		try { $p.Kill($true) } catch { try { $p.Kill() } catch { } }   # the whole tree where the runtime can (PowerShell 7)
	}
	$p.WaitForExit()
	$sw.Stop()

	$row = [ordered]@{ run = $Name; exit = $p.ExitCode; wall_s = [math]::Round($sw.Elapsed.TotalSeconds, 1); modules = $null; init_ms = $null; process_mb = $null; note = '' }
	if ($timedOut) { $row.note = "timed out after $TimeoutSec s (killed)" }

	if (Test-Path -LiteralPath $json) {
		try {
			$report = Get-Content -LiteralPath $json -Raw -Encoding UTF8 | ConvertFrom-Json
			$metrics = Get-DietMetrics $report
			$row.modules = $metrics['modules loaded']
			$row.init_ms = Format-Num $metrics['Engine.InitComplete (ms since process)'] 'N0'
			$row.process_mb = Format-Num $metrics['ProcessMB at report (settled)'] 'N1'
		}
		catch { $row.note = "report unreadable: $($_.Exception.Message)" }
	}
	else {
		if ($row.note -eq '') { $row.note = 'no report written - see ' + $log }
		if (Test-Path -LiteralPath $log) {
			Write-Warning "$Name wrote no report. Last lines of the log:"
			Get-Content -LiteralPath $log -Tail 15 | ForEach-Object { Write-Host "    $_" }
		}
	}
	return [pscustomobject]$row
}

$rows = @()
$port = $BasePort
for ($i = 1; $i -le $Warmup; $i++) { Write-Host "warm-up $i/$Warmup (discarded) ..."; $rows += Invoke-DietRun -Name "warmup$i" -Port $port; $port++ }
for ($i = 1; $i -le $Runs; $i++) { Write-Host "run $i/$Runs ..."; $rows += Invoke-DietRun -Name "run$i" -Port $port; $port++ }

Write-Host ''
Write-Host "== $Label =="
Write-Table $rows
Write-Host "reports and logs: $dir"

$failed = @($rows | Where-Object { $null -eq $_.modules -and $_.run -like 'run*' })
if ($failed.Count -gt 0) { Write-Warning "$($failed.Count) of $Runs runs produced no report." }
