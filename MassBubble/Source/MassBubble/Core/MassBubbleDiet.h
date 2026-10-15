#pragma once

#include "CoreMinimal.h"

/**
 * Server diet: which modules and plugins a process loaded, what that cost, and whether anything uses them.
 * Written as JSON so two builds can be compared (Tools/ServerDiet).
 *
 * Command line (works in every configuration, Shipping included):
 *   -OptDietModules          every module load becomes a "Module.<Name>.Loaded" milestone (with the process size at that moment),
 *                            every plugin loading phase a "Module.Phase.<Phase>.Done" milestone
 *   -OptDietReport=<label>   implies -OptDietModules. Once the engine has settled, writes <label>.json and logs where
 *   -OptDietOut=<path>       exact file of that report (default: <Saved>/Diet/<label>.json)
 *   -OptDietSettleSec=<s>    seconds between "Engine.InitComplete" and the report (default 15)
 *   -OptDietQuit             exit after the report was written (unattended A/B runs)
 *
 * Console (non-shipping):
 *   opt.diet.Report [label]  write the report now (default label "manual"; module load times need -OptDietModules at startup)
 *   opt.diet.Plugins [1]     enabled plugins with how many of their modules are loaded; 1 also lists the modules
 *
 * opt.boot.Modules 1 uses GetModuleOwner() to print which plugin owns every loaded module.
 */
namespace MassBubbleDiet
{
	/** Called once from the module's StartupModule: reads the command line, hooks module loads and the end of engine init. */
	void Install();

	/** Name of the plugin that owns the module, empty for engine / game modules. */
	MASSBUBBLE_API FString GetModuleOwner(FName ModuleName);

	/** The report as JSON text. */
	MASSBUBBLE_API FString BuildReportJson(const FString& Label);

	/** Writes the report. OutPath empty = <Saved>/Diet/<Label>.json. Returns the file that was written, empty on failure. */
	MASSBUBBLE_API FString WriteReport(const FString& Label, const FString& OutPath);

	/** Logs the enabled plugins (console: opt.diet.Plugins). */
	MASSBUBBLE_API void LogPlugins(bool bListModules);
}
