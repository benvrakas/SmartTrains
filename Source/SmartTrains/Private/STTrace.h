#pragma once

#include "CoreMinimal.h"

class AFGBuildableTrainPlatformCargo;
class UFGItemDescriptor;

// Follows a cargo platform's belt and pipe connections to the machines behind them and reads those machines' recipes,
// so an empty platform still gets its items (Docs/Migration.md, "Connection trace").
namespace STTrace
{
	enum class EConfidence : uint8
	{
		// One clean path to machines with a set recipe.
		High,
		// Through a merge, a split, a smart splitter, or cut short by a limit.
		Medium,
		// Nothing reached, an unknown recipe, or several fluids.
		Unclear,
	};

	struct FFlow
	{
		TSubclassOf<UFGItemDescriptor> Item;
		// Items (or cubic metres) per minute from the recipe and clock speed; 0 when the machine has no recipe rate.
		float PerMinute = 0.0f;
		// Reached through a splitter overflow output, whose share can't be known in advance.
		bool bShared = false;
		FString Path;
	};

	struct FResult
	{
		TArray<FFlow> Flows;
		// Fluid currently in the attached pipe network, if any: a hint only.
		TSubclassOf<UFGItemDescriptor> PipeFluidHint;
		EConfidence Confidence = EConfidence::Unclear;
		int32 Machines = 0;
		// The machines reached, for grouping platforms that serve the same machines.
		TArray<TWeakObjectPtr<const AActor>> MachineActors;
		// Train platforms reached (the walk stops at them).
		TArray<TWeakObjectPtr<const AActor>> PlatformActors;
		// The machine connections reached ("<machine path>|<connection name>"): platforms feeding different inputs of
		// the same machines carry different items (STMigration::ShareOutItems, lanes).
		TArray<FString> MachineInputs;
		// Machine items stopped by a splitter or balancer filter on the way.
		int32 Filtered = 0;
		int32 Nodes = 0;
		bool bBranched = false;
		bool bShared = false;
		bool bIncomplete = false;
		// Why the walk ended where it did, one line per distinct reason.
		TArray<FString> Notes;

		// Distinct items of Flows.
		TArray<TSubclassOf<UFGItemDescriptor>> Items() const;
		// "High: Wire x60/min via Constructor", for the preview and the CSV.
		FString Summary() const;
	};

	const TCHAR* ConfidenceName(EConfidence Confidence);

	// Buildings and belt segments one trace may visit: default, and the accepted range.
	constexpr int32 DefaultLimit = 5000;
	constexpr int32 MinLimit = 100;
	constexpr int32 MaxLimit = 500000;

	// Load platforms are traced upstream to what produces; unload platforms downstream to what consumes. Stops after
	// Limit buildings and belt segments.
	FResult Trace(const AFGBuildableTrainPlatformCargo* Platform, bool bLoadMode, int32 Limit = DefaultLimit);

	// What a flood fill through a factory reached: its machines (the starts first) and the train platforms where it
	// stopped, with the machine the walk last passed before each.
	struct FFlood
	{
		TArray<const AActor*> Machines;
		TArray<const AActor*> Platforms;
		TMap<const AActor*, TArray<const AActor*>> PlatformsByMachine;
		int32 Nodes = 0;
		// Stopped at Limit buildings and belt segments.
		bool bCut = false;
	};

	// One walk from Starts through belts, pipes and machines, downstream (outputs to inputs) or upstream, visiting each
	// building once and stopping at train platforms (Docs/Architecture.md, "Station set-up").
	FFlood Flood(const TArray<const AActor*>& Starts, bool bUpstream, int32 Limit);

	// What belts can carry from train platforms to the machines that use an item.
	struct FBeltCapacity
	{
		// Items per minute at most; -1 when no run of belts from the sources reaches a sink.
		double PerMinute = -1.0;
		// The narrowest set of belt runs on the way (a minimum cut): how many, and "2 belts at 780/min".
		int32 CutBelts = 0;
		FString Cut;
		// Stopped at Limit buildings and belt segments: the result counts what was reached.
		bool bCut = false;
	};

	// The most per minute belts carry from Sources (train platforms, out of their outputs) to Sinks (machines): a maximum
	// flow over the belt network downstream. Each run of belts and lifts is one link as fast as its slowest belt;
	// splitters, mergers, storage and balancers join links without a limit of their own; other machines and platforms
	// end the walk. Splitter filters aren't followed, so it is an upper bound (Docs/Architecture.md, "Station set-up").
	FBeltCapacity BeltCapacity(const TArray<const AActor*>& Sources, const TSet<const AActor*>& Sinks, int32 Limit);
}
