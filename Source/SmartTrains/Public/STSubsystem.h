#pragma once

#include "CoreMinimal.h"
#include "FGSaveInterface.h"
#include "STConfig.h"
#include "STTypes.h"
#include "STViews.h"
#include "Subsystem/ModSubsystem.h"
#include "STSubsystem.generated.h"

class AFGBuildableRailroadStation;
class AFGBuildableTrainPlatformCargo;
class AFGFreightWagon;
class AFGRailroadVehicle;
class AFGTrain;
class AFGTrainStationIdentifier;
class FSTThroughput;
class FSTTravel;
class FSTTripPlanner;
struct FSTBooking;
struct FSTPlanStop;
struct FSTPlanResult;
class UFGItemDescriptor;

// A planned change to a node's stock: a delivery (demand) or a pickup (supply) of Amount at Time (game seconds).
struct FSTNodeEvent
{
	double Time = 0.0;
	double Amount = 0.0;
};

// One station's supply or demand of one item, gathered from its platforms (Docs/Architecture.md, "Dispatching").
struct FSTNode
{
	TWeakObjectPtr<AFGTrainStationIdentifier> Station;
	TSubclassOf<UFGItemDescriptor> Item;
	ESTRole Role = ESTRole::Off;
	// Null Item on a Demand node: a sink that takes anything.
	bool bAnyItem = false;
	TArray<TWeakObjectPtr<AFGBuildableTrainPlatformCargo>> Platforms;
	int32 Stock = 0;
	int32 Capacity = 0;
	// Measured (or manual) rate, items per second; supply flows in, demand flows out.
	double Rate = 0.0;
	bool bRateKnown = false;
	// Demand ran dry a while ago, so its measured rate is only a lower bound.
	bool bStarved = false;
	ESTPriority Priority = ESTPriority::Normal;
	float TargetPercent = 50.f;
	float SendAtPercent = 0.f;
	int32 TrackGraph = INDEX_NONE;
	// Amounts already promised: to be picked up (supply) or on the way (demand).
	int32 Reserved = 0;
	// The part of Reserved planned at a known platform: pickups (supply) or deliveries (demand). The rest is shared out
	// over the platforms.
	TMap<const AFGBuildableTrainPlatformCargo*, int32> PlatformReserved;
	// Planned flow from the flow plan, items per second: out of a supplier, into a consumer.
	double PlannedFlow = 0.0;
	// Deliveries (demand) or pickups (supply) planned by jobs under way, with their arrival times; repeats excluded.
	TArray<FSTNodeEvent> Planned;
	// The machines behind its platforms (from the connection trace), and what their recipes make (supply) or use
	// (demand) of Item at their clock speed; 0 when not known. [/s]
	TArray<TWeakObjectPtr<const AActor>> Machines;
	double RecipeRate = 0.0;
	// Demand: its machines wait on another item that is starved there, so its shortfall counts this much.
	double CostScale = 1.0;
	TSubclassOf<UFGItemDescriptor> WaitsOn;
};

// A route of the flow plan: Item from From to To, with the trains it needs and the managed trains that fit it.
struct FSTFlowRoute
{
	TWeakObjectPtr<AFGTrainStationIdentifier> From;
	TWeakObjectPtr<AFGTrainStationIdentifier> To;
	TWeakObjectPtr<UClass> Item;
	// Planned flow. [/s]
	double Flow = 0.0;
	// Round trip with stays. [s]
	double RoundTrip = 0.0;
	// Share of trains it needs: flow x round trip / average load of the trains that fit.
	double Trains = 0.0;
	// Managed trains whose cars fit it at both ends, and what each carries per trip.
	TArray<TWeakObjectPtr<AFGTrain>> Fitting;
	TArray<int32> Capacity;

	static FString MakeKey(const void* From, const void* To, const void* Item) { return FString::Printf(TEXT("%p|%p|%p"), From, To, Item); }
	FString GetKey() const { return MakeKey(From.Get(), To.Get(), Item.Get()); }
};

