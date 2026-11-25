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
				LOCTEXT("ObserveOnlyTip", "SmartTrains works out each managed train's loop and writes it to the log (and /st jobs), but never changes a train's timetable or autopilot. Use it to check what it would do before letting it drive.")},
			{TEXT("BetaTools"), &FSTSettings::bBetaTools, LOCTEXT("BetaTools", "Beta testing tools"),
				LOCTEXT("BetaToolsTip", "Enables /st testcopy (saves the world as a separate session to test in) and /st backup (a timestamped save of this session).")},
			{TEXT("DetailedLog"), &FSTSettings::bDetailedLog, LOCTEXT("DetailedLog", "Detailed log"),
				LOCTEXT("DetailedLogTip", "Writes much more to FactoryGame.log: every loop plan, connection traces and dock maps. Turn it on to report a problem.")},
			{TEXT("JobNames"), &FSTSettings::bJobNames, LOCTEXT("JobNames", "Show jobs in train names"),
				LOCTEXT("JobNamesTip", "Renames each managed train to show its loop, e.g. \"ST: Iron Plate -> Steel Mill\". The original name comes back when this is turned off or the train is no longer managed.")},
			{TEXT("ManageNewTrains"), &FSTSettings::bManageNewTrains, LOCTEXT("ManageNewTrains", "Manage new trains"),
				LOCTEXT("ManageNewTrainsTip", "A new train with freight cars and no timetable is managed by SmartTrains 30 seconds after its cars last changed. A train with a timetable of its own, kept vanilla, or switched off in its locomotive menu is left alone.")},
			{TEXT("AdminsOnly"), &FSTSettings::bAdminsOnly, LOCTEXT("AdminsOnly", "Only admins change SmartTrains"),
				LOCTEXT("AdminsOnlyTip", "In multiplayer, only the host (or, on a dedicated server, its admins) can change platform, station and train settings. Server settings, migration, test copies and backups are always admin only. On a dedicated server, an admin's Mods menu changes go to the server; /st set works too.")},
			{TEXT("WaitForFullCars"), &FSTSettings::bWaitForFullCars, LOCTEXT("WaitForFullCars", "Wait for full cars"),
				LOCTEXT("WaitForFullCarsTip", "Off: a train loads what is at each supplier and unloads what fits at its consumer, once, then goes on, so no platform is held up. On: it stays at each supplier until its cars are full and at its consumer until they are empty.")},
		};
		return Settings;
	}

	const TArray<FNumberSetting>& GetNumberSettings()
	{
		static const TArray<FNumberSetting> Settings = {
			{TEXT("SinkAfterMinutes"), &FSTSettings::SinkAfterMinutes, 0.f, 240.f, LOCTEXT("SinkAfter", "Send leftovers to a Sink after (minutes)"),
				LOCTEXT("SinkAfterTip", "A train switching to another station's loop first unloads cargo its new loop doesn't carry at stations that use it. Only after this many minutes (or when no station takes it) does it take what is left to a Sink, one train per Sink at a time, 0 to 240.")},
			{TEXT("ResplitPercent"), &FSTSettings::ResplitPercent, 1.f, 100.f, LOCTEXT("Resplit", "Share trains out again after a rate change of (%)"),
				LOCTEXT("ResplitTip", "Trains are shared out over the factories again only when a factory's rate, or what its suppliers can give of it, changes by more than this since the last time (or when stations, patterns, priorities or trains change), 1 to 100. Moving a train costs at least a round, so small changes move none.")},
			{TEXT("UseAverageMinutes"), &FSTSettings::UseAverageMinutes, 1.f, 60.f, LOCTEXT("UseAverage", "Spare trains: average a factory's use over (minutes)"),
				LOCTEXT("UseAverageTip", "How long a factory has to run below its most before its spare trains park at Depot Signals (and how quickly they come back), 1 to 60 minutes. Shorter reacts faster; longer ignores short stalls.")},
			{TEXT("AverageSpeedKmh"), &FSTSettings::AverageSpeedKmh, 20.f, 300.f, LOCTEXT("Speed", "Speed estimate (km/h)"),
				LOCTEXT("SpeedTip", "Average speed used to estimate travel times between stations (for each loop's round trip) until a trip between them has been timed, 20 to 300 km/h.")},
			{TEXT("DefaultStaySeconds"), &FSTSettings::DefaultStaySeconds, 0.f, 600.f, LOCTEXT("Stay", "Station stay estimate (seconds)"),
				LOCTEXT("StayTip", "How long a stop is expected to take until stays at that station have been timed, 0 to 600 seconds.")},
			{TEXT("ScanDepth"), &FSTSettings::ScanDepth, 1000.f, 500000.f, LOCTEXT("ScanDepth", "Factory scan depth (buildings and belts)"),
				LOCTEXT("ScanDepthTip", "Station set-up, Autodetect and the rates in station names follow belts, pipes and machines through a factory for at most this many buildings and belt segments, 1,000 to 500,000. Raise it if a large factory's rate or items come out incomplete (the set-up says when a scan stopped early); lower it if setting up a station makes the game hitch.")},
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

bool USTConfig::ReadValues(const UObject* WorldContext, TMap<FString, FString>& OutValues)
{
	FSTSettings Settings;
	if (!Read(WorldContext, Settings))
	{
		return false;
	}
	OutValues.Reset();
	for (const FBoolSetting& Setting : GetBoolSettings())
	{
		OutValues.Add(Setting.Key, Settings.*Setting.Field ? TEXT("on") : TEXT("off"));
	}
	for (const FNumberSetting& Setting : GetNumberSettings())
	{
		OutValues.Add(Setting.Key, FString::SanitizeFloat(Settings.*Setting.Field));
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
