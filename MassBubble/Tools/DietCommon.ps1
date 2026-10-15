# Shared helpers of the server diet tools. Dot-source it: . "$PSScriptRoot\DietCommon.ps1"
# Works in Windows PowerShell 5.1 and PowerShell 7.

Set-StrictMode -Version 2.0

function Get-Median {
	param([object[]]$Values)
	$v = @($Values | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ } | Sort-Object)
	if ($v.Count -eq 0) { return $null }
	$mid = [int][math]::Floor($v.Count / 2)
	if ($v.Count % 2 -eq 1) { return $v[$mid] }
	return ($v[$mid - 1] + $v[$mid]) / 2.0
}

function Get-Prop {
	# Reads a property that may be missing (StrictMode would throw).
	param($Object, [string]$Name, $Default = $null)
	if ($null -ne $Object -and $Object.PSObject.Properties[$Name]) { return $Object.$Name }
	return $Default
}

function Read-DietReports {
	# A directory written by Run-ServerDiet.ps1 (run*.json) or a single report file.
	param([Parameter(Mandatory = $true)][string]$Path)
	if (-not (Test-Path -LiteralPath $Path)) { throw "Not found: $Path" }
	$item = Get-Item -LiteralPath $Path
	if ($item.PSIsContainer) {
		$files = @(Get-ChildItem -LiteralPath $Path -Filter 'run*.json' | Sort-Object Name)
		if ($files.Count -eq 0) { $files = @(Get-ChildItem -LiteralPath $Path -Filter '*.json' | Where-Object { $_.Name -notlike 'warmup*' } | Sort-Object Name) }
	}
	else {
		$files = @($item)
	}
	if ($files.Count -eq 0) { throw "No report (*.json) in $Path" }

	$reports = @()
	foreach ($f in $files) {
		$report = Get-Content -LiteralPath $f.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
		$report | Add-Member -NotePropertyName '_file' -NotePropertyValue $f.FullName -Force
		$reports += $report
	}
	return $reports
}

function Get-MilestoneMs {
	param($Report, [string]$Name)
	foreach ($m in @($Report.milestones)) { if ($m.name -eq $Name) { return [double]$m.ms_since_process } }
	return $null
}

function Get-SampleMb {
	param($Report, [string]$At)
	foreach ($s in @($Report.memory_samples)) { if ($s.at -eq $At) { return [double]$s.process_mb } }
	return $null
}

function Get-DietMetrics {
	# One number per metric for one report. $null = not recorded.
	param($Report)
	$mods = @($Report.modules)
	$plugins = @($Report.plugins)
	$loads = @(@($Report.milestones) | Where-Object { $_.name -like 'Module.*.Loaded' })

	$span = $null
	if ($loads.Count -ge 2) { $span = [double]$loads[$loads.Count - 1].ms_since_process - [double]$loads[0].ms_since_process }

	$fileMb = 0.0
	foreach ($mod in $mods) { $fileMb += [double](Get-Prop $mod 'file_mb' 0) }

	$result = [ordered]@{}
	$result['modules loaded'] = $mods.Count
	$result['plugins enabled'] = $plugins.Count
	$result['plugins with code loaded'] = @($plugins | Where-Object { [int]$_.modules_loaded -gt 0 }).Count
	$result['Module.*.Loaded events seen'] = $loads.Count
	$result['module load span (ms)'] = $span
	$result['Module.Phase.Default.Done (ms since process)'] = Get-MilestoneMs $Report 'Module.Phase.Default.Done'
	$result['Engine.InitComplete (ms since process)'] = Get-MilestoneMs $Report 'Engine.InitComplete'
	$result['ProcessMB at hook'] = Get-SampleMb $Report 'Diet.HookInstalled'
	$result['ProcessMB at Engine.InitComplete'] = Get-SampleMb $Report 'Engine.InitComplete'
	$result['ProcessMB at report (settled)'] = [double]$Report.memory.process_mb
	$result['peak physical (MB)'] = [double]$Report.memory.peak_physical_mb
	$result['virtual (MB)'] = [double]$Report.memory.virtual_mb
	$result['UObjects'] = [int]$Report.objects.total
	$result['UClasses'] = [int]$Report.objects.uclasses
	$result['executable (MB)'] = [double]$Report.executable_mb
	$result['loaded module files (MB)'] = $fileMb
	return $result
}

function Get-ModuleCosts {
	# What each module cost while it loaded: the growth of the process and the time between the previous load event and
	# its own, median over the runs. Rough (anything else that ran in between is charged to the module), but the order of
	# magnitude is what decides which plugin is worth removing. Only modules loaded after the hook have a figure.
	param([object[]]$Reports)

	$ms = @{}
	$mb = @{}
	foreach ($r in $Reports) {
		$prevMs = $null
		$prevMb = $null
		$hook = @(@($r.memory_samples) | Where-Object { $_.at -eq 'Diet.HookInstalled' })
		if ($hook.Count -gt 0) { $prevMs = [double]$hook[0].ms_since_process; $prevMb = [double]$hook[0].process_mb }

		$loaded = @(@($r.modules) | Where-Object { [double]$_.loaded_ms -ge 0 } | Sort-Object { [double]$_.loaded_ms })
		foreach ($m in $loaded) {
			if ($null -ne $prevMs) {
				if (-not $ms.ContainsKey($m.name)) { $ms[$m.name] = @(); $mb[$m.name] = @() }
				$ms[$m.name] += ([double]$m.loaded_ms - $prevMs)
				$mb[$m.name] += ([double]$m.mb_at_load - $prevMb)
			}
			$prevMs = [double]$m.loaded_ms
			$prevMb = [double]$m.mb_at_load
		}
	}

	$result = @{}
	foreach ($name in $ms.Keys) {
		$result[$name] = [pscustomobject]@{ ms = (Get-Median $ms[$name]); mb = (Get-Median $mb[$name]) }
	}
	return $result
}

function Format-Num {
	param($Value, [string]$Format = 'N1')
	if ($null -eq $Value) { return '-' }
	return ([double]$Value).ToString($Format, [System.Globalization.CultureInfo]::InvariantCulture)
}

function Format-Signed {
	param($Value, [string]$Format = 'N1')
	if ($null -eq $Value) { return '-' }
	$v = [double]$Value
	$text = $v.ToString($Format, [System.Globalization.CultureInfo]::InvariantCulture)
	if ($v -gt 0) { return '+' + $text }
	return $text
}

function Write-Table {
	# Format-Table without the truncation of a narrow console.
	param($Rows)
	if (@($Rows).Count -eq 0) { Write-Host '   (none)'; return }
	($Rows | Format-Table -AutoSize | Out-String -Width 260).TrimEnd() | Write-Host
}