// An item group (Docs/Architecture.md, "Groups"): the suppliers and consumers of one item that the flow plan links, and
// the trains whose home it is. Rebuilt from every flow plan; homes and the player's settings are saved with the trains
// and in FSTGroupConfig.
struct FSTGroup
{
	TWeakObjectPtr<UClass> Item;
	// The flow plan's routes of the item.
	TArray<FSTFlowRoute> Routes;
	// Trains its routes need, smoothed over flow plans.
	double Needed = 0.0;
	// When the flow plan last had a route of it (Now()).
	double LastRouteAt = 0.0;
	// Managed trains whose cars fit at least one of its routes.
	TArray<TWeakObjectPtr<AFGTrain>> Fitting;
	// Trains whose home it is.
	TArray<TWeakObjectPtr<AFGTrain>> Homes;
};

// What fits when one train does one pickup and delivery (Docs/Architecture.md, "Fit model").
struct FSTFit
{
	TArray<AFGFreightWagon*> Cars;
	TArray<AFGFreightWagon*> SkipAtPickup;
	TArray<AFGFreightWagon*> SkipAtDrop;
	int32 Capacity = 0;
	FString Problem;
};

// Holds SmartTrains' settings and learned data, saves them with the world, and dispatches managed trains (server).
UCLASS()
class SMARTTRAINS_API ASTSubsystem : public AModSubsystem, public IFGSaveInterface
{
	GENERATED_BODY()
	friend class FSTTripPlanner;
	friend class FSTThroughput;
	friend struct FSTTestAccess;

public:
	ASTSubsystem();
	virtual ~ASTSubsystem() override;

	static ASTSubsystem* Get(const UObject* WorldContext);

	// Called by the dock hook when Train starts docking at Station (server), so even docks of a few frames are seen.
	void NoteDockStarted(AFGTrain* Train, AFGTrainStationIdentifier* Station);

	//~ Settings, server only. Cause names the player or tool for the log.
	FSTPlatformConfig* FindPlatformConfig(const AFGBuildableTrainPlatformCargo* Platform);
	const FSTPlatformConfig* FindPlatformConfig(const AFGBuildableTrainPlatformCargo* Platform) const;
	FSTStationConfig* FindStationConfig(const AFGTrainStationIdentifier* Station);
	FSTTrainState* FindTrainState(const AFGTrain* Train);
	FSTTrainState& FindOrAddTrainState(AFGTrain* Train);

	// Sets Platform's role and items, switching its load/unload setting to match; Off puts its own setting back.
	void SetPlatformConfig(AFGBuildableTrainPlatformCargo* Platform, ESTRole NewRole, const TArray<FSTItemRule>& Items, const FString& Cause);
	void SetStationDepot(AFGTrainStationIdentifier* Station, bool bDepot, const FString& Cause);

	// Starts or ends management of Train. Starting saves its timetable; ending puts it back and lets it drive on.
	// Returns false with OutWhy set if it can't be managed (kept vanilla, no locomotive).
	bool SetTrainManaged(AFGTrain* Train, bool bManaged, const FString& Cause, FString& OutWhy);
	// Deletes everything in Train's freight cars; a managed train's trip is planned again. Returns the items removed
	// (fluids in m3).
	int32 EmptyTrainCars(AFGTrain* Train, const FString& Cause);
	// Managed trains, for commands that act on all of them.
	TArray<AFGTrain*> GetManagedTrains() const;
	void SetTrainKeepVanilla(AFGTrain* Train, bool bKeepVanilla, const FString& Cause);

	// Gives Train a job by hand (for testing): pick up Item at From with the cars that fit, deliver it to To.
	bool AssignManualJob(AFGTrain* Train, AFGTrainStationIdentifier* From, AFGTrainStationIdentifier* To, TSubclassOf<UFGItemDescriptor> Item, FString& OutResult);

	// Gives every other platform of the same cargo type in Platform's station Platform's settings. Returns how many
	// were changed, or -1 with OutResult saying why nothing was.
	int32 CopyPlatformToStation(AFGBuildableTrainPlatformCargo* Platform, const FString& Cause, FString& OutResult);

	//~ Readouts for the menus (sent through USTRemoteCallObject). AppliedEdit is echoed for the receiving player.
	FSTPlatformView MakePlatformView(const AFGBuildableTrainPlatformCargo* Platform, int32 AppliedEdit) const;
	FSTTrainView MakeTrainView(const AFGTrain* Train, int32 AppliedEdit) const;
	FSTStationView MakeStationView(const AFGTrainStationIdentifier* Station, int32 AppliedEdit) const;

	// Says why no train is coming to Platform, one line per item that has none on the way; empty when all is well.
	TArray<FString> ExplainPlatform(const AFGBuildableTrainPlatformCargo* Platform) const;

