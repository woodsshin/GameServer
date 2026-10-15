# Server diet

Which modules and plugins does the dedicated server load, what did they cost, is anything using them, and what changes
when some are switched off. Output is JSON (one report per run), so two builds can be compared.

## Files

| File | What |
|---|---|
| `Source/MassBubble/Core/MassBubbleDiet.h/.cpp` | The reporter. `-OptDietReport`, `-OptDietModules`, `opt.diet.Report`, `opt.diet.Plugins`. |
| `Tools/ServerDiet/Run-ServerDiet.ps1` | Starts the server N times, collects `runN.json` and `runN.log`. |
| `Tools/ServerDiet/Get-DietCandidates.ps1` | Which plugins can go, why the others stay, the `.uproject` entries to write. |
| `Tools/ServerDiet/Compare-ServerDiet.ps1` | Before / after: modules, plugins, `Module.*` milestones, ProcessMB, new warnings. |

## Workflow

1. Stage the server as it is (Development or Test keeps the console commands; the options and the report also work in Shipping).
2. `.\Run-ServerDiet.ps1 -Exe <staged>\MassBubbleServer.exe -Label before -Runs 5 -ExtraArgs '-OptBots=8'`
3. `.\Get-DietCandidates.ps1 -Report .\DietRuns\before -Uproject <project>\MassBubble.uproject -OutFile .\diet-plan.txt`
4. Put the printed entries into the `Plugins` array of the `.uproject`, rebuild and stage the server again.
5. `.\Run-ServerDiet.ps1 -Exe <staged-after>\MassBubbleServer.exe -Label after -Runs 5 -ExtraArgs '-OptBots=8'`
6. `.\Compare-ServerDiet.ps1 -Before .\DietRuns\before -After .\DietRuns\after`

Same map and same `-ExtraArgs` for both labels, or the memory figures do not compare.

By hand, in a running server: start with `-OptDietModules`, then `opt.boot.Modules 1` (every loaded module with its plugin),
`opt.diet.Plugins 1`, `opt.diet.Report <label>`.

## What a report contains

* `milestones`: the Boot timeline, in ms since the first milestone and since process start (`GStartTime`).
  `-OptDietModules` adds `Module.<Name>.Loaded` for every module loaded after `MassBubble` started and
  `Module.Phase.<Phase>.Done` for every plugin loading phase.
* `memory_samples` / `modules[].mb_at_load`: the process size (`UsedPhysical`, the same quantity as the `OptCrowd/ProcessMB`
  CSV stat) when the hook was installed, at every module load, at `Engine.InitComplete` and at the report.
* `modules[]`: loaded modules with owner plugin, load time, `uclasses` (registered) and `instances` (live objects of its classes).
  Modules loaded before `MassBubble` started have no load time (`loaded_before_hook`).
* `plugins[]`: enabled plugins, how many modules are loaded, required / optional plugin dependencies, live objects of the
  plugin's classes, objects in its mounted content.
* `objects`, `executable_mb`, `timing`, `memory` (settled and peak).

## Reading the results

* "In use" = live objects or content objects exist. No objects does not prove a plugin is unused (it may serve a map that is
  not loaded), so the log diff in `Compare-ServerDiet.ps1` matters: missing classes and failed loads show up there.
* The per-module MB / ms are differences between consecutive load events: rough, anything that ran in between is charged
  to the module. In a monolithic build a module that is linked but never started has no load event; its UClasses are
  still registered at startup (see `uclasses`, `UObjects`, `executable (MB)`).
* `Enabled:false` + `TargetAllowList:["Server"]` switches a plugin off for the server only; for a plugin that the project
  enables explicitly, `TargetDenyList:["Server"]` does it. UBT stops with an error if a plugin that stays requires one
  that was switched off, which is the safety net for plugin dependencies.

## Measured result

Staged dedicated server, `-OptBots=8`, 5 runs per label after one discarded warm-up
(`Run-ServerDiet.ps1 -Runs 5 -ExtraArgs '-OptBots=8'`). Same executable path and same arguments for both labels.
Plugins switched off for the server only (`Enabled:false` + `TargetAllowList:["Server"]`): 107 of 192. The list of
removed plugins is in the `Plugins only in BEFORE` block of `Compare-ServerDiet.ps1`.

### Runs

