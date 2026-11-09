#include "STLayout.h"
#include "Buildables/FGBuildableConveyorAttachment.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatform.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGFactoryConnectionComponent.h"
#include "FGFreightWagon.h"
#include "FGInventoryComponent.h"
#include "FGLocomotive.h"
#include "FGPipeConnectionComponent.h"
#include "FGRailroadVehicle.h"
#include "FGTrain.h"
#include "FGTrainPlatformConnection.h"
#include "FGTrainStationIdentifier.h"
#include "Resources/FGItemDescriptor.h"
#include "SmartTrains.h"
#include "STTypes.h"

EFreightCargoType FSTPlatformAccess::GetCargoType(const AFGBuildableTrainPlatformCargo* Platform)
{
	return Platform ? Platform->mFreightCargoType : EFreightCargoType::FCT_NONE;
}

void FSTPlatformAccess::CountConnections(const AFGBuildableTrainPlatformCargo* Platform, int32& OutInputs, int32& OutOutputs)
{
	OutInputs = 0;
	OutOutputs = 0;
	if (!Platform)
	{
		return;
	}
	TInlineComponentArray<UFGFactoryConnectionComponent*> Belts;
	Platform->GetComponents(Belts);
	for (const UFGFactoryConnectionComponent* Belt : Belts)
	{
		if (!Belt || !Belt->IsConnected())
		{
			continue;
		}
		if (Belt->GetDirection() == EFactoryConnectionDirection::FCD_INPUT)
		{
			++OutInputs;
		}
		else if (Belt->GetDirection() == EFactoryConnectionDirection::FCD_OUTPUT)
		{
			++OutOutputs;
		}
	}
	for (const UFGPipeConnectionComponent* Pipe : Platform->mPipeInputConnections)
	{
		OutInputs += Pipe && Pipe->IsConnected() ? 1 : 0;
	}
	for (const UFGPipeConnectionComponent* Pipe : Platform->mPipeOutputConnections)
	{
		OutOutputs += Pipe && Pipe->IsConnected() ? 1 : 0;
	}
}

TArray<UFGPipeConnectionComponent*> FSTPlatformAccess::GetPipeConnections(const AFGBuildableTrainPlatformCargo* Platform, bool bInputs)
{
	TArray<UFGPipeConnectionComponent*> Out;
	if (Platform)
	{
		for (UFGPipeConnectionComponent* Pipe : bInputs ? Platform->mPipeInputConnections : Platform->mPipeOutputConnections)
		{
			if (Pipe && Pipe->IsConnected())
			{
				Out.Add(Pipe);
			}
		}
	}
	return Out;
}

int32 FSTPlatformAccess::GetAttachmentOutputIndex(const AFGBuildableConveyorAttachment* Attachment, const UFGFactoryConnectionComponent* Connection)
{
	return Attachment ? Attachment->mOutputs.IndexOfByKey(Connection) : INDEX_NONE;
}

TArray<AFGBuildableTrainPlatform*> STLayout::GetPlatforms(const AFGBuildableRailroadStation* Station)
{
	TArray<AFGBuildableTrainPlatform*> Platforms;
	UFGTrainPlatformConnection* Connection = Station ? const_cast<AFGBuildableRailroadStation*>(Station)->GetStationOutputConnection() : nullptr;
	while (Connection && Platforms.Num() < 64)
	{
		UFGTrainPlatformConnection* Other = Connection->GetConnectedTo();
		AFGBuildableTrainPlatform* Platform = Other ? Other->GetPlatformOwner() : nullptr;
		if (!Platform || Platform->IsA<AFGBuildableRailroadStation>() || Platforms.Contains(Platform))
		{
			break;
		}
		Platforms.Add(Platform);
		Connection = Platform->GetConnectionInOppositeDirection(Other);
	}
	return Platforms;
}

AFGBuildableRailroadStation* STLayout::FindStation(const AFGBuildableTrainPlatform* Platform, int32* OutIndex)
{
	if (!Platform)
	{
		return nullptr;
	}
	// Walks both ways along the chain to a station, then checks the platform is on its output side.
	TInlineComponentArray<UFGTrainPlatformConnection*> Connections;
	Platform->GetComponents(Connections);
	for (UFGTrainPlatformConnection* Connection : Connections)
	{
		UFGTrainPlatformConnection* Next = Connection;
		for (int32 Step = 0; Step < 64 && Next; ++Step)
		{
			UFGTrainPlatformConnection* Other = Next->GetConnectedTo();
			AFGBuildableTrainPlatform* Neighbour = Other ? Other->GetPlatformOwner() : nullptr;
			if (!Neighbour)
			{
				break;
			}
			if (AFGBuildableRailroadStation* Station = Cast<AFGBuildableRailroadStation>(Neighbour))
			{
				const int32 Index = GetPlatforms(Station).IndexOfByKey(Platform);
				if (Index != INDEX_NONE)
				{
					if (OutIndex)
					{
						*OutIndex = Index;
					}
					return Station;
				}
				break;
			}
			Next = Neighbour->GetConnectionInOppositeDirection(Other);
		}
	}
	return nullptr;
}

