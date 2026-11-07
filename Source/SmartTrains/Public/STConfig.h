#pragma once

#include "CoreMinimal.h"
#include "Configuration/ModConfiguration.h"
#include "STConfig.generated.h"

// SmartTrains settings, read live from SML's Mods menu (the host's in multiplayer).
struct FSTSettings
{
	// Works out jobs and logs them but never touches a train.
	bool bObserveOnly = false;
	// Enables /st testcopy and /st backup.
	bool bBetaTools = true;
	// Writes SmartTrains' Verbose lines (planner rounds, re-plans, traces, dock maps) to the log.
	bool bDetailedLog = false;
	// Shows each managed train's job in its name.
	bool bJobNames = false;
	// Only admins (the host, or dedicated server admins) may change platform, station and train settings.
	bool bAdminsOnly = false;
	// Every stop is load/unload once, and a train with a trip leaves at once instead of holding in the station.
	bool bLeaveAtOnce = true;
	// A train with no new trip runs its last job again instead of parking.
	bool bRepeatLastJob = true;
	// How often jobs are handed out. [s]
	float DispatchSeconds = 3.f;
	// How often the flow plan is worked out again. [s]
	float PlanSeconds = 30.f;
	// Window over which platform rates are averaged. [min]
	float RateWindowMinutes = 5.f;
	// Average speed used for travel times until trips have been timed. [km/h]
	float AverageSpeedKmh = 80.f;
	// Stay assumed at a station until stays there have been timed. [s]
	float DefaultStaySeconds = 40.f;
	// Longest wait at a pickup for the cars to fill. [s]
	float LoadWaitSeconds = 30.f;
	// Longest wait at a delivery for the platform to take everything. [s]
	float UnloadWaitSeconds = 120.f;
	// A re-plan after a stop adds to the rest of the trip only if that scores this many times higher. [x]
	float ReplanThreshold = 1.1f;
	// Planning time per dispatch round; trains not planned in time wait for the next round. [ms]
	float PlanBudgetMs = 25.f;
	// Most stops in one trip.
	float MaxStops = 4.f;
	// Longest trip planned. [min]
	float MaxTripMinutes = 45.f;
	// After this long carrying cargo no trip wants, it may go to any consumer with room or a sink. [min]
	float LeftoverMinutes = 30.f;
	// Least time between one train leaving a station and the next arriving there. [s]
	float ArrivalGapSeconds = 30.f;
	// With bLeaveAtOnce: no trip makes more than this many trains wait on the line before a stop, itself included. 0:
	// no limit. [trains]
	float QueueTrains = 2.f;
	// Request a train setting Automatic: a demand platform asks when its stock and the deliveries on the way would fall
	// below its target within this long at its use rate. [min]
	float RequestLookaheadMinutes = 10.f;
};

// SmartTrains' mod configuration: switches and numbers shown in the Mods menu and Satisfactory Mod Manager.
UCLASS()
class SMARTTRAINS_API USTConfig : public UModConfiguration
{
	GENERATED_BODY()

public:
	USTConfig();

	// Sets OutMin and OutMax to the accepted range of the number setting Key; returns false if Key isn't one.
	static bool GetRange(const FString& Key, float& OutMin, float& OutMax);

	// Rebuilds the default configuration with SML's Blueprint property classes, which carry the editor widgets SML's
	// Mods menu shows. Call before SML registers the configuration.
	static void UseSMLEditorClasses();

	// Fills OutSettings from the registered configuration; returns false, leaving the defaults, if it isn't available.
	static bool Read(const UObject* WorldContext, FSTSettings& OutSettings);

	// "Key=value" for every setting, for the log.
	static TArray<FString> Describe(const FSTSettings& Settings);

	// One line per setting for chat: key, value, range and name; settings off their default are marked.
	static TArray<FString> DescribeForChat(const FSTSettings& Settings);

	// Sets the setting Key (any case; a unique part of a key or name also works) to Value ("on"/"off" or a number) in
	// the registered configuration, which SML saves to this machine's config file. OutMessage says what happened.
	// Returns false if Key or Value isn't valid or the configuration isn't available.
	static bool Write(const UObject* WorldContext, const FString& Key, const FString& Value, FString& OutMessage);
};
