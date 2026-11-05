#pragma once

#include "CoreMinimal.h"

class AFGTrainStationIdentifier;
class UFGRailroadTrackConnectionComponent;

// Track distances between stations, found by a directed search over the track network from each station's departure
// end to every station's arrival end (Docs/Architecture.md, "Travel times"). One source station is searched per Step.
class FSTTravel
{
public:
	// Searches from the next station in Stations; restarts the round when the list changes or a round is done.
	void Step(const TArray<AFGTrainStationIdentifier*>& Stations);

	// Track distance in cm a train drives from From to To; -1 if From hasn't been searched yet, -2 if To is unreachable. bReversing is set
	// when the route starts by backing out over From's platforms (push-pull trains only). bPushPull also allows
	// arriving at To from its far end, station building first, which only a train with a locomotive at each end can dock.
	float GetDistance(const AFGTrainStationIdentifier* From, const AFGTrainStationIdentifier* To, bool* bReversing = nullptr, bool bPushPull = false) const;

	// Number of source stations searched at least once.
	int32 GetSearchedCount() const { return mRows.Num(); }

	// Forgets everything, e.g. when the world changes.
	void Reset();

private:
	struct FRow
	{
		// Arriving through the platform end (any train).
		TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, float> Forward;
		TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, float> Reverse;
		// Arriving through the far end, station building first (push-pull trains).
		TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, float> ForwardBack;
		TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, float> ReverseBack;
	};

	// Runs the search from Source's departure through Exit, recording arrival distances through the platform ends into
	// Out and through the far ends into OutBack.
	static void Search(UFGRailroadTrackConnectionComponent* Exit, const TMap<UFGRailroadTrackConnectionComponent*, AFGTrainStationIdentifier*>& Arrivals,
		const TMap<UFGRailroadTrackConnectionComponent*, AFGTrainStationIdentifier*>& BackArrivals, const TMap<AFGTrainStationIdentifier*, float>& ArrivalExtra,
		TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, float>& Out, TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, float>& OutBack);

	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, FRow> mRows;
	TArray<TWeakObjectPtr<AFGTrainStationIdentifier>> mOrder;
	int32 mNext = 0;
};
