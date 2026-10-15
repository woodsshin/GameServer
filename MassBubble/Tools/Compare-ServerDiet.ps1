<#
.SYNOPSIS
	Before / after of a server module / plugin diet: modules, plugins, Module.* milestones, ProcessMB, and what the logs say.

.DESCRIPTION
	-Before and -After are the directories Run-ServerDiet.ps1 wrote (runN.json, runN.log) or single report files.
	Several runs per side are reduced to their median. It prints
	  1. the headline numbers (modules, plugins, Engine.InitComplete, ProcessMB at the hook / after init / settled, UObjects, ...)
	  2. which modules and plugins disappeared (or appeared) and what the removed ones cost in the BEFORE run
	  3. the startup milestones (Module.Phase.*, Engine.InitComplete) and the Module.<Name>.Loaded milestones that moved most
	  4. warnings / errors that exist only in the AFTER logs: the usual sign that cooked content or an .ini setting still
	     needs a plugin that was switched off

.EXAMPLE
	.\Compare-ServerDiet.ps1 -Before .\DietRuns\before -After .\DietRuns\after
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$Before,
	[Parameter(Mandatory = $true)][string]$After,
	[int]$Top = 15,
	[switch]$NoLogs
)

. "$PSScriptRoot\DietCommon.ps1"

$b = @(Read-DietReports $Before)
$a = @(Read-DietReports $After)

Write-Host ''
Write-Host "== Server diet: before = $($b.Count) run(s) [$Before]"
Write-Host "               after  = $($a.Count) run(s) [$After]"
if ($b.Count -lt 3 -or $a.Count -lt 3) { Write-Warning 'Fewer than 3 runs on a side: the medians are not worth much. Run-ServerDiet.ps1 -Runs 5.' }

# ---------------------------------------------------------------------------------------------------------------------
# 1. Headline numbers
# ---------------------------------------------------------------------------------------------------------------------
$metricsB = @($b | ForEach-Object { Get-DietMetrics $_ })
$metricsA = @($a | ForEach-Object { Get-DietMetrics $_ })

$summary = @()
$delta = @{}
foreach ($key in $metricsB[0].Keys) {
	$vb = Get-Median @($metricsB | ForEach-Object { $_[$key] })
	$va = Get-Median @($metricsA | ForEach-Object { $_[$key] })
	$d = $null; $pct = $null
	if ($null -ne $vb -and $null -ne $va) {
		$d = $va - $vb
		if ($vb -ne 0) { $pct = 100.0 * $d / $vb }
	}
	$delta[$key] = [pscustomobject]@{ before = $vb; after = $va; delta = $d; pct = $pct }
	$isCount = $key -in @('modules loaded', 'plugins enabled', 'plugins with code loaded', 'Module.*.Loaded events seen', 'UObjects', 'UClasses')
	$fmt = if ($isCount -or $key -like '*(ms*') { 'N0' } else { 'N1' }
	$summary += [pscustomobject]@{
		Metric = $key; Before = (Format-Num $vb $fmt); After = (Format-Num $va $fmt)
		Delta = (Format-Signed $d $fmt); 'Delta %' = $(if ($null -ne $pct) { (Format-Signed $pct 'N1') + '%' } else { '-' })
	}
}
Write-Host ''
Write-Host 'Headline numbers (median of the runs)'
Write-Table $summary

# ---------------------------------------------------------------------------------------------------------------------
# 2. Modules and plugins that came or went
# ---------------------------------------------------------------------------------------------------------------------
function Get-Presence {
	# name -> in how many runs
	param([object[]]$Reports, [scriptblock]$Names)
	$count = @{}
	foreach ($r in $Reports) {
		$seen = @{}
		foreach ($n in @(& $Names $r)) { $seen[[string]$n] = $true }
		foreach ($k in $seen.Keys) { if ($count.ContainsKey($k)) { $count[$k]++ } else { $count[$k] = 1 } }
	}
	return $count
}
function Test-Present { param($Map, [string]$Name, [int]$Runs) return ($Map.ContainsKey($Name) -and ($Map[$Name] * 2 -ge $Runs)) }

