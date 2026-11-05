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
		// Reached through a smart splitter, whose filters the trace does not map to outputs.
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

	// Load platforms are traced upstream to what produces; unload platforms downstream to what consumes.
	FResult Trace(const AFGBuildableTrainPlatformCargo* Platform, bool bLoadMode);
}
