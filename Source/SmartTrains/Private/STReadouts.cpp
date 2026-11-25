#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGInventoryComponent.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "Resources/FGItemDescriptor.h"
#include "SmartTrains.h"
#include "STLayout.h"
#include "STSubsystem.h"

int32 ASTSubsystem::CopyPlatformToStation(AFGBuildableTrainPlatformCargo* Platform, const FString& Cause, FString& OutResult)
{
	const FSTPlatformConfig* Source = FindPlatformConfig(Platform);
	const AFGBuildableRailroadStation* Station = STLayout::FindStation(Platform);
	if (!Source || !Station)
	{
		OutResult = TEXT("This platform has no settings to copy, or no station.");
		return -1;
	}
	const FSTPlatformConfig Copy = *Source;
	const EFreightCargoType Type = STLayout::GetPlatformCargoType(Platform);
	int32 Copied = 0;
	for (AFGBuildableTrainPlatform* Each : STLayout::GetPlatforms(Station))
	{
		AFGBuildableTrainPlatformCargo* Other = Cast<AFGBuildableTrainPlatformCargo>(Each);
		if (Other && Other != Platform && STLayout::GetPlatformCargoType(Other) == Type && STLayout::CanMoveCargo(Other))
		{
			SetPlatformConfig(Other, Copy.Role, Copy.Items, Cause);
			++Copied;
		}
	}
	OutResult = FString::Printf(TEXT("Copied to %d %s platforms of %s."), Copied, Type == EFreightCargoType::FCT_Liquid ? TEXT("fluid") : TEXT("freight"), *STLayout::Name(Station));
	return Copied;
}

FSTPlatformView ASTSubsystem::MakePlatformView(const AFGBuildableTrainPlatformCargo* Platform, int32 AppliedEdit) const
{
	FSTPlatformView View;
	View.Platform = const_cast<AFGBuildableTrainPlatformCargo*>(Platform);
	View.AppliedEdit = AppliedEdit;
	int32 Index = INDEX_NONE;
	const AFGBuildableRailroadStation* Station = STLayout::FindStation(Platform, &Index);
	View.bOnStation = Station != nullptr;
	View.bFluid = STLayout::GetPlatformCargoType(Platform) == EFreightCargoType::FCT_Liquid;
	View.Title = Station
		? FString::Printf(TEXT("%s platform %d of %s"), View.bFluid ? TEXT("Fluid") : TEXT("Freight"), Index + 1, *STLayout::Name(Station))
		: FString::Printf(TEXT("%s platform, not on a station's output side"), View.bFluid ? TEXT("Fluid") : TEXT("Freight"));
	View.bFcc = IsFccAvailable();
	const FSTPlatformConfig* Config = FindPlatformConfig(Platform);
	if (!Config)
	{
		return View;
	}
	View.Role = Config->Role;
	View.Items = Config->Items;
	View.EditedBy = Config->EditedBy;
	const UFGInventoryComponent* Inventory = Platform ? Platform->GetInventory() : nullptr;
	const AFGTrainStationIdentifier* Identifier = STLayout::GetIdentifier(Station);
	const FSTConsumer* Consumer = FindConsumer(Identifier);
	const FSTLoopSlot* Slot = Consumer ? Consumer->Slots.FindByPredicate([Index](const FSTLoopSlot& Each) { return Each.Index == Index; }) : nullptr;
	for (const FSTItemRule& Rule : Config->Items)
	{
		if (!Rule.Item)
		{
			View.ItemLines.Add(TEXT("takes any cargo a train switching patterns has left"));
			continue;
		}
		const int32 Stock = Inventory ? Inventory->GetNumItems(Rule.Item) : 0;
		const int32 Capacity = STLayout::GetCapacity(Inventory, Rule.Item);
		FString Rate;
		if (Config->Role == ESTRole::Demand && Slot)
		{
			const int32 ItemIndex = Slot->Items.IndexOfByKey(Rule.Item.Get());
			Rate = Slot->ItemRates.IsValidIndex(ItemIndex) && Slot->ItemRates[ItemIndex] > 0.0
				? FString::Printf(TEXT(", uses at most %.2f car loads/min"), Slot->ItemRates[ItemIndex]) : FString(TEXT(", rate not known"));
		}
		else if (Config->Role == ESTRole::Supply)
		{
			const FSTSupplier* Supplier = mSuppliers.FindByPredicate([&](const FSTSupplier& Each) { return Each.Station.Get() == Identifier && Each.Item.Get() == Rule.Item.Get(); });
			Rate = Supplier && Supplier->Rate >= 0.0 ? FString::Printf(TEXT(", makes at most %.2f car loads/min, loops take %.2f"), Supplier->Rate, Supplier->Taken)
				: FString(TEXT(", rate not known"));
		}
		View.ItemLines.Add(FString::Printf(TEXT("%s / %s%s"), *STNames::Amount(Rule.Item, Stock), *STNames::Amount(Rule.Item, Capacity), *Rate));
	}
	if (Config->Role == ESTRole::Off)
	{
		return View;
	}
	if (Config->Role == ESTRole::Demand)
	{
		if (!Consumer)
		{
			View.Lines.Add(TEXT("Its station is not a consumer yet: the loop plan is worked out every few seconds."));
		}
		else
		{
			View.Lines.Add(FString::Printf(TEXT("Its station's loop: %d trains (target %d)"), Consumer->Trains.Num(), Consumer->Target));
			if (!Consumer->Problem.IsEmpty())
			{
				View.Lines.Add(Consumer->Problem);
			}
		}
	}
	for (const FSTTrainState& State : mTrains)
	{
		if (!State.bManaged || !State.bHasJob || !State.Job.Legs.IsValidIndex(State.Job.Leg) || State.Job.Legs[State.Job.Leg].Station != Identifier)
		{
			continue;
		}
		for (const FSTCarWork& Work : State.Job.Legs[State.Job.Leg].Work)
		{
			if (Work.Platform == Platform)
			{
				View.Lines.Add(FString::Printf(TEXT("Next: %s, %s %s %s"), *STLayout::Name(State.Train), *STLayout::DescribeVehicle(State.Train, Work.Car),
					Work.bLoad ? TEXT("loads") : TEXT("unloads"), *STNames::Item(Work.Item)));
			}
		}
	}
	if (const TArray<FString>* History = Identifier ? mHistory.Find(const_cast<AFGTrainStationIdentifier*>(Identifier)) : nullptr)
	{
		for (const FString& Line : *History)
		{
			View.Lines.Add(TEXT("Recent: ") + Line);
		}
	}
	return View;
}