$moduleNames = { param($r) @($r.modules) | ForEach-Object { $_.name } }
$pluginNames = { param($r) @($r.plugins) | ForEach-Object { $_.name } }
$modB = Get-Presence $b $moduleNames; $modA = Get-Presence $a $moduleNames
$plgB = Get-Presence $b $pluginNames; $plgA = Get-Presence $a $pluginNames

$ownerOf = @{}
foreach ($r in @($b + $a)) { foreach ($m in @($r.modules)) { if ([string]$m.plugin -ne '') { $ownerOf[[string]$m.name] = [string]$m.plugin } } }

$modulesGone = @($modB.Keys | Where-Object { (Test-Present $modB $_ $b.Count) -and -not (Test-Present $modA $_ $a.Count) } | Sort-Object)
$modulesNew = @($modA.Keys | Where-Object { (Test-Present $modA $_ $a.Count) -and -not (Test-Present $modB $_ $b.Count) } | Sort-Object)
$pluginsGone = @($plgB.Keys | Where-Object { (Test-Present $plgB $_ $b.Count) -and -not (Test-Present $plgA $_ $a.Count) } | Sort-Object)
$pluginsNew = @($plgA.Keys | Where-Object { (Test-Present $plgA $_ $a.Count) -and -not (Test-Present $plgB $_ $b.Count) } | Sort-Object)

function Format-NameList {
	param([string[]]$Names, [hashtable]$Owner)
	if ($Names.Count -eq 0) { return '   (none)' }
	return '   ' + (($Names | ForEach-Object { if ($Owner -and $Owner.ContainsKey($_)) { "$_ [$($Owner[$_])]" } else { $_ } }) -join ', ')
}

Write-Host ''
Write-Host "Plugins only in BEFORE ($($pluginsGone.Count)):"; Write-Host (Format-NameList $pluginsGone $null)
Write-Host "Plugins only in AFTER ($($pluginsNew.Count)):";   Write-Host (Format-NameList $pluginsNew $null)
Write-Host "Modules only in BEFORE ($($modulesGone.Count)), [owner plugin]:"; Write-Host (Format-NameList $modulesGone $ownerOf)
Write-Host "Modules only in AFTER ($($modulesNew.Count)):"; Write-Host (Format-NameList $modulesNew $ownerOf)

if ($pluginsGone.Count -gt 0) {
	$costs = Get-ModuleCosts $b
	$modulesOfPlugin = @{}
	foreach ($p in @($b[0].plugins)) { $modulesOfPlugin[[string]$p.name] = @(@($p.modules) | ForEach-Object { [string]$_.name }) }

	$rows = @()
	$sumMb = 0.0; $sumMs = 0.0
	foreach ($name in $pluginsGone) {
		$mb = 0.0; $ms = 0.0; $known = 0
		foreach ($mn in @($modulesOfPlugin[$name])) { if ($costs.ContainsKey($mn)) { $mb += [double]$costs[$mn].mb; $ms += [double]$costs[$mn].ms; $known++ } }
		$sumMb += $mb; $sumMs += $ms
		$pl = @($b[0].plugins | Where-Object { $_.name -eq $name })[0]
		$rows += [pscustomobject]@{ Plugin = $name; 'Modules loaded' = "$($pl.modules_loaded)/$($pl.modules_declared)"; 'Live objects' = $pl.instances; 'Content objects' = $pl.content_objects; '~MB at load' = (Format-Num $mb); '~ms at load' = (Format-Num $ms 'N0'); _mb = $mb }
	}
	Write-Host ''
	Write-Host 'What the removed plugins cost in the BEFORE run (module-load samples: rough, anything that ran in between is charged to the module)'
	Write-Table ($rows | Sort-Object _mb -Descending | Select-Object Plugin, 'Modules loaded', 'Live objects', 'Content objects', '~MB at load', '~ms at load')
	Write-Host ("   together: about {0} MB, {1} ms" -f (Format-Num $sumMb), (Format-Num $sumMs 'N0'))
	Write-Host '   (compare with the measured delta of ProcessMB above: the difference is what the samples cannot see, e.g. UClass registration of modules that were linked but never started)'
}

