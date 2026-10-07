#pragma once

#include "CoreMinimal.h"

/**
 * Console switches of UMassBubbleStreamingMonitor. All of them can be changed at runtime.
 *
 *   opt.stream.ApplyProfile    0 = do not apply the engine CVar profile (read when the first world starts)
 *   opt.stream.QuietGC         1 = run the GC after cell unloads when World Partition is quiet (instead of right away)
 *   opt.stream.GCQuietSec      seconds without any cell loading / being added / removed that count as "quiet"
 *   opt.stream.GCMaxDeferSec   run the GC anyway after waiting this long
 *   opt.stream.GCMinSpacingSec never start two of these GCs closer together than this
 *   opt.hitch.LogMs            > 0 = log every frame longer than this, with GC / World Partition / crowd state
 */
namespace MassBubbleStreamCVars
{
	extern MASSBUBBLE_API int32 ApplyProfile;
	extern MASSBUBBLE_API int32 QuietGC;
	extern MASSBUBBLE_API float GCQuietSec;
	extern MASSBUBBLE_API float GCMaxDeferSec;
	extern MASSBUBBLE_API float GCMinSpacingSec;
	extern MASSBUBBLE_API float HitchLogMs;
}

namespace MassBubbleConfig
{
	/**
	 * Applies the engine CVar profile (World Partition / level streaming / GC time slicing) once per process.
	 * The values are set with ECVF_SetByProjectSetting: DefaultEngine.ini [SystemSettings], the command line and the
	 * console all win over them. CVars that do not exist in this engine version are reported and skipped.
	 */
	MASSBUBBLE_API void ApplyStreamingProfileOnce();

	/** Applies the profile now (console: opt.stream.Apply). Returns the number of entries that are in effect. */
	MASSBUBBLE_API int32 ApplyStreamingProfile(bool bLog);

	/** Logs every profile entry next to the value that is currently active (console: opt.stream.Dump). */
	MASSBUBBLE_API void DumpStreamingProfile();

	/** "Iris (command line)", "legacy (net.Iris.UseIrisReplication)" ... what the process was asked to use. */
	MASSBUBBLE_API FString GetReplicationModeString();

	/** Logs the replication related CVars (console: opt.net.Info). */
	MASSBUBBLE_API void DumpNetConfig();
}
