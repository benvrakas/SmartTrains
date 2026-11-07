#include "STConfig.h"
#include "Configuration/ConfigManager.h"
#include "Configuration/Properties/ConfigPropertyBool.h"
#include "Configuration/Properties/ConfigPropertyFloat.h"
#include "Configuration/Properties/ConfigPropertySection.h"
#include "Configuration/Properties/WidgetExtension/CP_Float.h"
#include "Configuration/Properties/WidgetExtension/CP_Section.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "SmartTrains.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

namespace
{
	struct FBoolSetting
	{
		const TCHAR* Key;
		bool FSTSettings::* Field;
		FText Name;
		FText Tooltip;
	};

	struct FNumberSetting
	{
		const TCHAR* Key;
		float FSTSettings::* Field;
		float Min;
		float Max;
		FText Name;
		FText Tooltip;
	};

	const TArray<FBoolSetting>& GetBoolSettings()
	{
		static const TArray<FBoolSetting> Settings = {
			{TEXT("ObserveOnly"), &FSTSettings::bObserveOnly, LOCTEXT("ObserveOnly", "Observe only"),
				LOCTEXT("ObserveOnlyTip", "SmartTrains works out jobs for managed trains and writes them to the log (and /st jobs), but never changes a train's timetable or autopilot. Use it to check what it would do before letting it drive.")},
			{TEXT("BetaTools"), &FSTSettings::bBetaTools, LOCTEXT("BetaTools", "Beta testing tools"),
				LOCTEXT("BetaToolsTip", "Enables /st testcopy (saves the world as a separate session to test in) and /st backup (a timestamped save of this session).")},
			{TEXT("DetailedLog"), &FSTSettings::bDetailedLog, LOCTEXT("DetailedLog", "Detailed log"),
				LOCTEXT("DetailedLogTip", "Writes much more to FactoryGame.log: every dispatch round, why each free train got no trip, re-plans, connection traces and dock maps. Turn it on to report a problem.")},
			{TEXT("JobNames"), &FSTSettings::bJobNames, LOCTEXT("JobNames", "Show jobs in train names"),
				LOCTEXT("JobNamesTip", "Renames each managed train to show what it is doing, e.g. \"ST: Iron Plate -> Steel Mill\". The original name comes back when this is turned off or the train is no longer managed.")},
			{TEXT("LeaveAtOnce"), &FSTSettings::bLeaveAtOnce, LOCTEXT("LeaveAtOnce", "Trains leave stations at once"),
				LOCTEXT("LeaveAtOnceTip", "Every stop is load/unload once: a train moves what it can in one transfer and leaves. A train with a trip also starts it at once instead of holding in the station while its next stop is busy or Auto Trains sees no way yet. Off: deliveries and pickups wait up to their longest wait, and trains hold in stations.")},
			{TEXT("RepeatLastJob"), &FSTSettings::bRepeatLastJob, LOCTEXT("RepeatLastJob", "Repeat the last job instead of parking"),
				LOCTEXT("RepeatLastJobTip", "A train with no new trip runs its last job again, like a timetable loop, instead of parking in a station. It takes a new trip as soon as one is worth it. Off: idle trains park and move out of the way when a station is needed.")},
			{TEXT("HorizonPlanner"), &FSTSettings::bHorizonPlanner, LOCTEXT("HorizonPlanner", "Horizon planner (beta)"),
				LOCTEXT("HorizonPlannerTip", "Plans all free trains together against a forecast of every platform for the next hour, so the platforms that will run short soonest are served first and a train may be emptied to serve one. Off: the classic planner, which gives each free train its best next trip.")},
			{TEXT("Roster"), &FSTSettings::bRoster, LOCTEXT("Roster", "Steady roster (beta)"),
				LOCTEXT("RosterTip", "With the horizon planner: the flow plan's routes are shared out over the trains that fit them, and each runs its routes in turn like a timetable, so trains keep doing the same thing while rates stay the same. The horizon planner only plans trains without a roster trip worth running. Off: the horizon planner plans every free train.")},
			{TEXT("RequestFallback"), &FSTSettings::bRequestFallback, LOCTEXT("RequestFallback", "Request fallback"),
				LOCTEXT("RequestFallbackTip", "A demand platform (Request a train) that would fall below its target before any train could bring it gets a train emptied for it. Works with either planner; with the horizon planner each request it makes shows something the planner missed.")},
			{TEXT("AdminsOnly"), &FSTSettings::bAdminsOnly, LOCTEXT("AdminsOnly", "Only admins change SmartTrains"),
				LOCTEXT("AdminsOnlyTip", "In multiplayer, only the host (or, on a dedicated server, its admins) can change platform, station and train settings. Server settings, migration, test copies and backups are always admin only. On a dedicated server, change settings in game with /st set.")},
		};
		return Settings;
	}

