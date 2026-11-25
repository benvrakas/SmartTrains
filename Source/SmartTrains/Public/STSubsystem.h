#pragma once

#include "CoreMinimal.h"
#include "FGSaveInterface.h"
#include "STConfig.h"
#include "STTypes.h"
#include "STDepotSignal.h"
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
class UFGItemDescriptor;

// One car slot of a consumer's pattern: the Demand platform at Index of its station, the items it takes (several: an
// item group) and how much of each its factory uses at most, in car loads per minute.
struct FSTLoopSlot
{
	int32 Index = INDEX_NONE;
	TWeakObjectPtr<AFGBuildableTrainPlatformCargo> Platform;
	TArray<TWeakObjectPtr<UClass>> Items;
	TArray<double> ItemRates;
	// The items' rates together. [car loads/min]
	double Rate = 0.0;
	bool bFluid = false;
};

// Where a consumer's loop takes one of its items: a supplier (index into the plan's suppliers) and the rate taken
// there. [car loads/min]
struct FSTLoopSource
{
	TWeakObjectPtr<UClass> Item;
	int32 Supplier = INDEX_NONE;
	double Taken = 0.0;
};

// A consumer station: its pattern, its need and the trains on its loop (Docs/Architecture.md, "Loops").
struct FSTConsumer
{
	TWeakObjectPtr<AFGTrainStationIdentifier> Station;
	TArray<FSTLoopSlot> Slots;
	// Its busiest slot's rate: one train brings one car per slot per round. [car loads/min]
	double Rate = 0.0;
	// A slot's rate is known from the factory scan or the platform's machines.
	bool bRateKnown = false;
	// Round trip over its nearest suppliers with the stays. [s]
	double RoundTrip = 0.0;
	// Rate times round trip: its share of all trains is its share of this.
	double Need = 0.0;
	// Trains it should have, and the trains on its loop now.
	int32 Target = 0;
	TArray<TWeakObjectPtr<AFGTrain>> Trains;
	// Its items' suppliers, nearest with spare rate first.
	TArray<FSTLoopSource> Sources;
	// Why it gets no train, empty when it has trains or nothing is wrong.
	FString Problem;
	// The same in a few words, for the monitor's row ("no train fits its cars").
	FString Cause;
	// Share of its machines that want input now (powered, running, output not backed up), averaged over a few minutes.
	double Use = 1.0;
	// Trains its loop needs running for that use (at least one); the rest of its trains wait at depot signals.
	int32 Wanted = 0;
	// Its trains waiting at a depot signal or on their way there.
	int32 AtDepot = 0;
};

// What a supplier station offers of one item, and how much of it consumers' loops take.
struct FSTSupplier
{
	TWeakObjectPtr<AFGTrainStationIdentifier> Station;
	TWeakObjectPtr<UClass> Item;
	// Its most rate; below 0 when not known (it counts as having enough). [car loads/min]
	double Rate = -1.0;
	double Taken = 0.0;
};

// Holds SmartTrains' settings and learned data, saves them with the world, and gives managed trains their loops
// (server).
UCLASS()
class SMARTTRAINS_API ASTSubsystem : public AModSubsystem, public IFGSaveInterface
{
	GENERATED_BODY()
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
	FSTTrainState* FindTrainState(const AFGTrain* Train);
	const FSTTrainState* FindTrainState(const AFGTrain* Train) const;
	FSTTrainState& FindOrAddTrainState(AFGTrain* Train);

	// Sets Platform's role and items, switching its load/unload setting to match; Off puts its own setting back.
	void SetPlatformConfig(AFGBuildableTrainPlatformCargo* Platform, ESTRole NewRole, const TArray<FSTItemRule>& Items, const FString& Cause);
	// Keeps the machines Platform's trace reached (for its recipe rate); only Autodetect, station set-up and the
	// migration trace platforms.
	void SetPlatformMachines(AFGBuildableTrainPlatformCargo* Platform, const TArray<TWeakObjectPtr<const AActor>>& Machines);

