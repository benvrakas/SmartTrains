#pragma once

#include "CoreMinimal.h"

// The arithmetic of the loop plan (Docs/Architecture.md, "Loops"), apart from the game so the tests can reach it.
namespace STLoopMath
{
	// Whole trains per weight in proportion to the weights, by largest remainder. With at least as many trains as
	// positive weights, every positive weight gets at least one; with fewer, the largest weights get one each. Weights
	// of 0 or less get none. Ties go to the larger weight, then to the lower index.
	TArray<int32> Split(const TArray<double>& Weights, int32 Trains);

	// Whole trains per consumer by priority (Tiers: 0 Low, 1 Medium, 2 High). A consumer's full need is its need
	// rounded up, at least one and at most Caps (the trains that fit it). The tiers take turns from High down: a tier
	// whose full needs the trains left cover gets them; otherwise it splits what is left in proportion to its full
	// needs (Split) and the lower tiers get none. Trains left once every tier has its full need go by need to consumers
	// below their cap. Needs of 0 or less get none.
	TArray<int32> SplitByPriority(const TArray<double>& Needs, const TArray<uint8>& Tiers, const TArray<int32>& Caps, int32 Trains);

	// A supplier's offer of one item: how far it is and how much of its rate is still free (below 0: not known, it
	// counts as having enough).
	struct FOffer
	{
		double Distance = 0.0;
		double Spare = -1.0;
	};

	// Takes Need from the nearest offer with spare rate, then the next nearest for the rest; an offer whose spare isn't
	// known takes all that is left. What no offer has room for goes to the nearest offer anyway. Returns the amount
	// taken from each offer, in Offers' order, and lowers their Spare.
	TArray<double> Allocate(double Need, TArray<FOffer>& Offers);
}