	const TArray<FNumberSetting>& GetNumberSettings()
	{
		static const TArray<FNumberSetting> Settings = {
			{TEXT("DispatchSeconds"), &FSTSettings::DispatchSeconds, 1.f, 30.f, LOCTEXT("Dispatch", "Dispatch interval (seconds)"),
				LOCTEXT("DispatchTip", "How often free trains are given jobs, 1 to 30 seconds. Trains finishing a job are also handled as they dock.")},
			{TEXT("PlanSeconds"), &FSTSettings::PlanSeconds, 5.f, 600.f, LOCTEXT("Plan", "Flow plan interval (seconds)"),
				LOCTEXT("PlanTip", "How often SmartTrains works out which suppliers should feed which consumers, 5 to 600 seconds.")},
			{TEXT("RateWindowMinutes"), &FSTSettings::RateWindowMinutes, 1.f, 60.f, LOCTEXT("RateWindow", "Rate averaging window (minutes)"),
				LOCTEXT("RateWindowTip", "Platform supply and demand rates are averaged over this long, 1 to 60 minutes. Longer is steadier; shorter reacts faster to changes in the factory.")},
			{TEXT("AverageSpeedKmh"), &FSTSettings::AverageSpeedKmh, 20.f, 300.f, LOCTEXT("Speed", "Speed estimate (km/h)"),
				LOCTEXT("SpeedTip", "Average speed used to estimate travel times between stations until a trip between them has been timed, 20 to 300 km/h.")},
			{TEXT("DefaultStaySeconds"), &FSTSettings::DefaultStaySeconds, 0.f, 600.f, LOCTEXT("Stay", "Station stay estimate (seconds)"),
				LOCTEXT("StayTip", "How long a stop is expected to take until stays at that station have been timed, 0 to 600 seconds.")},
			{TEXT("LoadWaitSeconds"), &FSTSettings::LoadWaitSeconds, 0.f, 600.f, LOCTEXT("LoadWait", "Pickup: longest wait (seconds)"),
				LOCTEXT("LoadWaitTip", "A train picking up leaves when its cars are full or after this long, 0 to 600 seconds.")},
			{TEXT("UnloadWaitSeconds"), &FSTSettings::UnloadWaitSeconds, 0.f, 1800.f, LOCTEXT("UnloadWait", "Delivery: longest wait (seconds)"),
				LOCTEXT("UnloadWaitTip", "A train delivering leaves when its cars are empty or after this long, 0 to 1800 seconds. Whatever is left is delivered somewhere else.")},
			{TEXT("ReplanThreshold"), &FSTSettings::ReplanThreshold, 1.f, 3.f, LOCTEXT("Replan", "Re-plan threshold (x)"),
				LOCTEXT("ReplanTip", "After each stop the rest of a trip is planned again. Stops are added to it only if the trip then scores this many times higher, 1 to 3. Higher keeps trips steadier; 1 takes every improvement. Stops that can no longer work are always dropped.")},
			{TEXT("PlanBudgetMs"), &FSTSettings::PlanBudgetMs, 5.f, 200.f, LOCTEXT("Budget", "Planning time per round (ms)"),
				LOCTEXT("BudgetTip", "Most time one dispatch round may spend planning trips, 5 to 200 milliseconds. Trains not planned in time wait for the next round. Raise it on big networks if trains wait long for jobs; lower it if the game stutters every few seconds.")},
			{TEXT("MaxStops"), &FSTSettings::MaxStops, 2.f, 8.f, LOCTEXT("MaxStops", "Most stops per trip"),
				LOCTEXT("MaxStopsTip", "A trip may visit up to this many stations, 2 to 8. 2 means only direct trips: one pickup, one delivery.")},
			{TEXT("MaxTripMinutes"), &FSTSettings::MaxTripMinutes, 5.f, 120.f, LOCTEXT("MaxTrip", "Longest trip (minutes)"),
				LOCTEXT("MaxTripTip", "Trips are planned at most this long, 5 to 120 minutes. Stock forecasts get unreliable over long trips.")},
			{TEXT("LeftoverMinutes"), &FSTSettings::LeftoverMinutes, 5.f, 240.f, LOCTEXT("Leftover", "Unwanted cargo after (minutes)"),
				LOCTEXT("LeftoverTip", "Cargo that no consumer below its target wants for this long may go to any consumer with room, or to an any-item sink, 5 to 240 minutes.")},
			{TEXT("QueueTrains"), &FSTSettings::QueueTrains, 0.f, 10.f, LOCTEXT("QueueTrains", "Most trains waiting before a station"),
				LOCTEXT("QueueTrainsTip", "While trains leave stations at once, no trip is planned that would make more than this many trains wait on the line before a station at the same time, the new one included. Long queues before busy stations block junctions and cause deadlocks; a shorter wait only makes a trip score lower. 0 to 10 trains; 0 means no limit.")},
			{TEXT("RequestMarginMinutes"), &FSTSettings::RequestMarginMinutes, 0.f, 60.f, LOCTEXT("RequestMargin", "Request a train this early (minutes)"),
				LOCTEXT("RequestMarginTip", "A demand platform asks for a train when it would fall below its target (Request a train: Automatic) before any train that can carry its item now could bring it. A train holding other cargo is then emptied and sent, early enough that the goods arrive this many minutes before the platform would fall below its target. 0 to 60 minutes.")},
			{TEXT("ArrivalGapSeconds"), &FSTSettings::ArrivalGapSeconds, 0.f, 300.f, LOCTEXT("ArrivalGap", "Gap between trains at a station (seconds)"),
				LOCTEXT("ArrivalGapTip", "Trips are timed so the next train arrives at a station at least this long after the last one left, 0 to 300 seconds. A train whose first stop is busy waits where it is instead of queueing on the line.")},
		};
		return Settings;
	}

