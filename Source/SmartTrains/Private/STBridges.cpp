#include "STBridges.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FGRailroadSubsystem.h"
#include "FGRailroadVehicle.h"
#include "FGTrainStationIdentifier.h"
#include "SmartTrains.h"
#include "Subsystem/SubsystemActorManager.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectIterator.h"

namespace
{
	// Finds a loaded class by its name without prefix (e.g. "FCCRuleSubsystem").
	UClass* FindClassByName(const TCHAR* Name)
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == Name && !It->HasAnyClassFlags(CLASS_NewerVersionExists))
			{
				return *It;
			}
		}
		return nullptr;
	}

	UObject* GetFccSubsystem(const UWorld* World)
	{
		static TWeakObjectPtr<UClass> Cached;
		static bool bSearched = false;
		if (!bSearched)
		{
			bSearched = true;
			Cached = FindClassByName(TEXT("FCCRuleSubsystem"));
			UE_LOG(LogSmartTrains, Log, TEXT("Freight Car Control %s"), Cached.IsValid() ? TEXT("found") : TEXT("not loaded; jobs need whole-train fits"));
		}
		UClass* Class = Cached.Get();
		USubsystemActorManager* Manager = World && Class ? World->GetSubsystem<USubsystemActorManager>() : nullptr;
		return Manager && Class->IsChildOf(AModSubsystem::StaticClass()) ? Manager->K2_GetSubsystemActor(Class) : nullptr;
	}

	// Copies Value into the parameter Name of Params when its property type matches TProperty; logs a mismatch.
	template <typename TProperty, typename TValue>
	bool SetParam(const UFunction* Function, FStructOnScope& Params, const TCHAR* Name, const TValue& Value)
	{
		FProperty* Property = Function->FindPropertyByName(Name);
		if (!Property || !CastField<TProperty>(Property))
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("Reflection: %s has no parameter %s of the expected type"), *Function->GetName(), Name);
			return false;
		}
		Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Params.GetStructMemory()), &Value);
		return true;
	}

	FString ReadString(const FProperty* Property, const void* Container)
	{
		if (const FStrProperty* Str = CastField<FStrProperty>(Property))
		{
			return Str->GetPropertyValue_InContainer(Container);
		}
		if (const FTextProperty* Text = CastField<FTextProperty>(Property))
		{
			return Text->GetPropertyValue_InContainer(Container).ToString();
		}
		if (const FNameProperty* Name = CastField<FNameProperty>(Property))
		{
			return Name->GetPropertyValue_InContainer(Container).ToString();
		}
		return FString();
	}

	AFGTrainStationIdentifier* FindStationByName(const UWorld* World, const FString& Name)
	{
		AFGRailroadSubsystem* Railroad = World ? AFGRailroadSubsystem::Get(const_cast<UWorld*>(World)) : nullptr;
		TArray<AFGTrainStationIdentifier*> Stations;
		if (Railroad)
		{
			Railroad->GetAllTrainStations(Stations);
		}
		for (AFGTrainStationIdentifier* Station : Stations)
		{
			if (!Station)
			{
				continue;
			}
			// A display name, or an id: the identifier's or the station building's object name or path.
			const AActor* Building = Station->GetStation();
			if (Station->GetStationName().ToString().Equals(Name, ESearchCase::IgnoreCase) || Station->GetName() == Name || Station->GetPathName() == Name
				|| (Building && (Building->GetName() == Name || Building->GetPathName() == Name)))
			{
				return Station;
			}
		}
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogSmartTrains, Warning, TEXT("DynamicTimeTable: station '%s' of a group matches no station by name or id; such stations are skipped"), *Name);
		}
		return nullptr;
	}

	// Reads one group member (a station object, or a struct holding one or its name, plus a slot count).
	void ReadMember(const UWorld* World, const FProperty* Inner, const void* Value, STDtt::FGroup& Group)
	{
		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Inner))
		{
			if (AFGTrainStationIdentifier* Station = Cast<AFGTrainStationIdentifier>(Object->GetObjectPropertyValue(Value)))
			{
				Group.Stations.Add(Station);
				Group.TargetSlots.Add(1);
			}
			return;
		}
		const FStructProperty* Struct = CastField<FStructProperty>(Inner);
		if (!Struct)
		{
			return;
		}
		AFGTrainStationIdentifier* Station = nullptr;
		FString StationName;
		int32 Slots = 1;
		for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
		{
			const FString FieldName = It->GetName();
			if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(*It))
			{
				Station = Station ? Station : Cast<AFGTrainStationIdentifier>(Object->GetObjectPropertyValue_InContainer(Value));
			}
			else if (const FIntProperty* Int = CastField<FIntProperty>(*It))
			{
				if (FieldName.Contains(TEXT("Slot")))
				{
					Slots = Int->GetPropertyValue_InContainer(Value);
				}
			}
			else if (FieldName.Contains(TEXT("Station")) || FieldName.Contains(TEXT("Name")))
			{
				StationName = StationName.IsEmpty() ? ReadString(*It, Value) : StationName;
			}
		}
		if (!Station && !StationName.IsEmpty())
		{
			Station = FindStationByName(World, StationName);
		}
		if (Station)
		{
			Group.Stations.Add(Station);
			Group.TargetSlots.Add(FMath::Max(1, Slots));
		}
	}

	// Reads one group struct: its name and its member array.
	void ReadGroup(const UWorld* World, const UScriptStruct* Struct, const void* Value, TArray<STDtt::FGroup>& OutGroups)
	{
		STDtt::FGroup Group;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FString FieldName = It->GetName();
			if (const FArrayProperty* Array = CastField<FArrayProperty>(*It))
			{
				FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Value));
				for (int32 Index = 0; Index < Helper.Num(); ++Index)
				{
					ReadMember(World, Array->Inner, Helper.GetRawPtr(Index), Group);
				}
			}
			else if (FieldName.Contains(TEXT("Name")) && Group.Name.IsEmpty())
			{
				Group.Name = ReadString(*It, Value);
			}
		}
		OutGroups.Add(MoveTemp(Group));
	}
}

