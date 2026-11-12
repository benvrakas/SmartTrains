#include "STForecast.h"
#include "STSubsystem.h"

namespace
{
	// A nearly full supplier is assumed to fill at least this many car loads per second (one per half hour).
	constexpr double MinFullSupplyLoadsPerSecond = 1.0 / 1800.0;
	// Share of capacity from which a supplier counts as nearly full.
	constexpr double NearlyFullShare = 0.9;

	// Integral over [0, Duration] of max(0, Target - x(t)) with x(t) = max(0, Start - Use * t).
	double ShortfallIntegral(double Start, double Use, double Target, double Duration)
	{
		if (Duration <= 0.0 || Target <= 0.0)
		{
			return 0.0;
		}
		if (Use <= 0.0)
		{
			return FMath::Max(0.0, Target - Start) * Duration;
		}
		const double ToTarget = FMath::Clamp((Start - Target) / Use, 0.0, Duration);
		const double ToEmpty = FMath::Clamp(Start / Use, ToTarget, Duration);
		// Between reaching the target and running empty the shortfall grows linearly; after that it is the whole target.
		const double Growing = ToEmpty - ToTarget;
		const double AtTarget = FMath::Max(0.0, Target - FMath::Max(0.0, Start - Use * ToTarget));
		const double Rising = Growing * (AtTarget + 0.5 * Use * Growing);
		return Rising + Target * (Duration - ToEmpty);
	}

	// Production lost over [0, Duration] while x(t) = min(Capacity, Start + Rate * t) is at capacity.
	double LostProduction(double Start, double Rate, double Capacity, double Duration)
	{
		if (Duration <= 0.0 || Rate <= 0.0)
		{
			return 0.0;
		}
		const double ToFull = FMath::Max(0.0, (Capacity - Start) / Rate);
		return Rate * FMath::Max(0.0, Duration - ToFull);
	}

	struct FWalk
	{
		double Cost = 0.0;
		double Deadline = 0.0;
	};

	// Walks Node's stock from Now through Events (sorted here) to the horizon's end, or to Until if that is sooner.
	FWalk Walk(const FSTNode& Node, TArray<FSTNodeEvent> Events, double Now, double CarCapacity, double Until = 0.0)
	{
		FWalk Out;
		const double End = Until > Now ? FMath::Min(Until, Now + STForecast::HorizonSeconds) : Now + STForecast::HorizonSeconds;
		Out.Deadline = End;
		const bool bDemand = Node.Role == ESTRole::Demand;
		const double Capacity = FMath::Max(1.0, static_cast<double>(Node.Capacity));
		const double Target = Capacity * Node.TargetPercent / 100.0;
		const double Rate = STForecast::GetRate(Node, CarCapacity);
		Events.Sort([](const FSTNodeEvent& A, const FSTNodeEvent& B) { return A.Time < B.Time; });
		double Stock = FMath::Clamp(static_cast<double>(Node.Stock), 0.0, Capacity);
		double Clock = Now;
		bool bDeadline = false;
		auto Advance = [&](double Until)
		{
			const double Span = FMath::Min(Until, End) - Clock;
			if (Span <= 0.0)
			{
				return;
			}
			if (bDemand)
			{
				if (!bDeadline && Stock - Rate * Span < Target)
				{
					bDeadline = true;
					Out.Deadline = Clock + (Stock <= Target ? 0.0 : (Stock - Target) / FMath::Max(Rate, 1e-9));
				}
				Out.Cost += ShortfallIntegral(Stock, Rate, Target, Span);
				Stock = FMath::Max(0.0, Stock - Rate * Span);
			}
			else
			{
				if (!bDeadline && Stock + Rate * Span >= Capacity)
				{
					bDeadline = true;
					Out.Deadline = Clock + FMath::Max(0.0, (Capacity - Stock) / FMath::Max(Rate, 1e-9));
				}
				Out.Cost += LostProduction(Stock, Rate, Capacity, Span);
				Stock = FMath::Min(Capacity, Stock + Rate * Span);
			}
			Clock += Span;
		};
		for (const FSTNodeEvent& Event : Events)
		{
			if (Event.Time >= End)
			{
				break;
			}
			Advance(Event.Time);
			Stock = bDemand ? FMath::Min(Capacity, Stock + Event.Amount) : FMath::Max(0.0, Stock - Event.Amount);
		}
		Advance(End);
		// Car-load minutes; demand weighted by priority.
		const double Weight = bDemand ? STNames::PriorityWeight(Node.Priority) * Node.CostScale / 60.0 : STForecast::LostLoadMinutes;
		Out.Cost = Weight * Out.Cost / FMath::Max(1.0, CarCapacity);
		return Out;
	}
}

double STForecast::GetRate(const FSTNode& Node, double CarCapacity)
{
	const double Rate = FMath::Max(0.0, Node.Rate);
	if (Node.Role == ESTRole::Supply && Node.Capacity > 0 && Node.Stock >= NearlyFullShare * Node.Capacity)
	{
		return FMath::Max(Rate, MinFullSupplyLoadsPerSecond * FMath::Max(1.0, CarCapacity));
	}
	return Rate;
}

double STForecast::Cost(const FSTNode& Node, const TArray<FSTNodeEvent>& Events, double Now, double CarCapacity, double Until)
{
	if (Node.bAnyItem || Node.Capacity <= 0 || (Node.Role != ESTRole::Demand && Node.Role != ESTRole::Supply))
	{
		return 0.0;
	}
	return Walk(Node, Events, Now, CarCapacity, Until).Cost;
}

double STForecast::Gain(const FSTNode& Node, const TArray<FSTNodeEvent>& Events, const FSTNodeEvent& Added, double Now, double CarCapacity, double Until)
{
	TArray<FSTNodeEvent> With = Events;
	With.Add(Added);
	return FMath::Max(0.0, Cost(Node, Events, Now, CarCapacity, Until) - Cost(Node, With, Now, CarCapacity, Until));
}

double STForecast::NextDeliveryTime(double Now, double Arrive)
{
	return Arrive + FMath::Max(Arrive - Now, NextDeliveryMinSeconds);
}

double STForecast::Deadline(const FSTNode& Node, const TArray<FSTNodeEvent>& Events, double Now, double CarCapacity)
{
	if (Node.bAnyItem || Node.Capacity <= 0)
	{
		return Now + HorizonSeconds;
	}
	return Walk(Node, Events, Now, CarCapacity).Deadline;
}