# ---------------------------------------------------------------------------------------------------------------------
# 3. Milestones
# ---------------------------------------------------------------------------------------------------------------------
function Get-MilestoneMedians {
	param([object[]]$Reports)
	$values = @{}
	$order = @{}
	foreach ($r in $Reports) {
		$i = 0
		foreach ($m in @($r.milestones)) {
			$i++
			if (-not $values.ContainsKey($m.name)) { $values[$m.name] = @(); $order[$m.name] = $i }
			$values[$m.name] += [double]$m.ms_since_process
		}
	}
	$result = @{}
	foreach ($k in $values.Keys) { $result[$k] = [pscustomobject]@{ ms = (Get-Median $values[$k]); order = $order[$k] } }
	return $result
}
$msB = Get-MilestoneMedians $b
$msA = Get-MilestoneMedians $a

function New-MilestoneRow {
	param([string]$Name)
	$vb = if ($msB.ContainsKey($Name)) { $msB[$Name].ms } else { $null }
	$va = if ($msA.ContainsKey($Name)) { $msA[$Name].ms } else { $null }
	$d = if ($null -ne $vb -and $null -ne $va) { $va - $vb } else { $null }
	[pscustomobject]@{ Milestone = $Name; 'Before (ms)' = (Format-Num $vb 'N0'); 'After (ms)' = (Format-Num $va 'N0'); Delta = (Format-Signed $d 'N0'); _d = $d; _o = $(if ($null -ne $vb) { $msB[$Name].order } else { $msA[$Name].order }) }
}

$phaseNames = @('Module.MassBubble.Startup') + @($msB.Keys + $msA.Keys | Where-Object { $_ -like 'Module.Phase.*' } | Sort-Object -Unique) + @('Engine.InitComplete', 'Diet.ReportDue')
$phaseRows = @($phaseNames | Select-Object -Unique | Where-Object { $msB.ContainsKey($_) -or $msA.ContainsKey($_) } | ForEach-Object { New-MilestoneRow $_ } | Sort-Object _o)
Write-Host ''
Write-Host 'Startup milestones (ms since process start, median)'
Write-Table ($phaseRows | Select-Object Milestone, 'Before (ms)', 'After (ms)', Delta)

$common = @($msB.Keys | Where-Object { $_ -like 'Module.*.Loaded' -and $msA.ContainsKey($_) })
$moved = @($common | ForEach-Object { New-MilestoneRow $_ } | Sort-Object { [math]::Abs([double]$_._d) } -Descending | Select-Object -First $Top)
Write-Host ''
Write-Host "Module.<Name>.Loaded milestones that moved most (top $Top of $($common.Count) in both; cumulative: everything after a removed module moves up too)"
Write-Table ($moved | Select-Object Milestone, 'Before (ms)', 'After (ms)', Delta)

