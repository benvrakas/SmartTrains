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
	constexpr int32 MaxLimit = 100000;

	// Load platforms are traced upstream to what produces; unload platforms downstream to what consumes. Stops after
	// Limit buildings and belt segments.
	FResult Trace(const AFGBuildableTrainPlatformCargo* Platform, bool bLoadMode, int32 Limit = DefaultLimit);

	// Walks from a machine's inputs upstream (bUpstream) or its outputs downstream to the next machines and train
	// platforms (Docs/Architecture.md, "Station set-up").
	FResult TraceFromMachine(const AActor* Machine, bool bUpstream, int32 Limit = DefaultLimit);
}