	//~ Readouts for chat and the migration report.
	TArray<FString> DescribePlatform(const AFGBuildableTrainPlatformCargo* Platform) const;
	TArray<FString> DescribeStation(const AFGTrainStationIdentifier* Station) const;
	TArray<FString> DescribeTrain(const AFGTrain* Train) const;
	TArray<FString> DescribeStatus() const;
	// What limits throughput per item, from the last half hour ("/st monitor").
	FSTMonitorView MakeMonitorView() const;
	TArray<FString> DescribeMonitor() const;
	TArray<FString> DescribeJobs() const;
	TArray<FString> DescribeFit(const AFGTrain* Train) const;

	// Platform rate in items per second and whether it has been measured long enough.
	double GetMeasuredRate(const AFGBuildableTrainPlatformCargo* Platform, TSubclassOf<UFGItemDescriptor> Item, bool* bKnown = nullptr) const;

	// Works out whether Train can carry Item from From to To, and with which cars. bIgnoreCargo: by the cars' positions
	// alone, as if they were empty (a train carrying something else delivers it first), for counts and readouts.
	FSTFit ComputeFit(const AFGTrain* Train, const AFGTrainStationIdentifier* From, const AFGTrainStationIdentifier* To, TSubclassOf<UFGItemDescriptor> Item,
		bool bIgnoreCargo = false) const;

	// Vehicles beside each platform of Station when Train docks there: learned if seen, else predicted. bLearned is set
	// when it was seen. Several maps come back when the train may arrive either way round and hasn't been seen.
	TArray<TArray<AFGRailroadVehicle*>> GetDockMaps(const AFGTrain* Train, const AFGTrainStationIdentifier* Station, bool* bLearned = nullptr) const;

	// Estimated travel time from From to To. [s]
	// bPushPull: the train has a locomotive at each end, so it may reverse out and arrive from a station's far end.
	float EstimateTravelSeconds(const AFGTrainStationIdentifier* From, const AFGTrainStationIdentifier* To, bool bPushPull = false) const;
	// True if Train has locomotives that can dock facing both ways.
	static bool IsPushPull(const AFGTrain* Train);
	float EstimateStaySeconds(const AFGTrainStationIdentifier* Station) const;
	// True if Station is a dead end: a train in it can only leave the way trains come in.
	bool IsDeadEnd(const AFGTrainStationIdentifier* Station) const;

	// /st snapshot: the next dispatch round writes its planning inputs to a file.
	void RequestSnapshot() { bSnapshotNext = true; }
	// How many trains of TrainLength fit on the track before Station's way-in switch, waiting for it; -1 if not known yet.
	int32 GetWaitingRoom(const AFGTrainStationIdentifier* Station, float TrainLength) const;
	// The switch on Station's way in, shared by stations reached through it; null if not known or none.
	const void* GetJunction(const AFGTrainStationIdentifier* Station) const;
	// Auto Trains' load on the switches and crossings around Station (STAt::GetTrackLoad, asked at most every 15 s per
	// station): 0 when free or unknown, 1 when booked the whole next 3 minutes. OutWaiting: seconds trains wait there now.
	float GetTrackLoad(const AFGTrainStationIdentifier* Station, float* OutWaiting = nullptr) const;
	struct FTrackLoad
	{
		float Load = 0.f;
		float Waiting = 0.f;
		double At = -1.0e9;
	};
	mutable TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, FTrackLoad> mTrackLoads;

	const FSTSettings& GetSettings() const { return mSettings; }


	// "car 1 unloads 1,200 Copper Ingot, car 3 unloads 600 Wire"; with bMoved, the moved amount of the planned one.
	FString DescribeLeg(const AFGTrain* Train, const FSTJobLeg& Leg, bool bMoved) const;

	// Items of Item in Car's freight inventory.
	static int32 CountInCar(const AFGRailroadVehicle* Car, TSubclassOf<UFGItemDescriptor> Item);
	bool IsFccAvailable() const;

	//~ Migration support.
	const TArray<FSTPlatformConfig>& GetPlatformConfigs() const { return mPlatforms; }
	const TArray<FSTTrainState>& GetTrainStates() const { return mTrains; }
	FSTMigrationUndo& GetMigrationUndo() { return mMigrationUndo; }
	// Set when /st testcopy or /st backup ran this session.
	bool bMadeBackupThisSession = false;

