#pragma once

#include "CoreMinimal.h"

class AFGBuildableTrainPlatformCargo;
class AFGFreightWagon;
class AFGTrainStationIdentifier;
class ASTSubsystem;
struct FSTJobLeg;
struct FSTNode;
struct FSTTrainState;

// One stop of a trip being planned: the station and the items moved there.
struct FSTPlanStop
{
	AFGTrainStationIdentifier* Station = nullptr;
	TArray<UClass*> LoadItems;
	TArray<UClass*> UnloadItems;
};

// What one car does at one stop of a simulated trip.
struct FSTPlanCarStep
{
	bool bSkip = false;
	TArray<TPair<UClass*, int32>> Loaded;
	TArray<TPair<UClass*, int32>> Unloaded;
};

// A simulated trip.
struct FSTPlanResult
{
	bool bValid = false;
	FString Problem;
	double Value = 0.0;
	// Value per second until the trip ends.
	double Score = 0.0;
	double EndTime = 0.0;
	int32 Moved = 0;
	TArray<double> Arrive;
	// [stop][car]
	TArray<TArray<FSTPlanCarStep>> Steps;
};

// A move the planner may add to a trip: from a supplier (or cargo already on the train) to a consumer.
struct FSTPlanMove
{
	// Supply node, or INDEX_NONE for cargo already on the train.
	int32 From = INDEX_NONE;
	int32 To = INDEX_NONE;
	UClass* Item = nullptr;
	// Score of a trip made of this move alone (0 if that trip doesn't work by itself).
	double SoloScore = 0.0;

	bool operator==(const FSTPlanMove& Other) const { return From == Other.From && To == Other.To && Item == Other.Item; }
};

// Plans platform-to-platform trips for managed trains (Docs/Architecture.md, "Trip planning"): a trip is a list of
// stops with the items loaded and unloaded at each; a simulation works out what every car does there, following
// where each car stops beside which platform. Moves are added by regret insertion while the trip's value per second
// rises.
class FSTTripPlanner
{
public:
	// BusyUntil: per station, (train index, booked until) of the jobs already running.
	FSTTripPlanner(const ASTSubsystem& InSubsystem, TArray<FSTNode>& InNodes, TMap<const AFGTrainStationIdentifier*, TArray<TPair<int32, double>>>& InBusyUntil);

	// Sets the train the next calls plan for and lists its moves. Returns false if it has no freight cars.
	// bAnchorDocked: the train is at Anchor now (docked there, or just left it), so Anchor can't be the first stop.
	bool BeginTrain(int32 TrainIndex, const AFGTrainStationIdentifier* Anchor, double FreeAt, bool bAnchorDocked);
	const TArray<FSTPlanMove>& GetMoves() const { return mMoves; }

	FSTPlanResult Simulate(const TArray<FSTPlanStop>& Stops) const;

	// Inserts Move at its best place from stop FirstMovable on. Returns false if no placement works.
	// OutSecond: score of the second best placement (0 if none).
	bool InsertBest(TArray<FSTPlanStop>& InOutStops, const FSTPlanMove& Move, int32 FirstMovable, FSTPlanResult& OutResult, double& OutSecond) const;

	// Adds moves by regret insertion while the trip's score rises; returns the final result.
	FSTPlanResult Improve(TArray<FSTPlanStop>& InOutStops, int32 FirstMovable) const;

	// Drops items that move nothing and stops left with nothing to do, from FirstMovable on.
	static void Trim(TArray<FSTPlanStop>& InOutStops, const FSTPlanResult& Result, int32 FirstMovable);

	// Turns Stops and their simulation into job legs.
	void MakeLegs(const TArray<FSTPlanStop>& Stops, const FSTPlanResult& Result, TArray<FSTJobLeg>& OutLegs) const;

	// Adds the trip's amounts to the node reservations and its stays to the station bookings, for the trains planned
	// after it.
	void Reserve(const TArray<FSTPlanStop>& Stops, const FSTPlanResult& Result);

	// Takes the planned amounts of Legs (from index From on) out of the node reservations, before they are re-planned.
	void Release(const TArray<FSTJobLeg>& Legs, int32 From);

