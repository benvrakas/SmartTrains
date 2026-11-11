#pragma once

#include "CoreMinimal.h"
#include "Buildables/FGBuildableRailroadSignal.h"
#include "FGRecipe.h"
#include "FGSchematic.h"
#include "Hologram/FGHologram.h"
#include "Resources/FGBuildingDescriptor.h"
#include "Unlocks/FGUnlockRecipe.h"
#include "STDepotSignal.generated.h"

class AFGBuildableRailroadTrack;
class AFGLocomotive;
class UFGRailroadTrackConnectionComponent;

// The track behind a depot signal, where a train waits (STDepots::Check).
struct FSTDepotSiding
{
	// Where a parked train's front stops: the end of the track just before the signal.
	UFGRailroadTrackConnectionComponent* Goal = nullptr;
	TArray<AFGBuildableRailroadTrack*> Tracks;
	// Track length from the signal back to the siding's entry signal. [cm]
	float Length = 0.f;
	int32 TrackGraph = INDEX_NONE;
	// Why the siding can't be used; empty when it can.
	FString Problem;
};

namespace STDepots
{
	// A train needs this much more siding than its own length. [cm]
	constexpr float Margin = 800.f;

	// Walks back from Signal to the siding's entry signal: no switch or station may lie between them.
	SMARTTRAINS_API FSTDepotSiding Check(const AFGBuildableRailroadSignal* Signal);

	// One line for the look-at text and /st depots: "Depot: ready, 312 m of siding (trains up to 304 m)" or the problem.
	SMARTTRAINS_API FString Describe(const FSTDepotSiding& Siding);

	// The shortest route from Locomotive (driving the way it faces) to one of Goals: each track's exit connection in
	// order, the last being the goal reached (OutGoal). Empty if none within the search limit. OutLength: cm.
	TArray<UFGRailroadTrackConnectionComponent*> FindRoute(const AFGLocomotive* Locomotive, const TSet<const UFGRailroadTrackConnectionComponent*>& Goals,
		const UFGRailroadTrackConnectionComponent*& OutGoal, float& OutLength);

	// Points the depot signal's content classes at the vanilla block signal's mesh, hologram, icons, categories and
	// ingredients (game instance post-initialization).
	void ResolveVanillaAssets();
}

// A block signal that also marks the siding behind it as a depot: managed trains with nothing to do wait there
// (Docs/Architecture.md, "Depot signals"). Same mesh and behaviour as the vanilla block signal.
UCLASS()
class SMARTTRAINS_API ASTBuildableDepotSignal : public AFGBuildableRailroadSignal
{
	GENERATED_BODY()

public:
	virtual FText GetLookAtDecription_Implementation(AFGCharacterPlayer* ByCharacter, const FUseState& State) const override;

	// Copies the vanilla block signal's settings and mesh onto this class's defaults.
	static void ResolveVanillaAssets();
};

UCLASS()
class SMARTTRAINS_API USTDesc_DepotSignal : public UFGBuildingDescriptor
{
	GENERATED_BODY()

public:
	USTDesc_DepotSignal();

	// Copies the vanilla block signal descriptor's icons, build menu category and order.
	static void ResolveVanillaAssets();
};

UCLASS()
class SMARTTRAINS_API USTRecipe_DepotSignal : public UFGRecipe
{
	GENERATED_BODY()

public:
	USTRecipe_DepotSignal();

	// Copies the vanilla block signal recipe's ingredients and where it is built.
	static void ResolveVanillaAssets();
};

UCLASS()
class SMARTTRAINS_API USTUnlockRecipe_DepotSignal : public UFGUnlockRecipe
{
	GENERATED_BODY()

public:
	USTUnlockRecipe_DepotSignal();
};

// AWESOME Shop entry that unlocks the depot signal.
UCLASS()
class SMARTTRAINS_API USTSchematic_DepotSignal : public UFGSchematic
{
	GENERATED_BODY()

public:
	USTSchematic_DepotSignal();

	// Sets the coupon cost, shop category and icon from vanilla assets.
	static void ResolveVanillaAssets();
};