# ---------------------------------------------------------------------------------------------------------------------
# 4. Logs
# ---------------------------------------------------------------------------------------------------------------------
function Get-LogIssueSets {
	# one hashtable per run: normalised warning / error line -> times
	param([string]$Path)
	$dir = if (Test-Path -LiteralPath $Path -PathType Container) { $Path } else { Split-Path -Parent (Resolve-Path -LiteralPath $Path).Path }
	$sets = @()
	foreach ($f in @(Get-ChildItem -LiteralPath $dir -Filter 'run*.log' | Where-Object { $_.Name -notlike '*.err.log' } | Sort-Object Name)) {
		$set = @{}
		foreach ($line in [System.IO.File]::ReadLines($f.FullName)) {
			if ($line -notmatch ':\s(Warning|Error|Fatal):\s') { continue }
			$n = [regex]::Replace($line, '^\[\d{4}\.\d\d\.\d\d-\d\d\.\d\d\.\d\d:\d{3}\]\[\s*\d+\]', '')
			$n = [regex]::Replace($n, '0x[0-9a-fA-F]+', '0x#')
			$n = [regex]::Replace($n, '\d+', '#').Trim()
			if ($set.ContainsKey($n)) { $set[$n]++ } else { $set[$n] = 1 }
		}
		$sets += $set
	}
	return $sets   # callers wrap the call in @(): a lone hashtable or nothing at all still becomes an array
}

if (-not $NoLogs) {
	$setsB = @(Get-LogIssueSets $Before)
	$setsA = @(Get-LogIssueSets $After)
	Write-Host ''
	if ($setsB.Count -eq 0 -or $setsA.Count -eq 0) {
		Write-Host 'Logs: none found next to the reports (runN.log), skipped.'
	}
	else {
		$inRuns = {
			param($Sets, [string]$Line)
			$n = 0
			foreach ($s in $Sets) { if ($s.ContainsKey($Line)) { $n++ } }
			return $n
		}
		$allA = @{}; foreach ($s in $setsA) { foreach ($k in $s.Keys) { $allA[$k] = $true } }
		$allB = @{}; foreach ($s in $setsB) { foreach ($k in $s.Keys) { $allB[$k] = $true } }

		$newInAfter = @($allA.Keys | Where-Object { (& $inRuns $setsA $_) * 2 -ge $setsA.Count -and (& $inRuns $setsB $_) -eq 0 } | Sort-Object)
		$goneFromBefore = @($allB.Keys | Where-Object { (& $inRuns $setsB $_) * 2 -ge $setsB.Count -and (& $inRuns $setsA $_) -eq 0 })

		Write-Host "Warnings / errors only in the AFTER logs: $($newInAfter.Count)   (only in BEFORE: $($goneFromBefore.Count))"
		if ($newInAfter.Count -gt 0) {
			Write-Host '   A plugin that cooked content or an .ini setting still needs usually shows up here (missing class, failed load, unknown settings).'
			foreach ($line in ($newInAfter | Select-Object -First 40)) {
				$text = if ($line.Length -gt 220) { $line.Substring(0, 220) + ' ...' } else { $line }
				Write-Host "   ! $text"
			}
			if ($newInAfter.Count -gt 40) { Write-Host "   ... and $($newInAfter.Count - 40) more" }
		}
	}
}

# ---------------------------------------------------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------------------------------------------------
function Format-Verdict {
	param([string]$Label, [string]$Key, [string]$Unit, [string]$Format = 'N0')
	$d = $delta[$Key]
	if ($null -eq $d -or $null -eq $d.delta) { return $null }
	$pct = if ($null -ne $d.pct) { ' (' + (Format-Signed $d.pct 'N1') + '%)' } else { '' }
	return "$Label $(Format-Signed $d.delta $Format) $Unit$pct"
}
$verdict = @(
	(Format-Verdict 'modules' 'modules loaded' ''),
	(Format-Verdict 'plugins' 'plugins enabled' ''),
	(Format-Verdict 'Engine.InitComplete' 'Engine.InitComplete (ms since process)' 'ms'),
	(Format-Verdict 'ProcessMB after init' 'ProcessMB at Engine.InitComplete' 'MB' 'N1'),
	(Format-Verdict 'ProcessMB settled' 'ProcessMB at report (settled)' 'MB' 'N1'),
	(Format-Verdict 'executable' 'executable (MB)' 'MB' 'N1')
) | Where-Object { $_ }
Write-Host ''
Write-Host ('Result: ' + ($verdict -join ' | '))