| Run | exit | wall (s) | modules | init (ms) | process (MB) |
|---|---|---|---|---|---|
| before, run 1 | 0 | 16.9 | 279 | 1,194 | 352.1 |
| before, run 2 | 0 | 16.9 | 279 | 1,191 | 351.9 |
| before, run 3 | 0 | 16.9 | 279 | 1,184 | 352.0 |
| before, run 4 | 0 | 16.9 | 279 | 1,186 | 352.3 |
| before, run 5 | 0 | 16.9 | 279 | 1,215 | 351.9 |
| after, run 1 | 0 | 16.4 | 206 | 1,063 | 336.6 |
| after, run 2 | 0 | 16.4 | 206 | 1,042 | 335.9 |
| after, run 3 | 0 | 16.4 | 206 | 1,051 | 336.2 |
| after, run 4 | 0 | 16.5 | 206 | 1,053 | 335.8 |
| after, run 5 | 0 | 16.5 | 206 | 1,075 | 336.4 |

The runs agree closely: the spread of `process_mb` is under 0.5 MB per label and the module count is constant, so the
medians below are not driven by outliers. The discarded warm-up runs (before 1,303 ms / 352.2 MB, after 1,112 ms / 335.9 MB)
are slower on init only.

### Headline numbers (median of the runs)

| Metric | Before | After | Delta | Delta % |
|---|---|---|---|---|
| modules loaded | 279 | 206 | -73 | -26.2% |
| plugins enabled | 192 | 85 | -107 | -55.7% |
| plugins with code loaded | 133 | 71 | -62 | -46.6% |
| `Module.*.Loaded` events seen | 141 | 105 | -36 | -25.5% |
| module load span (ms) | 257 | 229 | -28 | -10.8% |
| `Module.Phase.Default.Done` (ms since process) | 1,047 | 913 | -134 | -12.8% |
| `Engine.InitComplete` (ms since process) | 1,191 | 1,053 | -137 | -11.5% |
| ProcessMB at hook | 273.9 | 258.5 | -15.4 | -5.6% |
| ProcessMB at `Engine.InitComplete` | 296.5 | 278.3 | -18.2 | -6.1% |
| ProcessMB at report (settled) | 352.0 | 336.2 | -15.9 | -4.5% |
| peak physical (MB) | 767.6 | 751.6 | -16.0 | -2.1% |
| virtual (MB) | 251.3 | 244.6 | -6.7 | -2.7% |
| UObjects | 38,782 | 36,622 | -2,160 | -5.6% |
| UClasses | 5,811 | 5,573 | -238 | -4.1% |
| executable (MB) | 305.9 | 289.9 | -16.0 | -5.2% |

### Startup milestones (ms since process start, median)

| Milestone | Before | After | Delta |
|---|---|---|---|
| `Module.MassBubble.Startup` | 980 | 866 | -114 |
| `Module.Phase.Default.Done` | 1,047 | 913 | -134 |
| `Module.Phase.PostDefault.Done` | 1,053 | 919 | -134 |
| `Module.Phase.PostEngineInit.Done` | 1,111 | 974 | -137 |
| `Engine.InitComplete` | 1,191 | 1,053 | -137 |
| `Diet.ReportDue` | 16,216 | 16,084 | -132 |

Most of the gain is already in place when `MassBubble` starts (-114 ms): the removed plugins no longer take part in
the early module and plugin loading. The later `Module.<Name>.Loaded` milestones move by 137-145 ms; the shift is
cumulative, so every module after a removed one moves up too.

### What the removed plugins cost

* Nothing is lost: `Plugins only in AFTER` and `Modules only in AFTER` are empty, and there are 0 new warnings or errors
  in the after logs (0 in the before logs as well).
* Live objects of the removed plugins are almost nil: `Mutable` 15, `PhysicsControl` 2, every other plugin 0. Content
  objects are 0 for all of them.
* The module-load samples add up to about 1.4 MB and 19 ms. The measured drop is 15.9 MB settled and 137 ms to
  `Engine.InitComplete`. The samples cannot see what linked but never started modules cost: their UClasses are still
  registered at startup (-238 UClasses, -2,160 UObjects) and their code is part of the executable (-16.0 MB).
  The measured delta is therefore the figure to quote, not the sum of the samples.

### Reading the numbers

* `executable (MB)` and `ProcessMB settled` drop by the same 16 MB: the saving is the linked code and its startup
  registration, not heap that grows at runtime.
* `peak physical` drops by 16.0 MB as well, so the saving also holds under the load of 8 bots.
* The wall time of a run is dominated by the report timer (`Diet.ReportDue` at about 16 s), so startup savings barely
  show in `wall_s`. Compare `init_ms` and `Engine.InitComplete` instead.