	//~ Station set-up (STStationSetup.cpp).
	// How stations are named, saved with the world.
	UPROPERTY(SaveGame)
	FSTNamingConfig mNaming;
	// Runtime only: per platform, why its settings no longer match what its belts or pipes carry (from StepTraces).
	TMap<TWeakObjectPtr<AFGBuildableTrainPlatformCargo>, FString> mSetupHints;
	// Runtime only: the last set-up preview per station, applied as shown when confirmed soon after.
	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, double> mSetupPreviewAt;
	// Lines for the station menu's set-up section: a preview, or what the last apply did.
	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, TArray<FString>> mSetupLines;
	// Preview (bApply false) or apply the set-up of Station; bRename false keeps its name. Returns chat lines.
	TArray<FString> SetUpStation(AFGTrainStationIdentifier* Station, bool bApply, bool bRename, const FString& Cause);
	// Every station with cargo platforms; writes Saved/SmartTrains/StationSetup.csv. Returns chat lines.
	TArray<FString> SetUpAllStations(bool bApply, bool bRename, const FString& Cause);
	// /st setup format|words: changes mNaming. Returns false with OutMessage on bad input.
	bool SetNaming(const TArray<FString>& Words, const FString& Cause, FString& OutMessage);

	//~ Begin AActor
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor

	//~ Begin IFGSaveInterface
	virtual bool ShouldSave_Implementation() const override { return true; }
	virtual void PreSaveGame_Implementation(int32 SaveVersion, int32 GameVersion) override;
	virtual void PostLoadGame_Implementation(int32 SaveVersion, int32 GameVersion) override;
	//~ End IFGSaveInterface

private:
	struct FItemStats
	{
		int32 LastStock = -1;
		int32 LastCarItems = -1;
		TWeakObjectPtr<AFGRailroadVehicle> LastCar;
		double Rate = 0.0;
		double Measured = 0.0;
		double EmptySeconds = 0.0;
	};
	struct FPlatformStats
	{
		TMap<UClass*, FItemStats> Items;
	};

	// Fast checks of every managed train: docking, job progress, external timetable changes. [0.25 s]
	void MonitorTrains();
	// Supply chain (Docs/Architecture.md): marks consumers whose machines wait on another starved item there.
	static void LinkSupplyChain(TArray<FSTNode>& Nodes);
	// Samples every configured platform's stock and rates. [1 s]
	void SamplePlatforms();
	// /st snapshot: the next dispatch round writes every free train's planning inputs to a file for replay tests.
	bool bSnapshotNext = false;
	void WriteSnapshot(const TArray<FSTNode>& Nodes, const TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>>& Bookings, const TArray<TSharedPtr<class FJsonValue>>& Trains) const;
	// Traces one platform's belts or pipes to its machines (STTrace), each again after a while. [1 s]
	void StepTraces();
	// Machines behind each traced platform, and when it was traced (Now()).
	struct FTraceCache
	{
		double At = -1.0e9;
		TArray<TWeakObjectPtr<const AActor>> Machines;
	};
	TMap<TWeakObjectPtr<AFGBuildableTrainPlatformCargo>, FTraceCache> mTraces;
	int32 mTraceNext = 0;
	// Hands out trips to free trains, and re-plans trips under way that asked for it (STDispatch.cpp).
	void Dispatch();
	void DispatchRound();
	// Marks or clears State's unwanted cargo (an item on board no stop still ahead unloads).
	void UpdateUnwantedCargo(FSTTrainState& State);
	// Re-plans the rest of State's trip from where it is and what its cars hold; keeps stops already passed.
	void ReplanTrip(FSTTrainState& State, FSTTripPlanner& Planner);
	// Gives free trains trips (Docs/Architecture.md, "Groups"): home work first, then any work (lent), else they park.
	void GroupRound(FSTTripPlanner& Planner, TArray<FSTNode>& Nodes);
	// Gives free trains trips built around one consumer station each (STGather.cpp; the default dispatcher).
	void GatherRound(FSTTripPlanner& Planner, TArray<FSTNode>& Nodes);
	// Turns a planned trip into State's job (and, if bReserve, reserves it for the trains planned after it).
	void CommitTrip(FSTTrainState& State, FSTTripPlanner& Planner, const TArray<FSTPlanStop>& Stops, const FSTPlanResult& Result, bool bReserve, UClass* GroupItem = nullptr,
		AFGTrainStationIdentifier* Anchor = nullptr);
	// Records why a free train got no trip; logged when the reason changes.
	void NoteNoTrip(const FSTTrainState& State, const FString& Reason);
	// Works out planned flows between suppliers and consumers.
	void PlanFlows();
	// One step of the travel distance search.
	void StepTravel();
	void ReadSettings();

