#pragma once

#include "CoreMinimal.h"
#include "STViews.h"

class AFGTrainStationIdentifier;
class ASTSubsystem;

// The throughput monitor behind "/st monitor": samples the fleet, the platforms and the stations, and judges per item
// what limits deliveries over the last half hour (Docs/Architecture.md, "Throughput monitor").
class FSTThroughput
{
public:
	// How often the monitor samples, and how far back it judges. [s]
	static constexpr double SampleSeconds = 10.0;
	static constexpr double WindowSeconds = 30.0 * 60.0;

	void Sample(const ASTSubsystem& Subsystem);
	// A managed train docked for a trip leg, Late seconds after its planned arrival.
	void RecordArrival(const AFGTrainStationIdentifier* Station, double Late, double Time);

	FSTMonitorView MakeView(const ASTSubsystem& Subsystem) const;
	// The view as text lines, for chat, the server console and the log.
	static TArray<FString> Describe(const FSTMonitorView& View);

private:
	struct FFleetSample
	{
		double Time = 0.0;
		int32 Managed = 0;
		int32 Driving = 0;
		int32 Docked = 0;
		int32 Parking = 0;
		int32 Idle = 0;
		// Running their last job again: free for new trips.
		int32 Repeating = 0;
	};
	enum ENodeFlag : uint8
	{
		BelowTarget = 1,
		Starved = 2,
		Full = 4,
		Empty = 8,
		// Demand: a train is on its way with a delivery for it.
		OnTheWay = 16,
		// Demand: every one of its platforms is empty (Starved: at least one has been empty for 30 s).
		AllEmpty = 32,
	};
	struct FNodeSample
	{
		double Time = 0.0;
		uint8 Flags = 0;
	};
	struct FNodeHistory
	{
		TWeakObjectPtr<AFGTrainStationIdentifier> Station;
		TWeakObjectPtr<UClass> Item;
		ESTRole Role = ESTRole::Off;
		int32 Platforms = 0;
		// Its platforms (the station's platforms set to this item and role), at the last sample.
		TArray<TWeakObjectPtr<AFGBuildableTrainPlatformCargo>> PlatformList;
		// Latest rate, items per second, and stock and capacity.
		double Rate = 0.0;
		int32 Stock = 0;
		int32 Capacity = 0;
		TArray<FNodeSample> Samples;

		// Share of the samples with Flag set.
		double Share(uint8 Flag) const;
		// Share of the samples with Within set that also have Flag set (0 if none has Within).
		double ShareOf(uint8 Flag, uint8 Within) const;
	};
	struct FStationSample
	{
		double Time = 0.0;
		bool bOccupied = false;
		// Managed trains on their way to the station (its stop is their current leg), not docked there.
		int32 Heading = 0;
	};
	struct FArrival
	{
		double Time = 0.0;
		double Late = 0.0;
	};

	// Whether a managed train can serve a demand node: reach it from a supplier of the item, and fit at both ends.
	struct FServe
	{
		bool bReach = false;
		bool bFits = false;
		FString Problem;
		double CheckedAt = -1000.0;
	};

	TArray<FFleetSample> mFleet;
	// Per demand node key, checked a few per sample, oldest first.
	TMap<FString, FServe> mServe;
	TMap<FString, FNodeHistory> mNodes;
	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, TArray<FStationSample>> mOccupied;
	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, TArray<FArrival>> mArrivals;
};