	// One line per stop: station, items, arrival.
	FString Describe(const TArray<FSTPlanStop>& Stops, const FSTPlanResult& Result) const;

	// Lets the current train's trip run at least Seconds (for re-plans of trips under way); reset by BeginTrain.
	void RaiseTripLimit(double Seconds);
	// Back to the setting's limit, or AtLeast if that is longer.
	void ResetTripLimit(double AtLeast);

	// True when this round's planning time is used up.
	bool IsOverBudget() const;
	// Milliseconds spent since the planner was made.
	double GetElapsedMs() const;
	// Why the last trip tried for the current train didn't work, empty if none failed.
	const FString& GetLastProblem() const { return mLastProblem; }

private:
	struct FPlatformInfo
	{
		const AFGBuildableTrainPlatformCargo* Platform = nullptr;
		bool bConfigured = false;
		bool bSupply = false;
		bool bDemand = false;
		bool bAnyItem = false;
		bool bLoadMode = false;
		bool bFluid = false;
		TArray<UClass*> Items;
		// Items in its inventory now, with counts.
		TMap<UClass*, int32> Stock;
		int32 Slots = 0;
		int32 Occupied = 0;
	};
	struct FCarInfo
	{
		AFGFreightWagon* Car = nullptr;
		int32 Slots = 0;
		TArray<TPair<UClass*, double>> Cargo;
	};

	int32 GetPlatformInfo(const AFGBuildableTrainPlatformCargo* Platform) const;
	// [map][car] platform info index (INDEX_NONE where no cargo platform is beside the car) at Station.
	const TArray<TArray<int32>>& GetCarPlatforms(const AFGTrainStationIdentifier* Station) const;
	int32 FindNode(const AFGTrainStationIdentifier* Station, UClass* Item, bool bSupply) const;
	double Busy(const AFGTrainStationIdentifier* Station) const;
	double GetAvailable(int32 Node, double Arrive) const;
	// Room below capacity at Arrive, and whether the consumer wants more (below target).
	double GetRoom(int32 Node, double Arrive, bool& bOutWants) const;
	double GetUrgency(int32 Node, double Arrive) const;
	// Car's room for Item with Cargo on board.
	double GetCarRoom(const FCarInfo& Car, const TArray<TPair<UClass*, double>>& Cargo, UClass* Item) const;
	bool IsSupplyFull(int32 Node) const;
	FSTPlanResult SimulateWithSkips(const TArray<FSTPlanStop>& Stops, TSet<TPair<int32, int32>>& InOutSkips) const;

	const ASTSubsystem& mSubsystem;
	TArray<FSTNode>& mNodes;
	TMap<const AFGTrainStationIdentifier*, TArray<TPair<int32, double>>>& mBusyUntil;
	TMap<TTuple<const AFGTrainStationIdentifier*, UClass*, bool>, int32> mNodeIndex;
	TMap<const AFGTrainStationIdentifier*, int32> mAnyNode;
	double mTime = 0.0;
	double mStarted = 0.0;
	double mDeadline = 0.0;
	mutable FString mLastProblem;
	bool bFcc = false;
	int32 mMaxStops = 4;
	double mMaxTripSeconds = 1200.0;

	// Current train.
	int32 mTrainIndex = INDEX_NONE;
	const AFGTrainStationIdentifier* mAnchor = nullptr;
	double mFreeAt = 0.0;
	bool bAnchorDocked = false;
	bool bRelaxed = false;
	// The current train has a locomotive at each end (travel estimates may reverse and arrive from a far end).
	bool bPushPull = false;
	TArray<FCarInfo> mCars;
	TArray<FSTPlanMove> mMoves;

	mutable TArray<FPlatformInfo> mPlatforms;
	mutable TMap<const AFGBuildableTrainPlatformCargo*, int32> mPlatformIndex;
	mutable TMap<const AFGTrainStationIdentifier*, TArray<TArray<int32>>> mCarPlatforms;
};

namespace STAssignment
{
	// Assigns rows to columns (each at most once) for the largest total Value; returns each row's column, or
	// INDEX_NONE where a row gets nothing worth more than 0. Hungarian method, O(rows^2 x columns).
	TArray<int32> Solve(const TArray<TArray<double>>& Value);
}
