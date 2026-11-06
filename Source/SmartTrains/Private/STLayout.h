#pragma once

#include "CoreMinimal.h"
#include "FGFreightWagon.h"

class AFGBuildableRailroadStation;
class AFGBuildableTrainPlatform;
class AFGBuildableTrainPlatformCargo;
class AFGLocomotive;
class AFGRailroadVehicle;
class AFGTrain;
class AFGTrainStationIdentifier;
class UFGItemDescriptor;

// Reads protected cargo platform fields (friend of AFGBuildableTrainPlatformCargo through Config/AccessTransformers.ini).
class FSTPlatformAccess
{
public:
	static EFreightCargoType GetCargoType(const AFGBuildableTrainPlatformCargo* Platform);

	// Counts the platform's belt or pipe inputs and outputs that are connected to something.
	static void CountConnections(const AFGBuildableTrainPlatformCargo* Platform, int32& OutInputs, int32& OutOutputs);

	// The platform's connected pipe connections that fill it (bInputs) or empty it.
	static TArray<class UFGPipeConnectionComponent*> GetPipeConnections(const AFGBuildableTrainPlatformCargo* Platform, bool bInputs);

	// Index of Connection among a splitter's or merger's outputs, as its sort rules count them, or INDEX_NONE.
	static int32 GetAttachmentOutputIndex(const class AFGBuildableConveyorAttachment* Attachment, const class UFGFactoryConnectionComponent* Connection);
};

// Station layouts and consists, read the way the game docks a train (Docs/Architecture.md, "Fit model").
namespace STLayout
{
	// Platforms of Station counted from the station along its output side, as the game walks them when docking.
	// Stops at the next station or after 64 pieces.
	TArray<AFGBuildableTrainPlatform*> GetPlatforms(const AFGBuildableRailroadStation* Station);

	// Returns the station Platform belongs to and its position in that station's chain, or nullptr.
	AFGBuildableRailroadStation* FindStation(const AFGBuildableTrainPlatform* Platform, int32* OutIndex = nullptr);

	// Vehicles of Train from its first vehicle to its last, as coupled.
	TArray<AFGRailroadVehicle*> GetVehicles(const AFGTrain* Train);

	// Vehicles behind Locomotive, nearest first: the vehicles that stand beside platforms 0, 1, 2... when it docks.
	TArray<AFGRailroadVehicle*> GetVehiclesBehind(const AFGLocomotive* Locomotive);

	// Locomotives that may be at a station when Train docks: its MU master and any locomotive at an end of the train
	// whose back faces the rest of the train.
	TArray<AFGLocomotive*> GetPossibleDockingLocomotives(const AFGTrain* Train);

	// Returns the cargo type a car can take: none if it is empty, else what it carries.
	EFreightCargoType GetCargoType(const AFGFreightWagon* Car);

	// Returns how much of Item the inventory holds in total when full.
	int32 GetCapacity(const class UFGInventoryComponent* Inventory, TSubclassOf<UFGItemDescriptor> Item);
	// How much of Item one slot of Inventory holds: the item's stack size, or the slot's own size where the inventory
	// sets one (fluid platforms and fluid cars hold far more than a fluid's stack).
	int32 GetSlotSize(const class UFGInventoryComponent* Inventory, TSubclassOf<UFGItemDescriptor> Item);

	// Freight car size from its type, not its inventory now (a car set up for a fluid has one big slot).
	int32 GetWagonSlots(const AFGFreightWagon* Wagon);
	int64 GetWagonFluidCapacity(const AFGFreightWagon* Wagon);
	// How much of Item a freight car holds when empty.
	double GetCarCapacity(const AFGFreightWagon* Wagon, TSubclassOf<UFGItemDescriptor> Item);
	// How much of Item fills one car slot: its stack size, or for a fluid the car's fluid room per slot.
	double GetCarSlotAmount(const AFGFreightWagon* Wagon, TSubclassOf<UFGItemDescriptor> Item);

	// Returns true if Item can be put on a platform or car of cargo type Type.
	bool ItemFitsCargoType(TSubclassOf<UFGItemDescriptor> Item, EFreightCargoType Type);

	// Returns the platform's cargo type (solid or fluid).
	EFreightCargoType GetPlatformCargoType(const AFGBuildableTrainPlatformCargo* Platform);

	// True if Platform can move freight or fluid: it has an inventory and belt or pipe connectors, attached or not.
	// Empty platform pieces built on the cargo platform class (Modular Stations) have neither.
	bool CanMoveCargo(const AFGBuildableTrainPlatformCargo* Platform);

	// Station identifier of Station, or nullptr.
	AFGTrainStationIdentifier* GetIdentifier(const AFGBuildableRailroadStation* Station);

	// "Name" of the station, or "?".
	FString Name(const AFGTrainStationIdentifier* Station);
	FString Name(const AFGBuildableRailroadStation* Station);
	FString Name(const AFGTrain* Train);

	// "car 3" for the third freight car counted from the front of Train, or the vehicle's object name.
	FString DescribeVehicle(const AFGTrain* Train, const AFGRailroadVehicle* Vehicle);
}
