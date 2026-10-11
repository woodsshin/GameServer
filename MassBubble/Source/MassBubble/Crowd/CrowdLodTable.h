#pragma once

#include "CoreMinimal.h"
#include "Crowd/CrowdMath.h"
#include "Crowd/CrowdTypes.h"

/**
 * Per-frame bounds that replace the loop over every viewer when agents get their LOD tier.
 *
 * Each live region is classified from the distance between its box and the viewers:
 *   AllOff   no agent of the region can be nearer to any viewer than the outermost tier boundary
 *   AllHigh  every agent is inside the innermost tier boundary of some viewer
 *   Mixed    the nearest viewer is measured, but only among the viewers that can be nearest to an agent of the region
 * The tier is identical to the one the loop over every viewer gives (MassBubble.Crowd.Opt.LodTable).
 */
namespace CrowdLod
{
	enum class ERegionKind : uint8
	{
		Mixed,
		AllHigh,
		AllOff,
	};

	struct FRegionEntry
	{
		ERegionKind Kind = ERegionKind::Mixed;
		int32 FirstViewer = 0;

		/** Viewers listed for this region. -1: no bounds known, measure every viewer. */
		int32 NumViewers = -1;
	};

	/** With fewer viewers the loop over all of them costs about as much as building and reading the table. */
	constexpr int32 MinViewersForTable = 4;

	/** A shortcut is only taken this far (cm) inside its condition, so float rounding of the measured distance cannot contradict it. */
	constexpr double SafetyCm = 1.0;

	/** Agents stay in their region box; the bounds use a box this much larger in case one sits a hair outside. */
	constexpr double PadCm = 100.0;

	/** Squared distance from P to the closest point of the box. */
	FORCEINLINE double NearSq(const FVector2D& BoxMin, const FVector2D& BoxMax, const FVector2D& P)
	{
		const double DX = FMath::Max(FMath::Max(BoxMin.X - P.X, P.X - BoxMax.X), 0.0);
		const double DY = FMath::Max(FMath::Max(BoxMin.Y - P.Y, P.Y - BoxMax.Y), 0.0);
		return DX * DX + DY * DY;
	}

	/** Squared distance from P to the farthest corner of the box. */
	FORCEINLINE double FarSq(const FVector2D& BoxMin, const FVector2D& BoxMax, const FVector2D& P)
	{
		const double DX = FMath::Max(FMath::Abs(P.X - BoxMin.X), FMath::Abs(P.X - BoxMax.X));
		const double DY = FMath::Max(FMath::Abs(P.Y - BoxMin.Y), FMath::Abs(P.Y - BoxMax.Y));
		return DX * DX + DY * DY;
	}

	/** Tier of an agent at Pos, measured against NumViewers viewers (none: no viewer, so Off). */
	inline ECrowdLOD ClassifyAgainst(const FVector2D* Viewers, int32 NumViewers, const FVector2D& Pos, ECrowdLOD Current, const FCrowdTuning& Tuning)
	{
		float Distance = 1.0e12f;
		if (NumViewers > 0)
		{
			double BestSq = TNumericLimits<double>::Max();
			for (int32 i = 0; i < NumViewers; ++i)
			{
				BestSq = FMath::Min(BestSq, FVector2D::DistSquared(Pos, Viewers[i]));
			}
			Distance = static_cast<float>(FMath::Sqrt(BestSq));
		}
		return CrowdMath::ComputeTier(Distance, Current, Tuning);
	}

	struct FTable
	{
		/** Indexed by region slot. */
		TArray<FRegionEntry> Entries;

		/** Viewer lists of the Mixed regions, back to back. */
		TArray<FVector2D> Viewers;

		/** Every viewer: used for a slot without an entry. */
		TArray<FVector2D> AllViewers;

		int32 NumMixed = 0;
		int32 NumAllHigh = 0;
		int32 NumAllOff = 0;

		/** Starts a frame: NumSlots region slots, every entry unknown. */
		void Begin(int32 NumSlots, TConstArrayView<FVector2D> InViewers)
		{
			Entries.Reset();
			Entries.SetNum(NumSlots);
			Viewers.Reset();
			AllViewers.Reset();
			for (const FVector2D& Viewer : InViewers)
			{
				AllViewers.Add(Viewer);
			}
			NumMixed = 0;
			NumAllHigh = 0;
			NumAllOff = 0;
		}

		/** Bounds for the region in Slot, whose lower corner is RegionMin. */
		void AddRegion(int32 Slot, const FVector2D& RegionMin, double SizeCm, const FCrowdTuning& Tuning)
		{
			if (!Entries.IsValidIndex(Slot))
			{
				return;
			}
			FRegionEntry& Entry = Entries[Slot];

			if (AllViewers.Num() == 0)
			{
				Entry.Kind = ERegionKind::AllOff;
				Entry.NumViewers = 0;
				++NumAllOff;
				return;
			}

			const FVector2D BoxMin = RegionMin - FVector2D(PadCm, PadCm);
			const FVector2D BoxMax = RegionMin + FVector2D(SizeCm + PadCm, SizeCm + PadCm);

			double LowerSq = TNumericLimits<double>::Max(); // no agent is nearer than this to any viewer
			double UpperSq = TNumericLimits<double>::Max(); // every agent has a viewer at most this far away
			for (const FVector2D& Viewer : AllViewers)
			{
				LowerSq = FMath::Min(LowerSq, NearSq(BoxMin, BoxMax, Viewer));
				UpperSq = FMath::Min(UpperSq, FarSq(BoxMin, BoxMax, Viewer));
			}

			const double OffLimit = static_cast<double>(Tuning.LODDistanceCm[2]) + static_cast<double>(Tuning.LODHysteresisCm) + SafetyCm;
			const double HighLimit = static_cast<double>(Tuning.LODDistanceCm[0]) - static_cast<double>(Tuning.LODHysteresisCm) - SafetyCm;

			if (LowerSq > OffLimit * OffLimit)
			{
				Entry.Kind = ERegionKind::AllOff;
				Entry.NumViewers = 0;
				++NumAllOff;
			}
			else if (HighLimit > 0.0 && UpperSq < HighLimit * HighLimit)
			{
				Entry.Kind = ERegionKind::AllHigh;
				Entry.NumViewers = 0;
				++NumAllHigh;
			}
			else
			{
				// A viewer that is farther from the box than UpperSq cannot be the nearest one for any agent in it.
				Entry.Kind = ERegionKind::Mixed;
				Entry.FirstViewer = Viewers.Num();
				for (const FVector2D& Viewer : AllViewers)
				{
					if (NearSq(BoxMin, BoxMax, Viewer) <= UpperSq)
					{
						Viewers.Add(Viewer);
					}
				}
				Entry.NumViewers = Viewers.Num() - Entry.FirstViewer;
				++NumMixed;
			}
		}

		ECrowdLOD Classify(int32 Slot, const FVector2D& Pos, ECrowdLOD Current, const FCrowdTuning& Tuning) const
		{
			if (Entries.IsValidIndex(Slot))
			{
				const FRegionEntry& Entry = Entries[Slot];
				if (Entry.NumViewers >= 0)
				{
					switch (Entry.Kind)
					{
					case ERegionKind::AllOff:
						return ECrowdLOD::Off;
					case ERegionKind::AllHigh:
						return ECrowdLOD::High;
					default:
						return ClassifyAgainst(Viewers.GetData() + Entry.FirstViewer, Entry.NumViewers, Pos, Current, Tuning);
					}
				}
			}
			return ClassifyAgainst(AllViewers.GetData(), AllViewers.Num(), Pos, Current, Tuning);
		}
	};
}
