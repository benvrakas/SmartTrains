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
			{TEXT("MilkRuns"), &FSTSettings::bMilkRuns, LOCTEXT("MilkRuns", "Milk runs"),
				LOCTEXT("MilkRunsTip", "A train may pick up at two suppliers to fill up when neither has a full load, or split a load between two consumers when neither has room for all of it.")},
			{TEXT("JobNames"), &FSTSettings::bJobNames, LOCTEXT("JobNames", "Show jobs in train names"),
				LOCTEXT("JobNamesTip", "Renames each managed train to show what it is doing, e.g. \"ST: Iron Plate -> Steel Mill\". The original name comes back when this is turned off or the train is no longer managed.")},
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
			{TEXT("RetargetFactor"), &FSTSettings::RetargetFactor, 1.f, 10.f, LOCTEXT("Retarget", "Retarget threshold (x)"),
				LOCTEXT("RetargetTip", "An empty train already on its way to a pickup switches to a new job only if it is this many times better, 1 to 10.")},
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

#undef LOCTEXT_NAMESPACE
