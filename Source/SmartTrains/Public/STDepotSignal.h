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
class AFGTrain;
class ASTBuildableDepotSignal;
class UFGRailroadTrackConnectionComponent;

// The parking spot a depot signal marks, where one train waits (STDepots::Check): on a passing siding the track behind
// the signal, on a dead-end siding the track beyond it.
struct FSTDepotSiding
{
	// Where a parked train's front stops: the end of the spot's last track.
	UFGRailroadTrackConnectionComponent* Goal = nullptr;
	TArray<AFGBuildableRailroadTrack*> Tracks;
	// Length of the spot. [cm]
	float Length = 0.f;
	int32 TrackGraph = INDEX_NONE;
	// The signal faces into a dead-end siding: trains with a locomotive at each end drive in and back out.
	bool bDeadEnd = false;
	// The next depot signal on the same siding: toward the entrance (passing siding) or deeper in (dead end).
	const AFGBuildableRailroadSignal* Neighbour = nullptr;
	// The signal a parked train's front stops before (null: the track end). Without Auto Trains it is held at stop while
	// a train drives there, so the game's automatic control brakes for it.
	const AFGBuildableRailroadSignal* StopSignal = nullptr;
	// Why the spot can't be used; empty when it can.
	FString Problem;
};

// One parking spot per depot signal, linked along its siding (STDepots::Survey).
struct FSTDepotSpot
{
	ASTBuildableDepotSignal* Signal = nullptr;
	FSTDepotSiding Siding;
	// The spots next to it on its siding: toward the entrance, and deeper in (toward the exit or the track end).
	int32 Behind = INDEX_NONE;
	int32 Ahead = INDEX_NONE;
	// Trains with a vehicle on its tracks.
	TArray<const AFGTrain*> Trains;
};

namespace STDepots
{
	// Where a train stands with its drive to a depot spot.
	enum class EDriveState : uint8
	{
		// Auto Trains isn't loaded or has no such function.
		Unavailable,
		// No goal given (or it was dropped).
		None,
		UnderWay,
		// Standing at its goal.
		Arrived,
		// No way to the goal.
		NoWay,
	};

	// A train needs this much more siding than its own length. [cm]
	constexpr float Margin = 800.f;
	// A depot signal Auto Trains found no way to isn't offered to that train again for this long. [s]
	constexpr double NoWayRetrySeconds = 600.0;

	// Signal's parking spot. Facing into a dead end: the track beyond it to the next signal facing in, or the track end.
	// Otherwise (passing siding): back from it to the siding's entry, its switch or a signal. No station may lie on the
	// way. Without Auto Trains the game's signals keep trains apart: a passing siding needs an entry signal after its
	// switch, and a dead end a signal at its far end, facing in.
	SMARTTRAINS_API FSTDepotSiding Check(const AFGBuildableRailroadSignal* Signal);

	// Every depot signal's spot in World, linked to its neighbours on the same siding, with the trains standing on it.
	SMARTTRAINS_API TArray<FSTDepotSpot> Survey(UWorld* World);

	// Holds Signal at stop (bHold) or lets its block decide its aspect again (server).
	void Hold(AFGBuildableRailroadSignal* Signal, bool bHold);
	// True while Signal is held at stop.
	bool IsHeld(const AFGBuildableRailroadSignal* Signal);

	// One line for the look-at text and /st depots: "Depot: ready, 312 m of siding (trains up to 304 m)" or the problem.
	SMARTTRAINS_API FString Describe(const FSTDepotSiding& Siding);

	// The shortest route from Locomotive (driving the way it faces) to one of Goals: each track's exit connection in
	// order, the last being the goal reached (OutGoal). Empty if none within the search limit. OutLength: cm.
	TArray<UFGRailroadTrackConnectionComponent*> FindRoute(const AFGLocomotive* Locomotive, const TSet<const UFGRailroadTrackConnectionComponent*>& Goals,
		const UFGRailroadTrackConnectionComponent*& OutGoal, float& OutLength);

	// Points the depot signal's content classes at the vanilla block signal's mesh, hologram, icons, categories and
	// ingredients (game instance post-initialization).
	void ResolveVanillaAssets();

	// Server: makes the depot signal's recipe available once the vanilla block signal's is (it unlocks with it, in new
	// and existing saves).
	void KeepUnlocked(UWorld* World);
}

// A block signal that also marks the siding behind it as a depot: managed trains with nothing to do wait there
// (Docs/Architecture.md, "Depot signals"). Same mesh and behaviour as the vanilla block signal.
UCLASS()
class SMARTTRAINS_API ASTBuildableDepotSignal : public AFGBuildableRailroadSignal
{
	GENERATED_BODY()

public:
	virtual FText GetLookAtDecription_Implementation(AFGCharacterPlayer* ByCharacter, const FUseState& State) const override;
	// Every new depot signal takes the settings ResolveVanillaAssets put on the class default: actors of a native class
	// don't get the default's runtime changes by themselves.
	virtual void PostInitProperties() override;

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

// Kept so saves that bought the 0.17.0/0.18.0 AWESOME Shop entry load; no longer registered, so no shop lists it.
UCLASS()
class SMARTTRAINS_API USTUnlockRecipe_DepotSignal : public UFGUnlockRecipe
{
	GENERATED_BODY()

public:
	USTUnlockRecipe_DepotSignal();
};

// Kept so saves that bought the 0.17.0/0.18.0 AWESOME Shop entry load; no longer registered, so no shop lists it.
UCLASS()
class SMARTTRAINS_API USTSchematic_DepotSignal : public UFGSchematic
{
	GENERATED_BODY()

public:
	USTSchematic_DepotSignal();
};