	// Builds the supply and demand nodes from the platform settings and current jobs.
	void BuildNodes(TArray<FSTNode>& OutNodes) const;
	int32 FindNode(const TArray<FSTNode>& Nodes, const AFGTrainStationIdentifier* Station, TSubclassOf<UFGItemDescriptor> Item, ESTRole NodeRole) const;

	// Station nearest to Train's first vehicle on its rail network, for trains that haven't docked anywhere yet.
	AFGTrainStationIdentifier* FindNearestStation(const AFGTrain* Train) const;

	// Where Train will be free and when (game seconds); false if it isn't free soon.
	bool GetFreeAt(const FSTTrainState& State, AFGTrainStationIdentifier*& OutStation, double& OutTime) const;

	// Gives State's train Job (or only logs it in observe-only mode).
	void CommitJob(FSTTrainState& State, FSTJob&& Job, const FString& Line);
	// Writes State's job into its train's timetable (from the current leg on) and lets it drive.
	void WriteJob(FSTTrainState& State, const TCHAR* Why);
	// Ends State's job after its last leg: next job, depot, or parks where it is.
	void FinishJob(FSTTrainState& State);
	// Gives State a job that only drives to Station (moving nothing), then parks.
	void SendToStation(FSTTrainState& State, AFGTrainStationIdentifier* Station, const TCHAR* Why);
	// Switches the train's autopilot off where it stands until it gets a job.
	void Park(FSTTrainState& State, const TCHAR* Why);
	// Nearest depot for State's train, or nullptr.
	// The nearest depot (but Except) Train can reach that no other train is at or heading for.
	AFGTrainStationIdentifier* FindDepot(const FSTTrainState& State, const AFGTrainStationIdentifier* From, const AFGTrainStationIdentifier* Except = nullptr) const;
	// Another train is docked or parked at Station, or a managed train's job still goes there.
	bool IsDepotTaken(const AFGTrainStationIdentifier* Station, const AFGTrain* Except) const;
	// Moves parked trains off stations a job needs.
	void ClearNeededBays();
	// For a train without a job: hands out jobs now, else sends it to a depot or parks it.
	// bDispatchFirst: hand out trips first (during a dispatch round the train is settled when the round ends instead).
	void SettleIdle(FSTTrainState& State, const TCHAR* Why, bool bDispatchFirst = true);
	// Like ComputeFit for a train that already carries Item: which loaded cars can unload it at To.
	FSTFit ComputeDeliveryFit(const AFGTrain* Train, const AFGTrainStationIdentifier* To, TSubclassOf<UFGItemDescriptor> Item) const;
	// Room for Item at a sink node: free slots of its platforms times the item's stack size.
	int32 GetSinkRoom(const FSTNode& Node, TSubclassOf<UFGItemDescriptor> Item) const;
	// Adds a line to the station's recent job history.
	void AddHistory(const AFGTrainStationIdentifier* Station, const FString& Line);

	// Called when State's train docks at Station / leaves it.
	void OnDocked(FSTTrainState& State, AFGTrainStationIdentifier* Station);
	// Starts State's dock at Station: dock time, transfer tracking, OnDocked.
	void BeginDock(FSTTrainState& State, AFGTrainStationIdentifier* Station);
	void OnUndocked(FSTTrainState& State, AFGTrainStationIdentifier* Station, double Stay);
	// Ends a docked train's stay once its planned transfer here is done, or when nothing has moved for a while, so it
	// doesn't wait out the stop's time while blocking the station and locking its platforms.
	void ReleaseDockWhenDone(FSTTrainState& State, double Time);
	// One line per cargo platform of Station: position, load or unload, role, slots used and what it holds. For logs.
	FString DescribeStationPlatforms(const AFGTrainStationIdentifier* Station) const;
	// Station bookings of the trips under way (repeats book nothing), parked trains and Auto Trains' station queues.
	void BuildBookings(const TArray<FSTNode>& Nodes, double Time, TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>>& Out);
	// True if other managed trains (2 or more, Except not counted) are on their way to Station, or Auto Trains reports a
	// queue there.
	bool IsStationQueued(const AFGTrainStationIdentifier* Station, const AFGTrain* Except) const;
	// Records which vehicle is beside each platform of Station now.
	void LearnDockMap(AFGTrain* Train, AFGTrainStationIdentifier* Station);
	void LearnTrip(AFGTrainStationIdentifier* From, AFGTrainStationIdentifier* To, float Seconds);
	void LearnStay(AFGTrainStationIdentifier* Station, float Seconds);