float STLayout::GetTrainLength(const AFGTrain* Train)
{
	float Length = 0.f;
	for (const AFGRailroadVehicle* Vehicle : GetVehicles(Train))
	{
		Length += Vehicle ? Vehicle->GetLength() : 0.f;
	}
	return Length;
}

TArray<AFGRailroadVehicle*> STLayout::GetVehicles(const AFGTrain* Train)
{
	TArray<AFGRailroadVehicle*> Vehicles;
	AFGRailroadVehicle* Current = Train ? Train->GetFirstVehicle() : nullptr;
	const AFGRailroadVehicle* Previous = nullptr;
	while (Current && Vehicles.Num() < 256 && !Vehicles.Contains(Current))
	{
		Vehicles.Add(Current);
		AFGRailroadVehicle* Front = Current->GetCoupledVehicleAt(ERailroadVehicleCoupler::RVC_FRONT);
		AFGRailroadVehicle* Back = Current->GetCoupledVehicleAt(ERailroadVehicleCoupler::RVC_BACK);
		AFGRailroadVehicle* Next = Front && Front != Previous ? Front : (Back != Previous ? Back : nullptr);
		Previous = Current;
		Current = Next;
	}
	return Vehicles;
}

TArray<AFGRailroadVehicle*> STLayout::GetVehiclesBehind(const AFGLocomotive* Locomotive)
{
	TArray<AFGRailroadVehicle*> Vehicles;
	if (!Locomotive)
	{
		return Vehicles;
	}
	const bool bReversed = Locomotive->IsOrientationReversed();
	AFGRailroadVehicle* Current = Locomotive->GetCoupledVehicleAt(ERailroadVehicleCoupler::RVC_BACK);
	while (Current && Vehicles.Num() < 256 && !Vehicles.Contains(Current) && Current != Locomotive)
	{
		Vehicles.Add(Current);
		const bool bSameWay = Current->IsOrientationReversed() == bReversed;
		Current = Current->GetCoupledVehicleAt(bSameWay ? ERailroadVehicleCoupler::RVC_BACK : ERailroadVehicleCoupler::RVC_FRONT);
	}
	return Vehicles;
}

TArray<AFGLocomotive*> STLayout::GetPossibleDockingLocomotives(const AFGTrain* Train)
{
	TArray<AFGLocomotive*> Result;
	if (!Train)
	{
		return Result;
	}
	if (AFGLocomotive* Master = Train->GetMultipleUnitMaster())
	{
		Result.Add(Master);
	}
	const TArray<AFGRailroadVehicle*> Vehicles = GetVehicles(Train);
	if (Vehicles.Num() == 0)
	{
		return Result;
	}
	for (AFGRailroadVehicle* End : {Vehicles[0], Vehicles.Last()})
	{
		AFGLocomotive* Locomotive = Cast<AFGLocomotive>(End);
		if (!Locomotive || Result.Contains(Locomotive))
		{
			continue;
		}
		// A locomotive leads from this end only if the rest of the train is behind it.
		if (Vehicles.Num() == 1 || GetVehiclesBehind(Locomotive).Num() == Vehicles.Num() - 1)
		{
			Result.Add(Locomotive);
		}
	}
	return Result;
}

EFreightCargoType STLayout::GetCargoType(const AFGFreightWagon* Car)
{
	const UFGInventoryComponent* Inventory = Car ? Car->GetFreightInventory() : nullptr;
	if (!Inventory || Inventory->IsEmpty())
	{
		return EFreightCargoType::FCT_NONE;
	}
	return Car->GetFreightCargoType();
}

int32 STLayout::GetCapacity(const UFGInventoryComponent* Inventory, TSubclassOf<UFGItemDescriptor> Item)
{
	if (!Inventory || !Item)
	{
		return 0;
	}
	int64 Total = 0;
	for (int32 Index = 0; Index < Inventory->GetSizeLinear(); ++Index)
	{
		Total += Inventory->GetSlotSize(Index, Item);
	}
	return static_cast<int32>(FMath::Min<int64>(Total, MAX_int32));
}

int32 STLayout::GetSlotSize(const UFGInventoryComponent* Inventory, TSubclassOf<UFGItemDescriptor> Item)
{
	if (!Item)
	{
		return 1;
	}
	if (!Inventory || Inventory->GetSizeLinear() == 0)
	{
		return FMath::Max(1, UFGItemDescriptor::GetStackSize(Item));
	}
	return FMath::Max(1, Inventory->GetSlotSize(0, Item));
}