FSTTrainView ASTSubsystem::MakeTrainView(const AFGTrain* Train, int32 AppliedEdit) const
{
	FSTTrainView View;
	View.Train = const_cast<AFGTrain*>(Train);
	View.AppliedEdit = AppliedEdit;
	const FSTTrainState* State = FindTrainState(Train);
	View.bManaged = State && State->bManaged;
	View.bKeepVanilla = State && State->bKeepVanilla;
	View.bHeld = State && State->bHeld;
	View.bHasJob = State && State->bHasJob;
	if (View.bKeepVanilla)
	{
		View.CannotManage = TEXT("it is set to keep vanilla (/st train keepvanilla off)");
	}
	else if (STLayout::GetPossibleDockingLocomotives(Train).Num() == 0)
	{
		View.CannotManage = TEXT("it has no locomotive that can dock");
	}
	if (!View.bManaged)
	{
		View.Status = View.bKeepVanilla ? TEXT("Kept vanilla: SmartTrains and its migration leave this train alone.") : TEXT("Not managed: the timetable is yours.");
	}
	else
	{
		View.Status = FString::Printf(TEXT("%s%s"), mSettings.bObserveOnly ? TEXT("(observe only) ") : TEXT(""), *DescribeWork(*State));
		if (State->bHasJob)
		{
			View.Lines.Add(FString::Printf(TEXT("Pattern: %s"), *State->DescribePattern()));
			for (int32 Leg = 0; Leg < State->Job.Legs.Num(); ++Leg)
			{
				const FSTJobLeg& Each = State->Job.Legs[Leg];
				View.Lines.Add(FString::Printf(TEXT("%s %d. %s: %s"), Leg == State->Job.Leg ? TEXT(">") : TEXT(" "), Leg + 1, *STLayout::Name(Each.Station.Get()),
					Each.HasWork() ? *DescribeLeg(Train, Each, false) : TEXT("drive there")));
			}
		}
		else if (const FString* Observed = mObserved.Find(const_cast<AFGTrain*>(Train)))
		{
			View.Lines.Add(TEXT("Would do: ") + *Observed);
		}
	}
	for (const TPair<UClass*, int32>& Pair : GetCargo(Train))
	{
		View.Lines.Add(FString::Printf(TEXT("Carries %s %s"), *STNames::Amount(Pair.Key, Pair.Value), *STNames::Item(Pair.Key)));
	}
	return View;
}

FSTStationView ASTSubsystem::MakeStationView(const AFGTrainStationIdentifier* Station, int32 AppliedEdit) const
{
	FSTStationView View;
	View.Station = const_cast<AFGTrainStationIdentifier*>(Station);
	View.AppliedEdit = AppliedEdit;
	View.bBetaTools = mSettings.bBetaTools;
	if (const TArray<FString>* Setup = mSetupLines.Find(const_cast<AFGTrainStationIdentifier*>(Station)))
	{
		View.SetupLines = *Setup;
	}
	const double* PreviewAt = mSetupPreviewAt.Find(const_cast<AFGTrainStationIdentifier*>(Station));
	View.SetupReady = PreviewAt && Now() - *PreviewAt < 120.0 ? mSetupPreviewMode.FindRef(const_cast<AFGTrainStationIdentifier*>(Station)) : 0;
	TArray<FString> Lines = DescribeStation(Station);
	if (Lines.Num() > 0)
	{
		// The first line repeats the station name, which the menu shows itself.
		Lines.RemoveAt(0);
	}
	for (FString& Line : Lines)
	{
		View.Lines.Add(Line.TrimStart());
	}
	for (const FString& Line : DescribeStatus())
	{
		View.NetworkLines.Add(Line.TrimStart());
	}
	return View;
}
