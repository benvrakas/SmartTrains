#include "STLoopMath.h"

TArray<int32> STLoopMath::SplitByPriority(const TArray<double>& Needs, const TArray<uint8>& Tiers, const TArray<int32>& Caps, int32 Trains)
{
	TArray<int32> Out;
	Out.Init(0, Needs.Num());
	TArray<int32> Full;
	Full.Init(0, Needs.Num());
	for (int32 Index = 0; Index < Needs.Num(); ++Index)
	{
		const int32 Cap = Caps.IsValidIndex(Index) ? Caps[Index] : MAX_int32;
		Full[Index] = Needs[Index] > 0.0 && Cap > 0 ? FMath::Clamp(FMath::CeilToInt(Needs[Index] - 0.05), 1, Cap) : 0;
	}
	int32 Left = FMath::Max(0, Trains);
	for (int32 Tier = 2; Tier >= 0 && Left > 0; --Tier)
	{
		TArray<int32> Members;
		int32 Wanted = 0;
		for (int32 Index = 0; Index < Needs.Num(); ++Index)
		{
			if (Full[Index] > 0 && (Tiers.IsValidIndex(Index) ? Tiers[Index] : 1) == Tier)
			{
				Members.Add(Index);
				Wanted += Full[Index];
			}
		}
		if (Members.Num() == 0)
		{
			continue;
		}
		if (Left >= Wanted)
		{
			for (const int32 Index : Members)
			{
				Out[Index] = Full[Index];
			}
			Left -= Wanted;
			continue;
		}
		// Short: this tier shares what is left by full need, and the lower tiers get none.
		TArray<double> Weights;
		for (const int32 Index : Members)
		{
			Weights.Add(Full[Index]);
		}
		const TArray<int32> Shares = Split(Weights, Left);
		for (int32 Member = 0; Member < Members.Num(); ++Member)
		{
			Out[Members[Member]] = Shares[Member];
		}
		return Out;
	}
	// Every full need met: the rest by need to consumers that more trains still fit.
	while (Left > 0)
	{
		TArray<double> Weights;
		TArray<int32> Room;
		int32 Open = 0;
		for (int32 Index = 0; Index < Needs.Num(); ++Index)
		{
			const int32 Cap = Caps.IsValidIndex(Index) ? Caps[Index] : MAX_int32;
			Room.Add(Needs[Index] > 0.0 ? FMath::Max(0, Cap - Out[Index]) : 0);
			Weights.Add(Room.Last() > 0 ? Needs[Index] : 0.0);
			Open += Room.Last() > 0 ? 1 : 0;
		}
		if (Open == 0)
		{
			break;
		}
		const TArray<int32> Extra = Split(Weights, Left);
		int32 Given = 0;
		for (int32 Index = 0; Index < Needs.Num(); ++Index)
		{
			const int32 Take = FMath::Min(Extra[Index], Room[Index]);
			Out[Index] += Take;
			Given += Take;
		}
		if (Given == 0)
		{
			break;
		}
		Left -= Given;
	}
	return Out;
}

TArray<int32> STLoopMath::Split(const TArray<double>& Weights, int32 Trains)
{
	TArray<int32> Out;
	Out.Init(0, Weights.Num());
	TArray<int32> Positive;
	double Total = 0.0;
	for (int32 Index = 0; Index < Weights.Num(); ++Index)
	{
		if (Weights[Index] > 0.0)
		{
			Positive.Add(Index);
			Total += Weights[Index];
		}
	}
	if (Trains <= 0 || Positive.Num() == 0)
	{
		return Out;
	}
	// Larger weight first, then lower index.
	auto ByWeight = [&Weights](int32 A, int32 B) { return Weights[A] != Weights[B] ? Weights[A] > Weights[B] : A < B; };
	if (Trains < Positive.Num())
	{
		Positive.Sort(ByWeight);
		for (int32 Rank = 0; Rank < Trains; ++Rank)
		{
			Out[Positive[Rank]] = 1;
		}
		return Out;
	}
	// One each, then the rest by largest remainder over what each quota asks beyond its one.
	TArray<double> Extra;
	double ExtraTotal = 0.0;
	for (const int32 Index : Positive)
	{
		Out[Index] = 1;
		const double Quota = Weights[Index] / Total * Trains;
		Extra.Add(FMath::Max(0.0, Quota - 1.0));
		ExtraTotal += Extra.Last();
	}
	if (ExtraTotal <= 0.0)
	{
		for (int32 Slot = 0; Slot < Positive.Num(); ++Slot)
		{
			Extra[Slot] = Weights[Positive[Slot]];
		}
		ExtraTotal = Total;
	}
	const int32 Rest = Trains - Positive.Num();
	TArray<TPair<double, int32>> Remainders;
	int32 Given = 0;
	for (int32 Slot = 0; Slot < Positive.Num(); ++Slot)
	{
		const double Share = Extra[Slot] / ExtraTotal * Rest;
		const int32 Whole = FMath::FloorToInt(Share + 1e-9);
		Out[Positive[Slot]] += Whole;
		Given += Whole;
		Remainders.Emplace(Share - Whole, Positive[Slot]);
	}
	Remainders.Sort([&](const TPair<double, int32>& A, const TPair<double, int32>& B)
	{
		return FMath::Abs(A.Key - B.Key) > 1e-9 ? A.Key > B.Key : ByWeight(A.Value, B.Value);
	});
	for (int32 Rank = 0; Given < Rest && Rank < Remainders.Num(); ++Rank, ++Given)
	{
		++Out[Remainders[Rank].Value];
	}
	return Out;
}

TArray<double> STLoopMath::Allocate(double Need, TArray<FOffer>& Offers)
{
	TArray<double> Taken;
	Taken.Init(0.0, Offers.Num());
	TArray<int32> Order;
	for (int32 Index = 0; Index < Offers.Num(); ++Index)
	{
		Order.Add(Index);
	}
	Order.Sort([&Offers](int32 A, int32 B) { return Offers[A].Distance != Offers[B].Distance ? Offers[A].Distance < Offers[B].Distance : A < B; });
	double Left = FMath::Max(0.0, Need);
	for (const int32 Index : Order)
	{
		if (Left <= 0.0)
		{
			break;
		}
		FOffer& Offer = Offers[Index];
		const double Take = Offer.Spare < 0.0 ? Left : FMath::Min(Left, Offer.Spare);
		if (Take <= 0.0)
		{
			continue;
		}
		Taken[Index] += Take;
		Left -= Take;
		if (Offer.Spare >= 0.0)
		{
			Offer.Spare -= Take;
		}
	}
	if (Left > 0.0 && Order.Num() > 0)
	{
		Taken[Order[0]] += Left;
	}
	return Taken;
}