	// Starts or ends management of Train. Starting saves its timetable; ending puts it back and lets it drive on.
	// Returns false with OutWhy set if it can't be managed (kept vanilla, no locomotive).
	bool SetTrainManaged(AFGTrain* Train, bool bManaged, const FString& Cause, FString& OutWhy);
	// Deletes everything in Train's freight cars. Returns the items removed (fluids in m3).
	int32 EmptyTrainCars(AFGTrain* Train, const FString& Cause);
	// Managed trains, for commands that act on all of them.
	TArray<AFGTrain*> GetManagedTrains() const;
	void SetTrainKeepVanilla(AFGTrain* Train, bool bKeepVanilla, const FString& Cause);
	// Hold: autopilot off and no loop until Resume, which writes its job again.
	bool SetTrainHeld(AFGTrain* Train, bool bHold, const FString& Cause);

	// Gives every other platform of the same cargo type in Platform's station Platform's settings. Returns how many
	// were changed, or -1 with OutResult saying why nothing was.
	int32 CopyPlatformToStation(AFGBuildableTrainPlatformCargo* Platform, const FString& Cause, FString& OutResult);

	//~ Readouts for the menus (sent through USTRemoteCallObject). AppliedEdit is echoed for the receiving player.
	FSTPlatformView MakePlatformView(const AFGBuildableTrainPlatformCargo* Platform, int32 AppliedEdit) const;
	FSTTrainView MakeTrainView(const AFGTrain* Train, int32 AppliedEdit) const;
	// The train view's cars, stops, state and its consumer's loop numbers (UI).
	void FillTrainLayout(const FSTTrainState* State, FSTTrainView& View) const;
	// Seconds until State's train gets to its current stop: 0 docked there, -1 if not known.
	float SecondsToStop(const FSTTrainState& State) const;
	FSTStationView MakeStationView(const AFGTrainStationIdentifier* Station, int32 AppliedEdit) const;

	//~ Readouts for chat and the migration report.
	TArray<FString> DescribePlatform(const AFGBuildableTrainPlatformCargo* Platform) const;
	TArray<FString> DescribeStation(const AFGTrainStationIdentifier* Station) const;
	TArray<FString> DescribeTrain(const AFGTrain* Train) const;
	TArray<FString> DescribeStatus() const;
	// Consumers, suppliers and trains of the loop plan ("/st monitor").
	FSTMonitorView MakeMonitorView() const;
	TArray<FString> DescribeMonitor() const;
	TArray<FString> DescribeJobs() const;
	// Which consumers Train fits and why not the others.
	TArray<FString> DescribeFit(const AFGTrain* Train) const;
	// /st patterns: every consumer's pattern and the trains on its loop.
	TArray<FString> DescribePatterns() const;

	// Vehicles beside each platform of Station when Train docks there: learned if seen, else predicted. bLearned is set
	// when it was seen. Several maps come back when the train may arrive either way round and hasn't been seen.
	TArray<TArray<AFGRailroadVehicle*>> GetDockMaps(const AFGTrain* Train, const AFGTrainStationIdentifier* Station, bool* bLearned = nullptr) const;

	// Estimated travel time from From to To. [s]
	// bPushPull: the train has a locomotive at each end, so it may reverse out and arrive from a station's far end.
	float EstimateTravelSeconds(const AFGTrainStationIdentifier* From, const AFGTrainStationIdentifier* To, bool bPushPull = false) const;
	// True if Train has locomotives that can dock facing both ways.
	static bool IsPushPull(const AFGTrain* Train);
	float EstimateStaySeconds(const AFGTrainStationIdentifier* Station) const;

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
	// Runtime only: the last set-up preview per station and what it covered (1 name, 2 platforms, 3 both).
	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, double> mSetupPreviewAt;
	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, uint8> mSetupPreviewMode;
	// Steps the staged station naming of the migration preview and of /st setup all.
	FTimerHandle mSetupTimer;
	// Lines for the station menu's set-up section: a preview, or what the last apply did.
	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, TArray<FString>> mSetupLines;
	// Preview (bApply false) or apply the set-up of Station: its name (bRename) and/or its platforms. Returns lines.
	TArray<FString> SetUpStation(AFGTrainStationIdentifier* Station, bool bApply, bool bRename, bool bPlatforms, const FString& Cause);
	// Sets one platform's role and items from what it is connected to. Returns what it did.
	FString AutodetectPlatform(AFGBuildableTrainPlatformCargo* Platform, const FString& Cause);
	// Autodetect for every cargo platform of Station, set up already or not (station menu). Returns what it did.
	FString AutodetectStation(AFGTrainStationIdentifier* Station, const FString& Cause);
	// Starts the set-up of every station with cargo platforms, a few per step; the report goes to
	// Saved/SmartTrains/StationSetup-*.csv. Returns chat lines.
	TArray<FString> SetUpAllStations(bool bApply, bool bRename, const FString& Cause);
	// One step of SetUpAllStations; true when it is done (or none runs).
	bool StepSetupAll(double BudgetMs);
	// Runs the staged set-up steps until they are done.
	void StartSetupSteps();
	// Saves from before 0.14.0 kept no machines per platform: each platform with a role is traced once, a few per step,
	// after loading. True when done.
	bool StepMachineFill(double BudgetMs);
	// Set once every platform of a save from before 0.14.0 has had its one trace.
	UPROPERTY(SaveGame)
	bool bMachinesFilled = false;
	// Runtime only: platforms still to trace for the fill-in.
	TArray<TWeakObjectPtr<AFGBuildableTrainPlatformCargo>> mMachineFillQueue;
	// /st setup format|words: changes mNaming. Returns false with OutMessage on bad input.
	bool SetNaming(const TArray<FString>& Words, const FString& Cause, FString& OutMessage);