	FConfigId MakeConfigId()
	{
		FConfigId Id;
		Id.ModReference = TEXT("SmartTrains");
		return Id;
	}

	const UConfigPropertySection* FindRoot(const UObject* WorldContext)
	{
		const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		const UConfigManager* Manager = GameInstance ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
		return Manager ? Manager->GetConfigurationRootSection(MakeConfigId()) : nullptr;
	}

	const TCHAR* SMLPropertyPath = TEXT("/SML/Interface/UI/Menu/Mods/ConfigProperties/");

	// Loads SML's Blueprint subclass of a config property class, e.g. BP_ConfigPropertyBool for UConfigPropertyBool.
	UClass* LoadEditorClass(const UClass* NativeClass)
	{
		const FString Name = TEXT("BP_") + NativeClass->GetName();
		const FString Path = FString::Printf(TEXT("%s%s.%s_C"), SMLPropertyPath, *Name, *Name);
		UClass* Loaded = LoadClass<UConfigProperty>(nullptr, *Path);
		if (!Loaded || !Loaded->IsChildOf(NativeClass))
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("Config: SML editor class %s not found; that setting won't be editable in the Mods menu"), *Path);
			return nullptr;
		}
		return Loaded;
	}

	// Creates an instance of EditorClass under Outer with every property value of Source.
	UConfigProperty* CloneAs(const UConfigProperty* Source, UClass* EditorClass, UObject* Outer)
	{
		UConfigProperty* Clone = NewObject<UConfigProperty>(Outer, EditorClass, NAME_None, RF_Public);
		for (TFieldIterator<FProperty> It(Source->GetClass()); It; ++It)
		{
			It->CopyCompleteValue_InContainer(Clone, Source);
		}
		return Clone;
	}
}

USTConfig::USTConfig()
{
	ConfigId = MakeConfigId();
	DisplayName = LOCTEXT("ConfigName", "SmartTrains");
	Description = LOCTEXT("ConfigDescription", "Trains without fixed timetables, dispatched from supply to demand.");

	RootSection = CreateDefaultSubobject<UConfigPropertySection>(TEXT("RootSection"));
	const FSTSettings Defaults;
	for (const FBoolSetting& Setting : GetBoolSettings())
	{
		UConfigPropertyBool* Property = CreateDefaultSubobject<UConfigPropertyBool>(Setting.Key);
		Property->DisplayName = Setting.Name;
		Property->Tooltip = Setting.Tooltip;
		Property->DefaultValue = Defaults.*Setting.Field;
		Property->Value = Defaults.*Setting.Field;
		RootSection->SectionProperties.Add(Setting.Key, Property);
	}
	for (const FNumberSetting& Setting : GetNumberSettings())
	{
		UConfigPropertyFloat* Property = CreateDefaultSubobject<UConfigPropertyFloat>(Setting.Key);
		Property->DisplayName = Setting.Name;
		Property->Tooltip = Setting.Tooltip;
		Property->DefaultValue = Defaults.*Setting.Field;
		Property->Value = Defaults.*Setting.Field;
		RootSection->SectionProperties.Add(Setting.Key, Property);
	}
}

