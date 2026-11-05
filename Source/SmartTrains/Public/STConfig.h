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
	float MaxTripMinutes = 20.f;
	// After this long carrying cargo no trip wants, it may go to any consumer with room or a sink. [min]
	float LeftoverMinutes = 30.f;
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
};