	//~ The loop plan (STLoops.cpp), as last worked out.
	const TArray<FSTConsumer>& GetConsumers() const { return mConsumers; }
	const TArray<FSTSupplier>& GetSuppliers() const { return mSuppliers; }
	// The consumer whose station is Station, or nullptr.
	const FSTConsumer* FindConsumer(const AFGTrainStationIdentifier* Station) const;
	// Whether Train's cars line up with Consumer's pattern (a freight car of the right kind beside every slot's
	// platform); OutCars gets the car beside each slot, OutWhy why not.
	bool FitsConsumer(const AFGTrain* Train, const FSTConsumer& Consumer, TArray<AFGFreightWagon*>& OutCars, FString& OutWhy) const;

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
	// Fast checks of every managed train: docking, job progress, external timetable changes. [0.25 s]
	void MonitorTrains();
	// Manage new trains (setting): new trains without a timetable, once their consist is settled. [5 s]
	void ManageNewTrains();
	// Ends State's job at once; the next loop check gives the train its loop again.
	void EndJob(FSTTrainState& State, const TCHAR* Why);

	//~ Loops (STLoops.cpp).
	// Every LoopSeconds: one factory scan, the consumers and suppliers, a new split when its inputs changed, and each
	// managed train's job made to match.
	void CheckLoops();
	// What the factory behind one station uses or makes at most (one station per check, oldest first).
	void StepRateScan();
	// Consumers and suppliers from the platform settings and the scanned rates; round trips and needs.
	void BuildLoopModel();
	// Splits the trains over the consumers by need and gives each train its consumer, moving as few as it can.
	void AssignTrains();
	// Gives State's train the job its consumer calls for: its loop, or first what switching patterns needs. After is
	// the kind of job it just finished (Loop: none): after a Deliver whatever it still holds goes to a Sink, after a
	// Sink it starts its loop with what is left.
	void UpdateTrainJob(FSTTrainState& State, ESTJobKind After = ESTJobKind::Loop);
	// State's loop for Consumer: its pattern's cars, the supplier each car loads at, and the stops. False with OutWhy
	// if no stop could be made.
	bool MakeLoopJob(const FSTTrainState& State, const FSTConsumer& Consumer, FSTJob& OutJob, FString& OutWhy) const;
	// What State's train holds that its new loop doesn't carry in that car: false if nothing.
	bool HoldsOtherCargo(const FSTTrainState& State, const FSTJob& Loop) const;
	// Unloads (once) what State's train holds at Station's unloading platforms of the right items (Deliver), or at its
	// Sink platforms (Sink). False if no car of it stands beside such a platform holding cargo.
	bool MakeUnloadJob(const FSTTrainState& State, AFGTrainStationIdentifier* Station, ESTJobKind Kind, FSTJob& OutJob) const;
	// The nearest station with a Sink platform State's cargo can go to, or null.
	AFGTrainStationIdentifier* FindSinkStation(const FSTTrainState& State) const;

	// Gives State's train Job (or only logs it in observe-only mode).
	void CommitJob(FSTTrainState& State, FSTJob&& Job, const FString& Line);
	// Writes State's job into its train's timetable and lets it drive.
	void WriteJob(FSTTrainState& State, const TCHAR* Why);
	// Ends State's job after its last leg (Deliver, Sink); the train gets its next job at once.
	void FinishJob(FSTTrainState& State);
	// Switches the train's autopilot off where it stands, with Why kept for readouts.
	void Park(FSTTrainState& State, const TCHAR* Why);