bool STFcc::IsAvailable(const UWorld* World)
{
	UObject* Subsystem = GetFccSubsystem(World);
	return Subsystem && Subsystem->FindFunction(TEXT("ExternalSetStops"));
}

bool STFcc::SetStops(const UWorld* World, AFGRailroadTimeTable* TimeTable, const TArray<FTimeTableStop>& Stops, const TArray<int32>& SkipStops,
	const TArray<AFGRailroadVehicle*>& SkipVehicles, const FString& Owner)
{
	UObject* Subsystem = GetFccSubsystem(World);
	UFunction* Function = Subsystem ? Subsystem->FindFunction(TEXT("ExternalSetStops")) : nullptr;
	if (!Function || !TimeTable)
	{
		return false;
	}
	FStructOnScope Params(Function);
	TArray<TObjectPtr<AFGRailroadVehicle>> Vehicles;
	for (AFGRailroadVehicle* Vehicle : SkipVehicles)
	{
		Vehicles.Add(Vehicle);
	}
	TObjectPtr<AFGRailroadTimeTable> TimeTablePtr = TimeTable;
	const bool bSet = SetParam<FObjectPropertyBase>(Function, Params, TEXT("TimeTable"), TimeTablePtr)
		&& SetParam<FArrayProperty>(Function, Params, TEXT("Stops"), Stops)
		&& SetParam<FArrayProperty>(Function, Params, TEXT("SkipStops"), SkipStops)
		&& SetParam<FArrayProperty>(Function, Params, TEXT("SkipVehicles"), Vehicles)
		&& SetParam<FStrProperty>(Function, Params, TEXT("RulesOwner"), Owner);
	if (!bSet)
	{
		return false;
	}
	Subsystem->ProcessEvent(Function, Params.GetStructMemory());
	const FBoolProperty* Return = CastField<FBoolProperty>(Function->GetReturnProperty());
	return Return ? Return->GetPropertyValue_InContainer(Params.GetStructMemory()) : true;
}

namespace
{
	// DynamicTimeTable's subsystem class: DTTSubsystem since its rewrite, DynamicTimeTableSubsystem before.
	UClass* FindDttClass()
	{
		UClass* Class = FindClassByName(TEXT("DTTSubsystem"));
		return Class ? Class : FindClassByName(TEXT("DynamicTimeTableSubsystem"));
	}
}

bool STDtt::IsInstalled(const UWorld* World)
{
	return FindDttClass() != nullptr;
}

bool STDtt::ReadGroups(const UWorld* World, TArray<FGroup>& OutGroups, FString& OutError)
{
	OutGroups.Reset();
	UClass* Class = FindDttClass();
	if (!Class || !World)
	{
		OutError = TEXT("DynamicTimeTable is not loaded");
		return false;
	}
	UObject* Subsystem = nullptr;
	if (Class->IsChildOf(UWorldSubsystem::StaticClass()))
	{
		Subsystem = World->GetSubsystemBase(TSubclassOf<UWorldSubsystem>(Class));
	}
	else if (Class->IsChildOf(UGameInstanceSubsystem::StaticClass()) && World->GetGameInstance())
	{
		Subsystem = World->GetGameInstance()->GetSubsystemBase(TSubclassOf<UGameInstanceSubsystem>(Class));
	}
	UFunction* Function = Subsystem ? Subsystem->FindFunction(TEXT("GetStationGroups")) : nullptr;
	if (!Function)
	{
		OutError = FString::Printf(TEXT("DynamicTimeTable subsystem %s or its GetStationGroups not found"), Subsystem ? TEXT("found but") : TEXT("missing,"));
		return false;
	}
	FStructOnScope Params(Function);
	Subsystem->ProcessEvent(Function, Params.GetStructMemory());
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		const FArrayProperty* Array = CastField<FArrayProperty>(*It);
		const FStructProperty* Inner = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
		if (!Inner || !It->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm))
		{
			continue;
		}
		FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Params.GetStructMemory()));
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			ReadGroup(World, Inner->Struct, Helper.GetRawPtr(Index), OutGroups);
		}
		UE_LOG(LogSmartTrains, Log, TEXT("DynamicTimeTable: read %d groups from %s.%s (%s)"), OutGroups.Num(), *Function->GetName(), *It->GetName(), *Inner->Struct->GetName());
		return true;
	}
	OutError = TEXT("GetStationGroups returned no array of groups");
	return false;
}