bool USTConfig::GetRange(const FString& Key, float& OutMin, float& OutMax)
{
	for (const FNumberSetting& Setting : GetNumberSettings())
	{
		if (Key == Setting.Key)
		{
			OutMin = Setting.Min;
			OutMax = Setting.Max;
			return true;
		}
	}
	return false;
}

void USTConfig::UseSMLEditorClasses()
{
	USTConfig* Defaults = GetMutableDefault<USTConfig>();
	UConfigPropertySection* OldRoot = Defaults->RootSection;
	UClass* SectionClass = OldRoot ? LoadEditorClass(UConfigPropertySection::StaticClass()) : nullptr;
	if (!SectionClass || OldRoot->GetClass() == SectionClass)
	{
		return;
	}
	UConfigPropertySection* NewRoot = Cast<UConfigPropertySection>(CloneAs(OldRoot, SectionClass, Defaults));
	NewRoot->SectionProperties.Reset();
	if (UCP_Section* EditorSection = Cast<UCP_Section>(NewRoot))
	{
		EditorSection->WidgetType = ECP_SectionWidgetType::CPS_Vertical;
		EditorSection->HasHeader = false;
	}
	int32 Converted = 0;
	for (const TPair<FString, TObjectPtr<UConfigProperty>>& Pair : OldRoot->SectionProperties)
	{
		UConfigProperty* Old = Pair.Value;
		if (!Old)
		{
			continue;
		}
		if (UClass* EditorClass = LoadEditorClass(Old->GetClass()))
		{
			UConfigProperty* Clone = CloneAs(Old, EditorClass, NewRoot);
			UCP_Float* Float = Cast<UCP_Float>(Clone);
			float Min = 0.f;
			float Max = 1.f;
			if (Float && GetRange(Pair.Key, Min, Max))
			{
				Float->MinValue = Min;
				Float->MaxValue = Max;
			}
			NewRoot->SectionProperties.Add(Pair.Key, Clone);
			++Converted;
			continue;
		}
		NewRoot->SectionProperties.Add(Pair.Key, Old);
	}
	Defaults->RootSection = NewRoot;
	UE_LOG(LogSmartTrains, Log, TEXT("Config: %d of %d settings use SML's editor widgets"), Converted, OldRoot->SectionProperties.Num());
}

bool USTConfig::Read(const UObject* WorldContext, FSTSettings& OutSettings)
{
	const UConfigPropertySection* Root = FindRoot(WorldContext);
	if (!Root)
	{
		return false;
	}
	const FSTSettings Defaults;
	for (const FBoolSetting& Setting : GetBoolSettings())
	{
		const TObjectPtr<UConfigProperty>* Found = Root->SectionProperties.Find(Setting.Key);
		const UConfigPropertyBool* Property = Found ? Cast<UConfigPropertyBool>(Found->Get()) : nullptr;
		OutSettings.*Setting.Field = Property ? Property->Value : Defaults.*Setting.Field;
	}
	for (const FNumberSetting& Setting : GetNumberSettings())
	{
		const TObjectPtr<UConfigProperty>* Found = Root->SectionProperties.Find(Setting.Key);
		const UConfigPropertyFloat* Property = Found ? Cast<UConfigPropertyFloat>(Found->Get()) : nullptr;
		OutSettings.*Setting.Field = FMath::Clamp(Property ? Property->Value : Defaults.*Setting.Field, Setting.Min, Setting.Max);
	}
	return true;
}

TArray<FString> USTConfig::DescribeForChat(const FSTSettings& Settings)
{
	TArray<FString> Out;
	const FSTSettings Defaults;
	for (const FBoolSetting& Setting : GetBoolSettings())
	{
		const bool bValue = Settings.*Setting.Field;
		Out.Add(FString::Printf(TEXT("%s%s = %s   (%s)"), bValue != Defaults.*Setting.Field ? TEXT("* ") : TEXT(""), Setting.Key,
			bValue ? TEXT("on") : TEXT("off"), *Setting.Name.ToString()));
	}
	for (const FNumberSetting& Setting : GetNumberSettings())
	{
		const float Value = Settings.*Setting.Field;
		Out.Add(FString::Printf(TEXT("%s%s = %g   (%s, %g to %g)"), !FMath::IsNearlyEqual(Value, Defaults.*Setting.Field) ? TEXT("* ") : TEXT(""), Setting.Key,
			Value, *Setting.Name.ToString(), Setting.Min, Setting.Max));
	}
	return Out;
}