int32 STLayout::GetWagonSlots(const AFGFreightWagon* Wagon)
{
	static const FIntProperty* Property = FindFProperty<FIntProperty>(AFGFreightWagon::StaticClass(), TEXT("mInventorySize"));
	const int32 Slots = Property && Wagon ? Property->GetPropertyValue_InContainer(Wagon) : 0;
	return Slots > 0 ? Slots : 32;
}

int64 STLayout::GetWagonFluidCapacity(const AFGFreightWagon* Wagon)
{
	const int64 Capacity = Wagon ? Wagon->GetScaledFluidStackSize() : 0;
	// Logged once, from the first wagon seen.
	static bool bLogged = false;
	if (Wagon && !bLogged)
	{
		bLogged = true;
		UE_LOG(LogSmartTrains, Log, TEXT("Freight car size: %d slots, %lld fluid (%s)"), GetWagonSlots(Wagon), Capacity, *GetNameSafe(Wagon->GetClass()));
	}
	return Capacity > 0 ? Capacity : 1600000;
}

double STLayout::GetCarCapacity(const AFGFreightWagon* Wagon, TSubclassOf<UFGItemDescriptor> Item)
{
	if (!Item)
	{
		return 0.0;
	}
	return STNames::IsFluid(Item) ? static_cast<double>(GetWagonFluidCapacity(Wagon))
		: static_cast<double>(GetWagonSlots(Wagon)) * FMath::Max(1, UFGItemDescriptor::GetStackSize(Item));
}

double STLayout::GetCarSlotAmount(const AFGFreightWagon* Wagon, TSubclassOf<UFGItemDescriptor> Item)
{
	if (!Item)
	{
		return 1.0;
	}
	return STNames::IsFluid(Item) ? static_cast<double>(GetWagonFluidCapacity(Wagon)) / GetWagonSlots(Wagon)
		: static_cast<double>(FMath::Max(1, UFGItemDescriptor::GetStackSize(Item)));
}

bool STLayout::ItemFitsCargoType(TSubclassOf<UFGItemDescriptor> Item, EFreightCargoType Type)
{
	if (Type == EFreightCargoType::FCT_NONE || !Item)
	{
		return true;
	}
	return STNames::IsFluid(Item) == (Type == EFreightCargoType::FCT_Liquid);
}

EFreightCargoType STLayout::GetPlatformCargoType(const AFGBuildableTrainPlatformCargo* Platform)
{
	return FSTPlatformAccess::GetCargoType(Platform);
}

bool STLayout::CanMoveCargo(const AFGBuildableTrainPlatformCargo* Platform)
{
	if (!Platform)
	{
		return false;
	}
	const UFGInventoryComponent* Inventory = Platform->GetInventory();
	TInlineComponentArray<UFGFactoryConnectionComponent*> Belts;
	Platform->GetComponents(Belts);
	TInlineComponentArray<UFGPipeConnectionComponent*> Pipes;
	Platform->GetComponents(Pipes);
	return Inventory && Inventory->GetSizeLinear() > 0 && Belts.Num() + Pipes.Num() > 0;
}

AFGTrainStationIdentifier* STLayout::GetIdentifier(const AFGBuildableRailroadStation* Station)
{
	return Station ? Station->GetStationIdentifier() : nullptr;
}

FString STLayout::Name(const AFGTrainStationIdentifier* Station)
{
	return Station ? Station->GetStationName().ToString() : FString(TEXT("?"));
}

FString STLayout::Name(const AFGBuildableRailroadStation* Station)
{
	return Name(GetIdentifier(Station));
}

FString STLayout::DescribePlatform(const AFGBuildableTrainPlatform* Platform)
{
	int32 Index = INDEX_NONE;
	const AFGBuildableRailroadStation* Station = Platform ? FindStation(Platform, &Index) : nullptr;
	return Station ? FString::Printf(TEXT("P%d of %s"), Index + 1, *Name(Station)) : GetNameSafe(Platform);
}

FString STLayout::Name(const AFGTrain* Train)
{
	return Train ? Train->GetTrainName().ToString() : FString(TEXT("?"));
}

FString STLayout::DescribeVehicle(const AFGTrain* Train, const AFGRailroadVehicle* Vehicle)
{
	if (!Vehicle)
	{
		return TEXT("-");
	}
	int32 CarNumber = 0;
	for (const AFGRailroadVehicle* Each : GetVehicles(Train))
	{
		if (Each && Each->IsA<AFGFreightWagon>())
		{
			++CarNumber;
		}
		if (Each == Vehicle)
		{
			return Vehicle->IsA<AFGFreightWagon>() ? FString::Printf(TEXT("car %d"), CarNumber) : FString(TEXT("loco"));
		}
	}
	return Vehicle->GetName();
}
