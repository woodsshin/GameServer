<#
.SYNOPSIS
	Which enabled plugins can the dedicated server do without? Reads a report written by the server (-OptDietReport).

.DESCRIPTION
	A plugin is a removal candidate when none of these keeps it:
	  * project code     a module of the plugin is named in the Build.cs files of the server's own modules
	                     (read from the *Server.Target.cs ExtraModuleNames and their *.Build.cs, needs -Uproject or -SourceDir)
	  * runtime use      the server holds live objects of the plugin's classes or objects from its content
	                     (the report counts them; -IncludeUsed lists those plugins as candidates too)
	  * dependencies     a plugin that is kept lists it as a required plugin (followed transitively)
	  * -Keep            names you give
	The estimated memory / time come from the module-load samples of the report (see DietCommon.ps1, Get-ModuleCosts).

	It prints what to put into the .uproject (a plugin entry per candidate, with TargetAllowList / TargetDenyList so that
	the Editor and the client keep the plugin) and writes the same to -OutFile.

.EXAMPLE
	.\Get-DietCandidates.ps1 -Report .\DietRuns\before -Uproject D:\Proj\MassBubble.uproject -OutFile .\diet-plan.txt

.NOTES
	This is evidence, not proof. A plugin can still be needed by cooked content that is not loaded in the run, or by an
	.ini setting. Compare-ServerDiet.ps1 diffs the warnings and errors of the two builds for exactly that reason.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$Report,
	[string]$Uproject,
	[string]$SourceDir,
	[string[]]$Keep = @(),
	[switch]$IncludeUsed,
	[string]$OutFile
)

. "$PSScriptRoot\DietCommon.ps1"

$reports = @(Read-DietReports $Report)
$ignoreCase = [System.StringComparer]::OrdinalIgnoreCase

# ---------------------------------------------------------------------------------------------------------------------
# Plugins as the server saw them (evidence = the largest figure of any run)
# ---------------------------------------------------------------------------------------------------------------------
$plugins = @{}
foreach ($r in $reports) {
	foreach ($p in @($r.plugins)) {
		$deps = @(@($p.depends_on) | ForEach-Object { [pscustomobject]@{ name = [string]$_.name; optional = [bool]$_.optional } })
		if (-not $plugins.ContainsKey($p.name)) {
			$plugins[$p.name] = [pscustomobject]@{
				name = [string]$p.name; origin = [string]$p.origin; descriptor = [string]$p.descriptor
				enabled_by_default = [bool]$p.enabled_by_default; can_contain_content = [bool]$p.can_contain_content
				declared = [int]$p.modules_declared; loaded = [int]$p.modules_loaded
				instances = [int]$p.instances; content = [int]$p.content_objects
				deps = $deps; modules = @(@($p.modules) | ForEach-Object { [string]$_.name })
			}
		}
		else {
			$e = $plugins[$p.name]
			$e.loaded = [math]::Max($e.loaded, [int]$p.modules_loaded)
			$e.instances = [math]::Max($e.instances, [int]$p.instances)
			$e.content = [math]::Max($e.content, [int]$p.content_objects)
		}
	}
}
if ($plugins.Count -eq 0) { throw 'The report lists no plugins.' }

$costs = Get-ModuleCosts $reports
$moduleOwner = @{}
foreach ($m in @($reports[0].modules)) { if ([string]$m.plugin -ne '') { $moduleOwner[[string]$m.name] = [string]$m.plugin } }
foreach ($p in $plugins.Values) { foreach ($mn in $p.modules) { if (-not $moduleOwner.ContainsKey($mn)) { $moduleOwner[$mn] = $p.name } } }

$refBy = @{}   # plugin -> the plugins that list it (required or optional)
foreach ($p in $plugins.Values) {
	foreach ($d in $p.deps) {
		if (-not $refBy.ContainsKey($d.name)) { $refBy[$d.name] = @() }
		$refBy[$d.name] += $p.name
	}
}

# ---------------------------------------------------------------------------------------------------------------------
# What the project's own server modules need (Target.cs + Build.cs)
# ---------------------------------------------------------------------------------------------------------------------
function Remove-CsComments {
	param([string]$Text)
	$t = [regex]::Replace($Text, '/\*.*?\*/', '', [System.Text.RegularExpressions.RegexOptions]::Singleline)
	return [regex]::Replace($t, '//[^\r\n]*', '')
}