bool USTConfig::Write(const UObject* WorldContext, const FString& Key, const FString& Value, FString& OutMessage)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UConfigManager* Manager = GameInstance ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
	UConfigPropertySection* Root = Manager ? Manager->GetConfigurationRootSection(MakeConfigId()) : nullptr;
	if (!Root)
	{
		OutMessage = TEXT("SmartTrains' configuration isn't available on this machine.");
		UE_LOG(LogSmartTrains, Warning, TEXT("Settings: write of %s failed: configuration not found"), *Key);
		return false;
	}

	// The setting: an exact key, else the only key or name containing Key.
	const FString Wanted = Key.TrimStartAndEnd();
	TArray<FString> Matches;
	FString Exact;
	auto Consider = [&](const TCHAR* SettingKey, const FText& Name)
	{
		if (Wanted.Equals(SettingKey, ESearchCase::IgnoreCase))
		{
			Exact = SettingKey;
		}
		else if (FString(SettingKey).Contains(Wanted, ESearchCase::IgnoreCase) || Name.ToString().Contains(Wanted, ESearchCase::IgnoreCase))
		{
			Matches.Add(SettingKey);
		}
	};
	for (const FBoolSetting& Setting : GetBoolSettings())
	{
		Consider(Setting.Key, Setting.Name);
	}
	for (const FNumberSetting& Setting : GetNumberSettings())
	{
		Consider(Setting.Key, Setting.Name);
	}
	const FString Chosen = !Exact.IsEmpty() ? Exact : (Matches.Num() == 1 ? Matches[0] : FString());
	if (Chosen.IsEmpty())
	{
		OutMessage = Matches.Num() == 0 ? FString::Printf(TEXT("No setting called '%s'; /st settings lists them."), *Wanted)
			: FString::Printf(TEXT("'%s' matches %s; use the full key."), *Wanted, *FString::Join(Matches, TEXT(", ")));
		return false;
	}

	const TObjectPtr<UConfigProperty>* Found = Root->SectionProperties.Find(Chosen);
	UConfigProperty* Property = Found ? Found->Get() : nullptr;
	FString Shown;
	if (UConfigPropertyBool* Bool = Cast<UConfigPropertyBool>(Property))
	{
		const FString Word = Value.TrimStartAndEnd();
		const bool bOn = Word.Equals(TEXT("on"), ESearchCase::IgnoreCase) || Word.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Word.Equals(TEXT("yes"), ESearchCase::IgnoreCase) || Word == TEXT("1");
		const bool bOff = Word.Equals(TEXT("off"), ESearchCase::IgnoreCase) || Word.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Word.Equals(TEXT("no"), ESearchCase::IgnoreCase) || Word == TEXT("0");
		if (!bOn && !bOff)
		{
			OutMessage = FString::Printf(TEXT("%s takes on or off."), *Chosen);
			return false;
		}
		Bool->Value = bOn;
		Shown = bOn ? TEXT("on") : TEXT("off");
	}
	else if (UConfigPropertyFloat* Float = Cast<UConfigPropertyFloat>(Property))
	{
		float Min = 0.f;
		float Max = 0.f;
		const FString Number = Value.TrimStartAndEnd();
		if (!GetRange(Chosen, Min, Max) || !Number.IsNumeric())
		{
			OutMessage = FString::Printf(TEXT("%s takes a number from %g to %g."), *Chosen, Min, Max);
			return false;
		}
		const float Parsed = FCString::Atof(*Number);
		if (Parsed < Min || Parsed > Max)
		{
			OutMessage = FString::Printf(TEXT("%s takes a number from %g to %g."), *Chosen, Min, Max);
			return false;
		}
		Float->Value = Parsed;
		Shown = FString::Printf(TEXT("%g"), Parsed);
	}
	else
	{
		OutMessage = FString::Printf(TEXT("%s isn't in the configuration."), *Chosen);
		UE_LOG(LogSmartTrains, Warning, TEXT("Settings: write of %s failed: property missing or of an unexpected class"), *Chosen);
		return false;
	}
	Property->MarkDirty();
	Manager->MarkConfigurationDirty(MakeConfigId());
	Manager->FlushPendingSaves();
	OutMessage = FString::Printf(TEXT("%s is now %s."), *Chosen, *Shown);
	return true;
}

TArray<FString> USTConfig::Describe(const FSTSettings& Settings)
{
	TArray<FString> Out;
	for (const FBoolSetting& Setting : GetBoolSettings())
	{
		Out.Add(FString::Printf(TEXT("%s=%s"), Setting.Key, Settings.*Setting.Field ? TEXT("on") : TEXT("off")));
	}
	for (const FNumberSetting& Setting : GetNumberSettings())
	{
		Out.Add(FString::Printf(TEXT("%s=%g"), Setting.Key, Settings.*Setting.Field));
	}
	return Out;
}

#undef LOCTEXT_NAMESPACE