	//~ Depot signals (Docs/Architecture.md, "Depot signals"): trains a consumer's use doesn't need now wait there.
	// Measures each consumer's Use and sets Wanted; wakes depot trains of consumers that need them back.
	void UpdateUse();
	// After a round of its loop: sends State's empty train to a depot signal if its consumer runs more trains than its use
	// needs and a free spot fits it. True if it went.
	bool MaybeSendToDepot(FSTTrainState& State);
	// Sends State's train to the nearest free depot signal whose passing siding fits it. False if there is none or no
	// way to one.
	bool SendToDepotSignal(FSTTrainState& State, const TCHAR* Why);
	// Back to its loop from a depot signal.
	void WakeFromDepot(FSTTrainState& State, const TCHAR* Why);
	// A depot train's drive: parked once it stands at its spot; a drive that ends elsewhere puts it back on its loop.
	void CheckDepotDrive(FSTTrainState& State);
	struct FDepotTrip
	{
		TWeakObjectPtr<class ASTBuildableDepotSignal> Signal;
		double SentAt = 0.0;
		// Where its front stops, the way there (track exits in order) and the signal it stops before (STDepots::Check).
		TWeakObjectPtr<class UFGRailroadTrackConnectionComponent> Goal;
		TArray<TWeakObjectPtr<class UFGRailroadTrackConnectionComponent>> Route;
		TWeakObjectPtr<class AFGBuildableRailroadSignal> StopSignal;
		// SmartTrains drives it (no Auto Trains): along the game's signals, with StopSignal held at stop.
		bool bOwnDrive = false;
		// When SmartTrains last gave it the path (bOwnDrive).
		double PathSetAt = 0.0;
	};
	// Auto Trains drives the train when it is loaded, else SmartTrains along the game's signals. Start: false if it
	// can't. State: as Auto Trains reports it, or (own drive) arrived once it stands near the goal, the path given again if
	// the game dropped it. End: lets StopSignal go, and with bCancel stops an Auto Trains drive.
	bool StartDepotDrive(AFGTrain* Train, FDepotTrip& Trip);
	STDepots::EDriveState GetDepotDriveState(AFGTrain* Train, FDepotTrip& Trip);
	void EndDepotDrive(AFGTrain* Train, const FDepotTrip& Trip, bool bCancel);
	// A train parked on a depot spot with another train (or one heading there) ahead of it on its passing siding.
	bool IsBoxedInAtDepot(const FSTTrainState& State, const TArray<struct FSTDepotSpot>& Spots) const;
	TMap<TWeakObjectPtr<AFGTrain>, FDepotTrip> mDepotTrips;
	// Depot signals no way was found to for a train, and until when (Now()) they aren't offered to it again.
	TMap<TPair<const AFGTrain*, const class ASTBuildableDepotSignal*>, double> mDepotNoWay;
	// Each consumer's averaged use, kept across loop models, and when it was last sampled. [Now()]
	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, double> mUse;
	double mUseSampledAt = -1.0;
	FTimerHandle mUnlockTimer;

public:
	// One line per depot signal: its spot, or why it can't be used, and the train using it (/st depots).
	TArray<FString> DescribeDepots() const;

private:

	// Called when State's train docks at Station / leaves it.
	void BeginDock(FSTTrainState& State, AFGTrainStationIdentifier* Station);
	void OnDocked(FSTTrainState& State, AFGTrainStationIdentifier* Station);
	void OnUndocked(FSTTrainState& State, AFGTrainStationIdentifier* Station, double Stay);
	// The stop at Station is over after Stay seconds: what moved is counted and the job moves on to its next leg (a
	// loop starts again, other jobs end).
	void FinishLeg(FSTTrainState& State, AFGTrainStationIdentifier* Station, double Stay);
	// One line per cargo platform of Station: position, load or unload, role, slots used and what it holds. For logs.
	FString DescribeStationPlatforms(const AFGTrainStationIdentifier* Station) const;
	// Records which vehicle is beside each platform of Station now.
	void LearnDockMap(AFGTrain* Train, AFGTrainStationIdentifier* Station);
	void LearnTrip(AFGTrainStationIdentifier* From, AFGTrainStationIdentifier* To, float Seconds);
	void LearnStay(AFGTrainStationIdentifier* Station, float Seconds);
	// Adds a line to the station's recent job history.
	void AddHistory(const AFGTrainStationIdentifier* Station, const FString& Line);