	// Shows the job in the train's name (setting) or puts the original back.
	void UpdateTrainName(FSTTrainState& State);

	// Returns true if Train may serve Station (same rail network).
	bool CanServe(const FSTTrainState& State, const AFGTrainStationIdentifier* Station) const;

	// One readout line for Rule of Platform: stock, capacity, rate, batch or target, priority.
	FString DescribeItemRule(const AFGBuildableTrainPlatformCargo* Platform, const FSTPlatformConfig& Config, const FSTItemRule& Rule) const;

	// Items carried by Train's freight cars, by item class.
	static TMap<UClass*, int32> GetCargo(const AFGTrain* Train);

	double Now() const;

	UPROPERTY(SaveGame)
	TArray<FSTPlatformConfig> mPlatforms;

	UPROPERTY(SaveGame)
	TArray<FSTStationConfig> mStations;

	UPROPERTY(SaveGame)
	TArray<FSTTrainState> mTrains;

	UPROPERTY(SaveGame)
	TArray<FSTDockMap> mDockMaps;

	UPROPERTY(SaveGame)
	TArray<FSTTripTime> mTrips;

	UPROPERTY(SaveGame)
	TArray<FSTDwellTime> mStays;

	UPROPERTY(SaveGame)
	FSTMigrationUndo mMigrationUndo;

	UPROPERTY(SaveGame)
	int32 mNextJobId = 1;

	// When each train last had its turn in a dispatch round (Now()); runtime only.
	TMap<TWeakObjectPtr<AFGTrain>, double> mPlanTurns;

	// Players' group settings, by item.
	UPROPERTY(SaveGame)
	TArray<FSTGroupConfig> mGroupConfigs;

	// The item groups of the last flow plan.
	TArray<FSTGroup> mGroups;

public:
	// Groups (STGroups.cpp).
	const FSTGroup* FindGroup(const UClass* Item) const;
	const FSTGroupConfig* FindGroupConfig(const UClass* Item) const;
	// "Copper Ingot".
	static FString DescribeGroup(const FSTGroup& Group);
	// Every flow plan: groups follow the plan's routes (kept for GroupKeepSeconds without any), then trains get homes.
	void UpdateGroups(TArray<FSTFlowRoute>&& Routes);
	// Gives every managed train a home among the groups it fits, in proportion to what they need (AllocateHomes).
	void AllocateHomes();
	// Trains a group wants as homes: ceil(needed - slack), within the player's limits.
	int32 GetGroupTarget(const FSTGroup& Group) const;
	// Trains it lacks that no idle train could make up for: short only while every train that fits it is busy.
	int32 GetGroupShortfall(const FSTGroup& Group) const;
	// Trains whose current trip moves the group's item, home or lent.
	int32 CountWorkingFor(const FSTGroup& Group) const;
	// One line per group, for /st group.
	TArray<FString> DescribeGroups() const;
	// What the train does for its group (empty when not managed).
	FString DescribeHome(const FSTTrainState& State) const;
	// Groups of Platform's items, or why it has none; groups at Station, its room to wait and its switch.
	TArray<FString> DescribePlatformGroups(const AFGBuildableTrainPlatformCargo* Platform) const;
	TArray<FString> DescribeStationGroups(const AFGTrainStationIdentifier* Station) const;
	// Player settings: a train's Stay in this group; a group's fewest and most trains (0: automatic).
	bool SetTrainHomeStay(AFGTrain* Train, bool bOn, const FString& Cause);
	bool SetGroupTrains(UClass* Item, int32 MinTrains, int32 MaxTrains, const FString& Cause, FString& OutResult);

private:

