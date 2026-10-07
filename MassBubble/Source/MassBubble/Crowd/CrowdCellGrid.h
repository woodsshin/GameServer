#pragma once

#include "CoreMinimal.h"

/** One agent as seen by the replication layer: a flat copy, no entity manager access needed. */
struct FCrowdGridAgent
{
	FVector2D Pos = FVector2D::ZeroVector;
	FVector2f Vel = FVector2f::ZeroVector;
	uint32 NetId = 0;
};

/**
 * Dense uniform grid over ONE square region, rebuilt from scratch with a counting sort.
 *
 *   build   O(N + cells)   two linear passes over contiguous arrays, zero allocations after warm-up
 *   query   O(cells hit + agents in them)
 *
 * One grid per region rather than one global grid: a region is 128 m wide, so the grid is tiny (8x8 cells)
 * and cache resident, and the world size does not matter.
 */
class FCrowdCellGrid
{
public:
	void Init(const FVector2D& InMin, double InExtent, int32 InCellsPerSide)
	{
		Min = InMin;
		Extent = FMath::Max(InExtent, 1.0);
		CellsPerSide = FMath::Max(1, InCellsPerSide);
		InvCell = static_cast<double>(CellsPerSide) / Extent;
		CellStart.Init(0, CellsPerSide * CellsPerSide + 1);
		Agents.Reset();
		Order.Reset();
	}

	void BeginBuild()
	{
		Agents.Reset();
	}

	void Add(const FVector2D& Pos, const FVector2f& Vel, uint32 NetId)
	{
		FCrowdGridAgent& A = Agents.AddDefaulted_GetRef();
		A.Pos = Pos;
		A.Vel = Vel;
		A.NetId = NetId;
	}

	/** Counting sort of the collected agents into cell order. */
	void EndBuild()
	{
		const int32 NumCells = CellsPerSide * CellsPerSide;
		const int32 Num = Agents.Num();

		FMemory::Memzero(CellStart.GetData(), sizeof(int32) * (NumCells + 1));
		CellOfAgent.SetNumUninitialized(Num, EAllowShrinking::No);

		for (int32 i = 0; i < Num; ++i)
		{
			const int32 Cell = CellIndex(Agents[i].Pos);
			CellOfAgent[i] = Cell;
			++CellStart[Cell + 1];
		}
		for (int32 c = 0; c < NumCells; ++c)
		{
			CellStart[c + 1] += CellStart[c];
		}

		Cursor.SetNumUninitialized(NumCells, EAllowShrinking::No);
		FMemory::Memcpy(Cursor.GetData(), CellStart.GetData(), sizeof(int32) * NumCells);

		Order.SetNumUninitialized(Num, EAllowShrinking::No);
		for (int32 i = 0; i < Num; ++i)
		{
			Order[Cursor[CellOfAgent[i]]++] = i;
		}
	}

	/** Calls Fn(const FCrowdGridAgent&, double DistSquared) for every agent inside the circle. */
	template <typename FuncType>
	void ForEachInCircle(const FVector2D& Center, double Radius, FuncType&& Fn) const
	{
		if (Agents.Num() == 0)
		{
			return;
		}

		const double RadiusSq = Radius * Radius;
		const int32 MinCX = CellCoord(Center.X - Radius, Min.X);
		const int32 MaxCX = CellCoord(Center.X + Radius, Min.X);
		const int32 MinCY = CellCoord(Center.Y - Radius, Min.Y);
		const int32 MaxCY = CellCoord(Center.Y + Radius, Min.Y);

		for (int32 CY = MinCY; CY <= MaxCY; ++CY)
		{
			for (int32 CX = MinCX; CX <= MaxCX; ++CX)
			{
				const int32 Cell = CY * CellsPerSide + CX;
				for (int32 K = CellStart[Cell]; K < CellStart[Cell + 1]; ++K)
				{
					const FCrowdGridAgent& A = Agents[Order[K]];
					const double DistSq = FVector2D::DistSquared(A.Pos, Center);
					if (DistSq <= RadiusSq)
					{
						Fn(A, DistSq);
					}
				}
			}
		}
	}

	int32 Num() const { return Agents.Num(); }
	const TArray<FCrowdGridAgent>& GetAgents() const { return Agents; }

	void Clear()
	{
		Agents.Reset();
		Order.Reset();
		if (CellStart.Num() > 0)
		{
			FMemory::Memzero(CellStart.GetData(), sizeof(int32) * CellStart.Num());
		}
	}

private:
	FORCEINLINE int32 CellCoord(double V, double MinV) const
	{
		return FMath::Clamp(static_cast<int32>(FMath::FloorToInt((V - MinV) * InvCell)), 0, CellsPerSide - 1);
	}

	FORCEINLINE int32 CellIndex(const FVector2D& P) const
	{
		return CellCoord(P.Y, Min.Y) * CellsPerSide + CellCoord(P.X, Min.X);
	}

	FVector2D Min = FVector2D::ZeroVector;
	double Extent = 1.0;
	double InvCell = 1.0;
	int32 CellsPerSide = 1;

	TArray<FCrowdGridAgent> Agents;
	TArray<int32> CellStart; // size = cells + 1, prefix sums into Order
	TArray<int32> Order;     // agent indices sorted by cell
	TArray<int32> CellOfAgent;
	TArray<int32> Cursor;
};
