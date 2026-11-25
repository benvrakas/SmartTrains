#pragma once

#include "CoreMinimal.h"
#include "Configuration/ModConfiguration.h"
#include "STConfig.generated.h"

// SmartTrains settings, read live from SML's Mods menu (the host's in multiplayer).
struct FSTSettings
{
	// Works out loops and logs them but never touches a train.
	bool bObserveOnly = false;
	// Enables /st testcopy and /st backup.
	bool bBetaTools = true;
	// Writes SmartTrains' Verbose lines (loop plans, traces, dock maps) to the log.
	bool bDetailedLog = false;
	// Shows each managed train's loop in its name.
	bool bJobNames = false;
	// New trains without a timetable are managed once their consist hasn't changed for a while.
	bool bManageNewTrains = false;
	// Only admins (the host, or dedicated server admins) may change platform, station and train settings.
	bool bAdminsOnly = false;
	// Average speed used for travel times until trips have been timed. [km/h]
	float AverageSpeedKmh = 80.f;
	// Stay assumed at a station until stays there have been timed. [s]
	float DefaultStaySeconds = 40.f;
	// Most buildings and belt segments one scan of a factory (station set-up, Autodetect, rates) visits.
	float ScanDepth = 50000.f;
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

	// Every setting's key and value as Write takes them ("on"/"off" or a number); false if the configuration isn't
	// available.
	static bool ReadValues(const UObject* WorldContext, TMap<FString, FString>& OutValues);

	// "Key=value" for every setting, for the log.
	static TArray<FString> Describe(const FSTSettings& Settings);

	// One line per setting for chat: key, value, range and name; settings off their default are marked.
	static TArray<FString> DescribeForChat(const FSTSettings& Settings);

	// Sets the setting Key (any case; a unique part of a key or name also works) to Value ("on"/"off" or a number) in
	// the registered configuration, which SML saves to this machine's config file. OutMessage says what happened.
	// Returns false if Key or Value isn't valid or the configuration isn't available.
	static bool Write(const UObject* WorldContext, const FString& Key, const FString& Value, FString& OutMessage);
};
