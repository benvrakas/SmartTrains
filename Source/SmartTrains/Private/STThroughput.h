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
	};
	enum ENodeFlag : uint8
	{
		BelowTarget = 1,
		Starved = 2,
		Full = 4,
		Empty = 8,
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
		// Latest rate, items per second, and stock and capacity.
		double Rate = 0.0;
		int32 Stock = 0;
		int32 Capacity = 0;
		TArray<FNodeSample> Samples;

		// Share of the samples with Flag set.
		double Share(uint8 Flag) const;
	};
	struct FStationSample
	{
		double Time = 0.0;
		bool bOccupied = false;
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