function Get-CsLiterals {
	# String literals inside the argument list of statements like  Name.Add("A")  /  Name.AddRange(new string[] { "A", "B" });
	param([string]$Text, [string]$Members)
	$found = @()
	$pattern = '(?:' + $Members + ')\s*\.\s*(?:Add|AddRange)\s*\((.*?)\)\s*;'
	foreach ($m in [regex]::Matches($Text, $pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)) {
		foreach ($s in [regex]::Matches($m.Groups[1].Value, '"([^"]+)"')) { $found += $s.Groups[1].Value }
	}
	return $found
}

$uprojectObject = $null
$uprojectEntries = @{}
$disableEngineDefault = $false
if ($Uproject) {
	$uprojectObject = Get-Content -LiteralPath $Uproject -Raw -Encoding UTF8 | ConvertFrom-Json
	foreach ($e in @(Get-Prop $uprojectObject 'Plugins' @())) { $uprojectEntries[[string]$e.Name] = $e }
	$disableEngineDefault = [bool](Get-Prop $uprojectObject 'DisableEnginePluginsByDefault' $false)
	if (-not $SourceDir) { $SourceDir = Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $Uproject).Path) 'Source' }
}

$needed = $null
$neededNote = 'Build.cs files not read (give -Uproject or -SourceDir): "project code" does not keep any plugin'
if ($SourceDir -and (Test-Path -LiteralPath $SourceDir)) {
	$serverTargets = @(Get-ChildItem -LiteralPath $SourceDir -Recurse -Filter '*Server.Target.cs')
	if ($serverTargets.Count -eq 0) {
		$neededNote = "no *Server.Target.cs under ${SourceDir} - `"project code`" does not keep any plugin"
	}
	else {
		$needed = New-Object 'System.Collections.Generic.HashSet[string]' ($ignoreCase)
		$queue = New-Object System.Collections.Generic.Queue[string]
		foreach ($t in $serverTargets) {
			$text = Remove-CsComments (Get-Content -LiteralPath $t.FullName -Raw)
			foreach ($n in (Get-CsLiterals $text 'ExtraModuleNames')) { if ($needed.Add($n)) { $queue.Enqueue($n) } }
		}
		$visited = New-Object 'System.Collections.Generic.HashSet[string]' ($ignoreCase)
		while ($queue.Count -gt 0) {
			$module = $queue.Dequeue()
			if (-not $visited.Add($module)) { continue }
			$buildFile = Get-ChildItem -LiteralPath $SourceDir -Recurse -Filter "$module.Build.cs" | Select-Object -First 1
			if ($null -eq $buildFile) { continue }   # an engine / plugin module, nothing more to read
			$text = Remove-CsComments (Get-Content -LiteralPath $buildFile.FullName -Raw)
			$deps = Get-CsLiterals $text 'PublicDependencyModuleNames|PrivateDependencyModuleNames|DynamicallyLoadedModuleNames|PublicIncludePathModuleNames|PrivateIncludePathModuleNames'
			foreach ($d in $deps) { if ($needed.Add($d)) { $queue.Enqueue($d) } }
		}
		$neededNote = "read $($serverTargets.Count) Server.Target.cs and the Build.cs files of $($visited.Count) modules"
	}
}

# ---------------------------------------------------------------------------------------------------------------------
# What stays
# ---------------------------------------------------------------------------------------------------------------------
$keepReason = @{}
foreach ($p in $plugins.Values) {
	if ($null -ne $needed) {
		foreach ($mn in $p.modules) { if ($needed.Contains($mn)) { $keepReason[$p.name] = "project code uses module $mn"; break } }
	}
}
foreach ($k in $Keep) { if ($plugins.ContainsKey($k)) { $keepReason[$plugins[$k].name] = 'named in -Keep' } }
if (-not $IncludeUsed) {
	foreach ($p in $plugins.Values) {
		if (-not $keepReason.ContainsKey($p.name) -and ($p.instances -gt 0 -or $p.content -gt 0)) {
			$keepReason[$p.name] = "in use: $($p.instances) live objects, $($p.content) content objects"
		}
	}
}
$work = New-Object System.Collections.Generic.Queue[string]
foreach ($k in @($keepReason.Keys)) { $work.Enqueue($k) }
while ($work.Count -gt 0) {
	$name = $work.Dequeue()
	foreach ($d in $plugins[$name].deps) {
		if ($d.optional -or -not $plugins.ContainsKey($d.name)) { continue }
		if (-not $keepReason.ContainsKey($d.name)) { $keepReason[$d.name] = "required by $name"; $work.Enqueue($d.name) }
	}
}

# ---------------------------------------------------------------------------------------------------------------------
# Candidates
# ---------------------------------------------------------------------------------------------------------------------
function Test-EntryAppliesToServer {
	param($Entry)
	$allow = @(Get-Prop $Entry 'TargetAllowList' @())
	$deny = @(Get-Prop $Entry 'TargetDenyList' @())
	if ($allow.Count -gt 0 -and ($allow -notcontains 'Server')) { return $false }
	if ($deny -contains 'Server') { return $false }
	return $true
}

$rows = @()
foreach ($p in ($plugins.Values | Sort-Object name)) {
	if ($keepReason.ContainsKey($p.name)) { continue }

	$mb = 0.0; $ms = 0.0
	foreach ($mn in $p.modules) { if ($costs.ContainsKey($mn)) { $mb += [double]$costs[$mn].mb; $ms += [double]$costs[$mn].ms } }

	$keptRefs = @(@($refBy[$p.name]) | Where-Object { $_ -and $keepReason.ContainsKey($_) })
	$isDefault = $p.enabled_by_default -and -not ($p.origin -eq 'engine' -and $disableEngineDefault)
	$entry = if ($uprojectEntries.ContainsKey($p.name)) { $uprojectEntries[$p.name] } else { $null }
	$isExplicit = ($null -ne $entry) -and [bool](Get-Prop $entry 'Enabled' $true) -and (Test-EntryAppliesToServer $entry)

	if (-not $Uproject) { $enabledBy = 'unknown (no -Uproject)'; $action = 'Disable' }
	elseif ($isDefault) { $enabledBy = 'default'; $action = 'Disable' }
	elseif ($keptRefs.Count -gt 0) { $enabledBy = 'dependency of a kept plugin'; $action = if ($isExplicit) { 'DenyServer+Disable' } else { 'Disable' } }
	elseif ($isExplicit) { $enabledBy = '.uproject'; $action = 'DenyServer' }
	else { $enabledBy = 'dependency of a candidate'; $action = 'None' }

	$notes = @()
	if ($p.can_contain_content) { $notes += 'has content' }
	if ($p.instances -gt 0 -or $p.content -gt 0) { $notes += 'IN USE' }
	if ($p.loaded -eq 0) { $notes += 'no code loaded: saves link size only' }
	if ($keptRefs.Count -gt 0) { $notes += 'referenced by ' + ($keptRefs -join ', ') }

	$rows += [pscustomobject]@{
		Plugin = $p.name; From = $p.origin; Code = "$($p.loaded)/$($p.declared)"; Inst = $p.instances; Content = $p.content
		'~MB' = (Format-Num $mb); '~ms' = (Format-Num $ms 'N0'); 'Enabled by' = $enabledBy; Notes = ($notes -join '; ')
		_mb = $mb; _ms = $ms; _action = $action; _entry = $entry; _enabledBy = $enabledBy
	}
}
$rows = @($rows | Sort-Object @{ Expression = '_mb'; Descending = $true }, @{ Expression = '_ms'; Descending = $true }, Plugin)

# ---------------------------------------------------------------------------------------------------------------------
# .uproject text
# ---------------------------------------------------------------------------------------------------------------------
function ConvertTo-JsonArrayText {
	param([string[]]$Items, [string]$Indent)
	$inner = ($Items | ForEach-Object { "$Indent`t`"$_`"" }) -join ",`n"
	return "[`n$inner`n$Indent]"
}

