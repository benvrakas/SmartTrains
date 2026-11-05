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
class FSTTravel;
class FSTTripPlanner;
class UFGItemDescriptor;

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
	// Planned flow from the flow plan, items per second: out of a supplier, into a consumer.
	double PlannedFlow = 0.0;
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

public:
	ASTSubsystem();
	virtual ~ASTSubsystem() override;

	static ASTSubsystem* Get(const UObject* WorldContext);

	//~ Settings, server only. Cause names the player or tool for the log.
	FSTPlatformConfig* FindPlatformConfig(const AFGBuildableTrainPlatformCargo* Platform);
	const FSTPlatformConfig* FindPlatformConfig(const AFGBuildableTrainPlatformCargo* Platform) const;
	FSTStationConfig* FindStationConfig(const AFGTrainStationIdentifier* Station);
	FSTTrainState* FindTrainState(const AFGTrain* Train);
	FSTTrainState& FindOrAddTrainState(AFGTrain* Train);

	// Sets Platform's role and items, switching its load/unload setting to match; Off puts its own setting back.
	// With bWholeStation, a station whose platforms are linked (IsPlatformLinked) gets them on every platform of that
	// kind.
	void SetPlatformConfig(AFGBuildableTrainPlatformCargo* Platform, ESTRole NewRole, const TArray<FSTItemRule>& Items, const FString& Cause,
		bool bWholeStation = true);

	// Station platform linking (STStationLink.cpp).
	ESTPlatformLink GetStationLink(const AFGTrainStationIdentifier* Station) const;
	// Turning linking on copies Source's settings to every platform of its kind in the station.
	void SetStationLink(AFGTrainStationIdentifier* Station, ESTPlatformLink Link, AFGBuildableTrainPlatformCargo* Source, const FString& Cause);
	// True if an edit of Platform applies to every platform of its kind in its station.
	bool IsPlatformLinked(const AFGBuildableTrainPlatformCargo* Platform) const;
	// True if every platform of Platform's kind in its station has the same role and items.
	bool DoStationPlatformsMatch(const AFGBuildableTrainPlatformCargo* Platform) const;
	void SetStationDepot(AFGTrainStationIdentifier* Station, bool bDepot, const FString& Cause);

	// Starts or ends management of Train. Starting saves its timetable; ending puts it back and lets it drive on.
	// Returns false with OutWhy set if it can't be managed (kept vanilla, no locomotive).
	bool SetTrainManaged(AFGTrain* Train, bool bManaged, const FString& Cause, FString& OutWhy);
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
	TArray<FString> DescribeJobs() const;
	TArray<FString> DescribeFit(const AFGTrain* Train) const;

	// Platform rate in items per second and whether it has been measured long enough.
	double GetMeasuredRate(const AFGBuildableTrainPlatformCargo* Platform, TSubclassOf<UFGItemDescriptor> Item, bool* bKnown = nullptr) const;

	// Works out whether Train can carry Item from From to To, and with which cars.
	FSTFit ComputeFit(const AFGTrain* Train, const AFGTrainStationIdentifier* From, const AFGTrainStationIdentifier* To, TSubclassOf<UFGItemDescriptor> Item) const;

	// Vehicles beside each platform of Station when Train docks there: learned if seen, else predicted. bLearned is set
	// when it was seen. Several maps come back when the train may arrive either way round and hasn't been seen.
	TArray<TArray<AFGRailroadVehicle*>> GetDockMaps(const AFGTrain* Train, const AFGTrainStationIdentifier* Station, bool* bLearned = nullptr) const;

	// Estimated travel time from From to To. [s]
	float EstimateTravelSeconds(const AFGTrainStationIdentifier* From, const AFGTrainStationIdentifier* To) const;
	float EstimateStaySeconds(const AFGTrainStationIdentifier* Station) const;

	const FSTSettings& GetSettings() const { return mSettings; }

	// When a delivery of Item last reached Station (game seconds); the session start if none has yet.
	double GetLastServed(const AFGTrainStationIdentifier* Station, UClass* Item) const;

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
	// Samples every configured platform's stock and rates. [1 s]
	void SamplePlatforms();
	// Hands out trips to free trains, and re-plans trips under way that asked for it (STDispatch.cpp).
	void Dispatch();
	void DispatchRound();
	// Re-plans the rest of State's trip from where it is and what its cars hold; keeps stops already passed.
	void ReplanTrip(FSTTrainState& State, FSTTripPlanner& Planner);
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
	AFGTrainStationIdentifier* FindDepot(const FSTTrainState& State, const AFGTrainStationIdentifier* From) const;
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
	void OnUndocked(FSTTrainState& State, AFGTrainStationIdentifier* Station, double Stay);
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

	FSTSettings mSettings;
	// Planned flow per (supplier node key, consumer node key), items per second, from the last flow plan.
	TMap<FString, double> mPlannedPairs;
	TMap<UClass*, FAdvice> mAdvice;
	// Recent job lines per station, newest last.
	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, TArray<FString>> mHistory;
	// Item count in the job's cars when the current dock began, per train.
	// Per train: the amount of each work item in its car when the current dock began (index = work of the leg).
	TMap<TWeakObjectPtr<AFGTrain>, TArray<int32>> mDockStartAmounts;
	// When a delivery last reached each (station, item); runtime only.
	TMap<TPair<TWeakObjectPtr<AFGTrainStationIdentifier>, UClass*>, double> mLastServed;
	double mStartTime = 0.0;
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
	FTimerHandle mMonitorTimer;
	FTimerHandle mSampleTimer;
	FTimerHandle mDispatchTimer;
	FTimerHandle mPlanTimer;
	FTimerHandle mTravelTimer;
	double mLastSample = 0.0;
	bool bDispatchSoon = false;
};
