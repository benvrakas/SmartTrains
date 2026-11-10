#pragma once

#include "CoreMinimal.h"

class AFGBuildableTrainPlatformCargo;
class AFGFreightWagon;
class AFGRailroadVehicle;
class AFGTrain;
class AFGTrainStationIdentifier;
class ASTSubsystem;
struct FSTJobLeg;
class UFGInventoryComponent;
struct FSTNode;
struct FSTTrainState;

// A train's planned time at a station: from its arrival to the end of its stay.
struct FSTBooking
{
	int32 Train = INDEX_NONE;
	double From = 0.0;
	double Until = 0.0;
};

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
	// The platform the car loads from or unloads into (the first way the train may dock), if it does.
	const AFGBuildableTrainPlatformCargo* Platform = nullptr;
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
	// Automation tests set planner situations up without a game world (Private/Tests).
	friend struct FSTTestAccess;

public:
	// InBookings: per station, the bookings of the jobs already running; Reserve adds the planned trip's.
	FSTTripPlanner(const ASTSubsystem& InSubsystem, TArray<FSTNode>& InNodes, TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>>& InBookings);

	// Sets the train the next calls plan for and lists its moves. Returns false if it has no freight cars.
	// bAnchorDocked: the train is at Anchor now (docked there, or just left it), so Anchor can't be the first stop.
	bool BeginTrain(int32 TrainIndex, const AFGTrainStationIdentifier* Anchor, double FreeAt, bool bAnchorDocked);
	// Makes TrainIndex the current train again as BeginTrain set it up earlier with the same anchor and time, without
	// listing and scoring its moves again. Returns false if it wasn't set up that way this round.
	bool ResumeTrain(int32 TrainIndex, const AFGTrainStationIdentifier* Anchor, double FreeAt, bool bAnchorDocked);
	const TArray<FSTPlanMove>& GetMoves() const { return mMoves; }

	FSTPlanResult Simulate(const TArray<FSTPlanStop>& Stops) const;

	// Inserts Move at its best place from stop FirstMovable on. Returns false if no placement works.
	// OutSecond: score of the second best placement (0 if none).
	bool InsertBest(TArray<FSTPlanStop>& InOutStops, const FSTPlanMove& Move, int32 FirstMovable, FSTPlanResult& OutResult, double& OutSecond) const;

	// Adds moves by regret insertion while the trip's score rises; returns the final result.
	FSTPlanResult Improve(TArray<FSTPlanStop>& InOutStops, int32 FirstMovable) const;
	// While set, Improve only adds moves delivering to Station (a trip built around one consumer); null: any move.
	void SetOnlyStation(const AFGTrainStationIdentifier* Station) { mOnlyStation = Station; }
	// The stations the current train's moves deliver to, each once.
	TArray<const AFGTrainStationIdentifier*> GetConsumerStations() const;
	// The current train's cars and start only (no moves): enough to simulate given stops.
	bool PrepareCars(int32 TrainIndex, const AFGTrainStationIdentifier* Anchor, double FreeAt, bool bInAnchorDocked);
	// The current train's planning inputs (cars, platforms, dock maps, start), for /st snapshot.
	TSharedRef<class FJsonObject> DescribeForReplay() const;
	const TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>>& GetBookings() const { return mBookings; }

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
	// Lets the current train's trip have at least Stops stops (the rest of a trip under way);
	// reset by BeginTrain, ResumeTrain and PrepareCars.
	void RaiseStopLimit(int32 Stops);
	// Lets the current train's trip wait for a busy station beyond the queue and waiting-room limits (the rest of a trip
	// under way, whose cargo is on board); reset by BeginTrain, ResumeTrain and PrepareCars.
	void AllowQueue() { bAllowQueue = true; }

	// True when this round's planning time is used up.
	bool IsOverBudget() const;
	// At least Milliseconds more planning time from now (a train whose turn it is).
	void GiveTime(double Milliseconds);
	// Milliseconds spent since the planner was made.
	double GetElapsedMs() const;
	// Why the last trip tried for the current train didn't work, empty if none failed.
	// Why trips tried for the current train were rejected: the most frequent reasons with their counts, or empty.
	FString DescribeProblems() const;

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
		const UFGInventoryComponent* Inventory = nullptr;
	};
	struct FCarInfo
	{
		AFGFreightWagon* Car = nullptr;
		int32 Slots = 0;
		const UFGInventoryComponent* Inventory = nullptr;
		TArray<TPair<UClass*, double>> Cargo;
	};

	int32 GetPlatformInfo(const AFGBuildableTrainPlatformCargo* Platform) const;
	// The current train's moves (mMoves), from its cars, their platforms and the nodes; needs no live train, so a
	// replayed snapshot can use it.
	void BuildMoves();
	// The current train, if it is a live one.
	const AFGTrain* TrainNow() const;
	FString TrainName() const;
	// True if car Index of the current train stops beside a platform passing Test at Station, whichever way it docks.
	bool CarFitsAt(int32 Index, const AFGTrainStationIdentifier* Station, TFunctionRef<bool(const FPlatformInfo&)> Test) const;
	// True if the current train can serve Station (same track network); always true in the automation tests.
	bool CanServeHere(const AFGTrainStationIdentifier* Station) const;
	// [map][car] platform info index (INDEX_NONE where no cargo platform is beside the car) at Station.
	const TArray<TArray<int32>>& GetCarPlatforms(const AFGTrainStationIdentifier* Station) const;
	int32 FindNode(const AFGTrainStationIdentifier* Station, UClass* Item, bool bSupply) const;
	// Earliest arrival at Station from Arrive on that keeps the arrival gap to other trains' bookings there, for a stay
	// of Stay seconds.
	double FirstFreeArrival(const AFGTrainStationIdentifier* Station, double Arrive, double Stay) const;
	// Other trains booked at Station that are still there at Reach (when this train could get there) or dock before
	// Dock (when it can dock): the docked one and those waiting with it.
	int32 CountAhead(const AFGTrainStationIdentifier* Station, double Reach, double Dock) const;
	// Other trains booked at Station between From and Until.
	int32 CountNear(const AFGTrainStationIdentifier* Station, double From, double Until) const;
	// How much Cargo ties its car down: 0 empty, NeededSoonShare if every item is wanted soon, else 1.
	double GetTiedShare(const TArray<TPair<UClass*, double>>& Cargo) const;
	// Car CarIndex can unload Item at a consumer of it the train can reach (whichever way it docks there).
	bool CarHasConsumer(int32 CarIndex, UClass* Item) const;
	// A consumer of Item the train can reach exists.
	bool ItemHasConsumer(UClass* Item) const;
	double GetAvailable(int32 Node, double Arrive) const;
	// What platform info index Info of supply node Node can give until Until seconds from now: its stock, its share of
	// the node's rate, less the pickups promised at it and its share of promises made at no known platform.
	double GetPlatformAvailable(int32 Node, int32 Info, double Until) const;
	// Room platform info index Info has for Item when a train arrives at Arrive: free slots and room in a part-full
	// stack, plus what its consumers use until then, less the deliveries promised to it and its share of promises made
	// at no known platform of demand node Node (INDEX_NONE: the free room alone).
	double GetPlatformRoom(int32 Node, int32 Info, UClass* Item, double Arrive) const;
	// Car's room for Item with Cargo on board.
	double GetCarRoom(const FCarInfo& Car, const TArray<TPair<UClass*, double>>& Cargo, UClass* Item) const;
	// True if Cargo holds at least one of an item other than Item.
	static bool HoldsOtherItem(const TArray<TPair<UClass*, double>>& Cargo, UClass* Item);
	bool IsSupplyFull(int32 Node) const;
	// True if the current train loaded (bLoad) or unloaded none of Item at Station lately.
	bool IsNoProgressHere(const AFGTrainStationIdentifier* Station, UClass* Item, bool bLoad) const;
	// True if platforms A and B (indices into mPlatforms) have the same role, mode, form and items.
	bool IsSameKind(int32 A, int32 B) const;
	FSTPlanResult SimulateWithSkips(const TArray<FSTPlanStop>& Stops, TSet<TPair<int32, int32>>& InOutSkips, const TMap<TPair<int32, UClass*>, double>& Caps) const;

	const ASTSubsystem& mSubsystem;
	TArray<FSTNode>& mNodes;
	TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>>& mBookings;
	TMap<TTuple<const AFGTrainStationIdentifier*, UClass*, bool>, int32> mNodeIndex;
	TMap<const AFGTrainStationIdentifier*, int32> mAnyNode;
	// Items a demand platform falls below target of within the forecast horizon.
	TSet<UClass*> mNeededSoon;
	double mTime = 0.0;
	double mStarted = 0.0;
	double mDeadline = 0.0;
	// Rejection reason -> how often it came up for the current train.
	mutable TMap<FString, int32> mProblems;
	bool bFcc = false;
	const AFGTrainStationIdentifier* mOnlyStation = nullptr;
	// Automation tests: every station counts as on the train's track network (they have no world).
	bool bServeAll = false;
	int32 mMaxStops = 4;
	// The stop limit from the settings, which mMaxStops goes back to for each train.
	int32 mBaseMaxStops = 4;
	bool bAllowQueue = false;
	double mMaxTripSeconds = 1200.0;

	// Current train.
	int32 mTrainIndex = INDEX_NONE;
	const AFGTrainStationIdentifier* mAnchor = nullptr;
	double mFreeAt = 0.0;
	// The train's length, for stations' waiting room (0: unknown). [cm]
	float mTrainLength = 0.f;
	bool bAnchorDocked = false;
	// The train stands in Anchor (a vehicle on its tracks) without being docked: Anchor can't be its first stop either,
	// as the game doesn't dock a train standing there already.
	bool bAnchorHere = false;
	bool bRelaxed = false;
	// The current train has a locomotive at each end (travel estimates may reverse and arrive from a far end).
	bool bPushPull = false;
	TArray<FCarInfo> mCars;
	TArray<FSTPlanMove> mMoves;

	mutable TArray<FPlatformInfo> mPlatforms;
	mutable TMap<const AFGBuildableTrainPlatformCargo*, int32> mPlatformIndex;
	mutable TMap<const AFGTrainStationIdentifier*, TArray<TArray<int32>>> mCarPlatforms;

	// What BeginTrain worked out per train this round, for ResumeTrain.
	struct FTrainSetup
	{
		const AFGTrainStationIdentifier* Anchor = nullptr;
		double FreeAt = 0.0;
		bool bAnchorDocked = false;
		bool bAnchorHere = false;
		bool bRelaxed = false;
		bool bPushPull = false;
		float TrainLength = 0.f;
		TArray<FCarInfo> Cars;
		TArray<FSTPlanMove> Moves;
		TMap<const AFGTrainStationIdentifier*, TArray<TArray<int32>>> CarPlatforms;
		// Rejection reasons from scoring the moves.
		TMap<FString, int32> Problems;
	};
	TMap<int32, FTrainSetup> mSetups;
};