function ConvertTo-EntryText {
	param([string]$Name, [bool]$Enabled, [string[]]$Allow, [string[]]$Deny, $Existing)
	$props = @()
	$props += "`t`t`t`"Name`": `"$Name`""
	$props += "`t`t`t`"Enabled`": " + $(if ($Enabled) { 'true' } else { 'false' })
	if ($null -ne $Existing) {
		foreach ($prop in $Existing.PSObject.Properties) {
			if (@('Name', 'Enabled', 'TargetAllowList', 'TargetDenyList') -contains $prop.Name) { continue }
			$v = $prop.Value
			if ($v -is [bool]) { $props += "`t`t`t`"$($prop.Name)`": " + $(if ($v) { 'true' } else { 'false' }) }
			elseif ($v -is [string]) { $props += "`t`t`t`"$($prop.Name)`": `"$($v -replace '\\', '\\' -replace '"', '\"')`"" }
			elseif ($v -is [array] -and @($v | Where-Object { $_ -isnot [string] }).Count -eq 0) { $props += "`t`t`t`"$($prop.Name)`": " + (ConvertTo-JsonArrayText ([string[]]$v) "`t`t`t") }
			else { $props += "`t`t`t`"$($prop.Name)`": $($v | ConvertTo-Json -Compress -Depth 5)" }
		}
	}
	if ($Allow.Count -gt 0) { $props += "`t`t`t`"TargetAllowList`": " + (ConvertTo-JsonArrayText $Allow "`t`t`t") }
	if ($Deny.Count -gt 0) { $props += "`t`t`t`"TargetDenyList`": " + (ConvertTo-JsonArrayText $Deny "`t`t`t") }
	return "`t`t{`n" + ($props -join ",`n") + "`n`t`t}"
}

$edits = @()      # text blocks for the user
$entriesText = @()
foreach ($row in $rows) {
	$name = $row.Plugin
	switch -Wildcard ($row._action) {
		'*Disable*' {
			$entriesText += (ConvertTo-EntryText $name $false @('Server') @() $null)
		}
	}
	if ($row._action -like '*DenyServer*') {
		$existing = $row._entry
		$allow = @(Get-Prop $existing 'TargetAllowList' @()) | Where-Object { $_ -ne 'Server' }
		$deny = @(@(Get-Prop $existing 'TargetDenyList' @()) + 'Server' | Select-Object -Unique)
		$entriesText += (ConvertTo-EntryText $name $true @($allow) $deny $existing)
	}
}

# ---------------------------------------------------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------------------------------------------------
$out = New-Object System.Collections.Generic.List[string]
function Emit { param([string]$Line = '') $out.Add($Line); Write-Host $Line }

$totalMb = 0.0; $totalMs = 0.0
foreach ($r in $rows) { $totalMb += $r._mb; $totalMs += $r._ms }

Emit "== Server plugin diet: $($plugins.Count) enabled plugins, $(@($reports).Count) run(s) from $Report"
Emit "   $neededNote"
Emit ''
Emit "Kept ($($keepReason.Count)):"
function Get-KeepKind {
	param([string]$Reason)
	if ($Reason -like 'project code*') { return '1 project code (Build.cs)' }
	if ($Reason -like 'named in*') { return '2 named in -Keep' }
	if ($Reason -like 'in use*') { return '3 in use at runtime' }
	return '4 required by a kept plugin'
}
foreach ($g in ($keepReason.GetEnumerator() | Group-Object { Get-KeepKind $_.Value } | Sort-Object Name)) {
	$names = $g.Group | Sort-Object Key | ForEach-Object {
		if ($g.Name -like '3*') { "$($_.Key) [$($plugins[$_.Key].instances) objects, $($plugins[$_.Key].content) content]" } else { $_.Key }
	}
	Emit ("   {0,-30} {1}" -f $g.Name.Substring(2), ($names -join ', '))
}
Emit ''
Emit "Candidates ($($rows.Count) plugins, about $(Format-Num $totalMb) MB and $(Format-Num $totalMs 'N0') ms of module loading; estimates):"
Emit ''
$table = ($rows | Select-Object Plugin, From, Code, Inst, Content, '~MB', '~ms', 'Enabled by', Notes | Format-Table -AutoSize | Out-String -Width 260).TrimEnd()
Emit $table
Emit ''

if ($entriesText.Count -gt 0) {
	Emit '.uproject "Plugins" entries (Enabled:false + TargetAllowList:Server switches a plugin off for the server only; the Editor and the client are untouched):'
	Emit ''
	Emit ($entriesText -join ",`n")
	Emit ''
	foreach ($row in ($rows | Where-Object { $_._entry -ne $null -and $_._action -like '*Disable*' -and $_._enabledBy -eq 'default' })) {
		Emit "   replace the existing .uproject entry of $($row.Plugin) with the one above (it only repeats the default)"
	}
	foreach ($row in ($rows | Where-Object { $_._action -eq 'DenyServer+Disable' })) {
		Emit "   $($row.Plugin): two entries with the same name, one per target set (the existing one gets the Server deny list, the new one disables it for the server)"
	}
	foreach ($row in ($rows | Where-Object { $_._action -eq 'DenyServer' })) {
		Emit "   $($row.Plugin): replace its existing entry with the one above (adds Server to TargetDenyList)"
	}
}
$goesAway = @($rows | Where-Object { $_._action -eq 'None' })
if ($goesAway.Count -gt 0) {
	Emit ''
	Emit ('No entry needed (only enabled through other candidates, they go away together): ' + (($goesAway | ForEach-Object { $_.Plugin }) -join ', '))
}
Emit ''
Emit 'Then: rebuild + stage the server, run Run-ServerDiet.ps1 -Label after, and Compare-ServerDiet.ps1. UBT stops with an error if a kept plugin still requires a disabled one.'

if ($OutFile) { Set-Content -LiteralPath $OutFile -Value $out -Encoding UTF8; Write-Host "written: $OutFile" }