	// Now() when the world was saved, so the clock carries on after loading instead of restarting at 0. [s]
	UPROPERTY(SaveGame)
	double mClockAtSave = 0.0;

	// Fleet advice per item from the last flow plan.
	struct FAdvice
	{
		double FlowPerMinute = 0.0;
		double TrainsNeeded = 0.0;
		int32 TrainsFitting = 0;
		int32 Pairs = 0;
	};
	// Records Flow (items per second) on the planned route From -> To and adds its trains to Advice.
	void AddPlannedRoute(const TArray<FSTNode>& Nodes, int32 From, int32 To, double Flow, float Time, FAdvice& Advice);
	// Routes of the flow plan being worked out.
	TArray<FSTFlowRoute> mPendingRoutes;
	// Unloading platforms that took much less than planned, until when: their predicted room counts no drain.
	TMap<TWeakObjectPtr<const AFGBuildableTrainPlatformCargo>, double> mSlowDrainUntil;
	bool IsSlowDrain(const AFGBuildableTrainPlatformCargo* Platform) const;

	FSTSettings mSettings;
	// Planned flow per (supplier node key, consumer node key), items per second, from the last flow plan.
	TMap<FString, double> mPlannedPairs;
	TMap<UClass*, FAdvice> mAdvice;
	// Recent job lines per station, newest last.
	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, TArray<FString>> mHistory;
	// Item count in the job's cars when the current dock began, per train.
	// Per train: the amount of each work item in its car when the current dock began (index = work of the leg).
	TMap<TWeakObjectPtr<AFGTrain>, TArray<int32>> mDockStartAmounts;
	double mStartTime = 0.0;
	// Added to the world's time (which starts at 0 each session) to make Now(). [s]
	double mClockBase = 0.0;
	// Platform settings changed: trips under way are re-planned in the next dispatch.
	bool bReplanAll = false;
	// The settings have been read once (the first read logs them all).
	bool bSettingsRead = false;
	// Per free train: why it got no trip in the last round, logged when it changes and shown in its menu.
	TMap<TWeakObjectPtr<AFGTrain>, FString> mIdleReasons;
	// When the planning time last ran out (for a throttled warning). [game s]
	double mLastBudgetWarning = -1000.0;
	// A dispatch round is running; trains that become idle in it are settled when it ends (mToSettle).
	bool bInDispatch = false;
	TArray<TWeakObjectPtr<AFGTrain>> mToSettle;
	// Last "would" line logged per train in observe-only mode.
	TMap<TWeakObjectPtr<AFGTrain>, FString> mObserved;
	// Trains whose dock map still has to be read during the current dock.
	TSet<TWeakObjectPtr<AFGTrain>> mPendingDockMaps;
	double mLastDispatch = -1000.0;
	double mLastPlan = -1000.0;
	TMap<TWeakObjectPtr<AFGBuildableTrainPlatformCargo>, FPlatformStats> mStats;
	// Last flow plan: (station, item, role) -> planned flow per second.
	TArray<FSTNode> mPlannedNodes;
	FSTTravel* mTravel = nullptr;
	FSTThroughput* mThroughput = nullptr;
	FTimerHandle mThroughputTimer;
	// When the monitor report was last logged. [game s]
	double mLastMonitorLog = 0.0;
	// True if State's train moved none of Item at Station lately (FSTTrainState::NoProgress), so the move isn't
	// planned again yet.
	bool IsNoProgress(const FSTTrainState& State, const AFGTrainStationIdentifier* Station, const UClass* Item, bool bLoad) const;
	// Auto Trains' station queues: seconds until the last train there has left, and when it was asked. [s]
	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, TPair<float, double>> mAtQueues;
	// Auto Trains travel-time questions left this dispatch round (each is a search over the whole network).
	mutable int32 mAtTravelCallsLeft = 0;
	// Auto Trains' travel times between stations, and when they were asked. [s]
	mutable TMap<TPair<TWeakObjectPtr<AFGTrainStationIdentifier>, TWeakObjectPtr<AFGTrainStationIdentifier>>, TPair<float, double>> mAtTravel;
	FTimerHandle mMonitorTimer;
	FTimerHandle mSampleTimer;
	FTimerHandle mDispatchTimer;
	FTimerHandle mPlanTimer;
	FTimerHandle mTravelTimer;
	double mLastSample = 0.0;
	bool bDispatchSoon = false;
};