	// One step of the travel distance search.
	void StepTravel();
	void ReadSettings();

	// Shows the loop in the train's name (setting) or puts the original back.
	void UpdateTrainName(FSTTrainState& State);

	// Returns true if Train may serve Station (same rail network).
	bool CanServe(const AFGTrain* Train, const AFGTrainStationIdentifier* Station) const;

	// Items carried by Train's freight cars, by item class.
	static TMap<UClass*, int32> GetCargo(const AFGTrain* Train);
	// What a managed train does, for readouts: its loop, its migration step, or why it has none.
	FString DescribeWork(const FSTTrainState& State) const;
	// Once a minute: every managed train in one category (FLEET line).
	void LogFleet();
	double mFleetLoggedAt = 0.0;
	// "Items: station -> station" for a job's name.
	static FString SummarizeLegs(const TArray<FSTJobLeg>& Legs);

	double Now() const;

	UPROPERTY(SaveGame)
	TArray<FSTPlatformConfig> mPlatforms;

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

	// Now() when the world was saved, so the clock carries on after loading instead of restarting at 0. [s]
	UPROPERTY(SaveGame)
	double mClockAtSave = 0.0;

	// 1 once jobs from before 0.50.0 (trips of the old dispatcher) were dropped for loops.
	UPROPERTY(SaveGame)
	int32 mLoopsVersion = 0;

	// What the factory behind a station uses (In) and makes (Out) at most per item, and when it was scanned. [/min,
	// m³ for fluids]
	struct FStationRates
	{
		TMap<TWeakObjectPtr<UClass>, double> In;
		TMap<TWeakObjectPtr<UClass>, double> Out;
		double ScannedAt = -1.0e9;
	};
	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, FStationRates> mStationRates;
	// Stations whose platform settings changed: scanned again first.
	TSet<TWeakObjectPtr<AFGTrainStationIdentifier>> mRatesDirty;
	// The loop plan.
	TArray<FSTConsumer> mConsumers;
	TArray<FSTSupplier> mSuppliers;
	// The split's inputs when the trains were last assigned (consumers, slots, rates, trains): a new split only when it
	// changes (Docs/Architecture.md, "Rebalancing").
	FString mSplitInputs;
	FTimerHandle mLoopTimer;
	// When the consumers and suppliers were last worked out. [Now() s]
	double mModelAt = -1.0e9;
	// Trains with no state yet: vehicles when first seen and since when (Now()).
	TMap<TWeakObjectPtr<AFGTrain>, TPair<int32, double>> mNewTrains;
	FTimerHandle mNewTrainTimer;

	FSTSettings mSettings;
	// Recent job lines per station, newest last.
	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, TArray<FString>> mHistory;
	// Per train: the amount of each work item in its car when the current dock began (index = work of the leg).
	TMap<TWeakObjectPtr<AFGTrain>, TArray<int32>> mDockStartAmounts;
	// Added to the world's time (which starts at 0 each session) to make Now(). [s]
	double mClockBase = 0.0;
	// The settings have been read once (the first read logs them all).
	bool bSettingsRead = false;
	// Last "would" line logged per train in observe-only mode.
	TMap<TWeakObjectPtr<AFGTrain>, FString> mObserved;
	// Trains whose dock map still has to be read during the current dock.
	TSet<TWeakObjectPtr<AFGTrain>> mPendingDockMaps;
	FSTTravel* mTravel = nullptr;
	// When the monitor report was last logged. [game s]
	double mLastMonitorLog = 0.0;
	// Auto Trains travel-time questions left this loop check (each is a search over the whole network).
	mutable int32 mAtTravelCallsLeft = 0;
	// Auto Trains' travel times between stations, and when they were asked. [s]
	mutable TMap<TPair<TWeakObjectPtr<AFGTrainStationIdentifier>, TWeakObjectPtr<AFGTrainStationIdentifier>>, TPair<float, double>> mAtTravel;
	FTimerHandle mMonitorTimer;
	FTimerHandle mTravelTimer;
	// The loop plan is worked out again at the next check (a train or platform changed).
	bool bCheckSoon = false;

public:
	// SmartTrains.CheckTravel: for every managed train docked at a station, compares SmartTrains' route search with the
	// game's pathfinder for every station, and logs each pair they disagree on (TRAVELCHECK lines) and a summary.
	void CheckTravelAgainstGame() const;
};
