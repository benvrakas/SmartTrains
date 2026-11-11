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

FString ASTSubsystem::DescribeItemRule(const AFGBuildableTrainPlatformCargo* Platform, const FSTPlatformConfig& Config, const FSTItemRule& Rule) const
{
	if (!Rule.Item)
	{
		return Config.Role == ESTRole::Sink ? FString(TEXT("takes any cargo no consumer wants")) : FString::Printf(TEXT("takes any surplus, %s priority"), STNames::Priority(Rule.Priority));
	}
	const UFGInventoryComponent* Inventory = Platform ? Platform->GetInventory() : nullptr;
	const bool bManual = Rule.ManualRatePerMinute > 0.f;
	bool bKnown = false;
	const double Rate = bManual ? Rule.ManualRatePerMinute / 60.0 : GetMeasuredRate(Platform, Rule.Item, &bKnown);
	const int32 Stock = Inventory ? Inventory->GetNumItems(Rule.Item) : 0;
	const int32 Capacity = STLayout::GetCapacity(Inventory, Rule.Item);
	const FString Goal = Config.Role == ESTRole::Supply
		? FString::Printf(TEXT("least per visit %.0f%% of a load"), STNames::SendAtPercent(Rule.SendAtPercent))
		: FString::Printf(TEXT("target %.0f%%"), Rule.TargetPercent);
	// Priority counts only where cargo is delivered.
	const FString Priority = Config.Role == ESTRole::Supply ? FString() : FString::Printf(TEXT(", %s priority"), STNames::Priority(Rule.Priority));
	return FString::Printf(TEXT("%s / %s, %s%s/min%s, %s%s"), *STNames::Amount(Rule.Item, Stock), *STNames::Amount(Rule.Item, Capacity),
		bManual ? TEXT("manual ") : TEXT(""), *STNames::Amount(Rule.Item, Rate * 60.0), bKnown || bManual ? TEXT("") : TEXT(" (still measuring)"), *Goal, *Priority);
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
	for (const FSTItemRule& Rule : Config->Items)
	{
		View.ItemLines.Add(DescribeItemRule(Platform, *Config, Rule));
	}
	if (Config->Role == ESTRole::Off)
	{
		return View;
	}
	const AFGTrainStationIdentifier* Identifier = STLayout::GetIdentifier(Station);
	for (const FSTTrainState& State : mTrains)
	{
		if (!State.bManaged || !State.bHasJob)
		{
			continue;
		}
		for (int32 Leg = State.Job.Leg; Leg < State.Job.Legs.Num(); ++Leg)
		{
			const FSTJobLeg& Each = State.Job.Legs[Leg];
			if (!Identifier || Each.Station != Identifier)
			{
				continue;
			}
			for (const FSTCarWork& Work : Each.Work)
			{
				if (Config->FindRule(Work.Item) && Work.bLoad == (Config->Role == ESTRole::Supply))
				{
					View.Lines.Add(FString::Printf(TEXT("Inbound: %s, %s %s %s %s, ETA %.0f s"), *STLayout::Name(State.Train), *STLayout::DescribeVehicle(State.Train, Work.Car),
						Work.bLoad ? TEXT("loads") : TEXT("unloads"), *STNames::Amount(Work.Item, Work.PlannedAmount), *STNames::Item(Work.Item),
						FMath::Max(0.0, Each.PlannedArrival - Now())));
				}
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
	View.Lines.Append(ExplainPlatform(Platform));
	return View;
}

TArray<FString> ASTSubsystem::ExplainPlatform(const AFGBuildableTrainPlatformCargo* Platform) const
{
	TArray<FString> Lines;
	const FSTPlatformConfig* Config = FindPlatformConfig(Platform);
	if (!Config || Config->Role == ESTRole::Off)
	{
		return Lines;
	}
	Lines.Append(DescribePlatformGroups(Platform));
	if (mSettings.bObserveOnly)
	{
		Lines.Add(TEXT("Observe only is on (Mods menu): rates are measured, but no jobs are given."));
		return Lines;
	}
	const AFGBuildableRailroadStation* Station = STLayout::FindStation(Platform);
	const AFGTrainStationIdentifier* Identifier = STLayout::GetIdentifier(Station);
	if (!Identifier)
	{
		Lines.Add(TEXT("Not on a station's output side: trains can't dock here."));
		return Lines;
	}
	if (Config->Items.Num() == 0)
	{
		Lines.Add(TEXT("No items yet: add the item this platform supplies or demands."));
		return Lines;
	}
	TArray<FSTNode> Nodes;
	BuildNodes(Nodes);
	const int32 Graph = Station->GetTrackGraphID();
	const bool bSupply = Config->Role == ESTRole::Supply;
	const ESTRole Other = bSupply ? ESTRole::Demand : ESTRole::Supply;
	for (const FSTItemRule& Rule : Config->Items)
	{
		if (!Rule.Item)
		{
			continue;
		}
		const FString Name = STNames::Item(Rule.Item);
		const bool bInbound = mTrains.ContainsByPredicate([&](const FSTTrainState& State)
		{
			if (!State.bManaged || !State.bHasJob)
			{
				return false;
			}
			for (int32 Leg = State.Job.Leg; Leg < State.Job.Legs.Num(); ++Leg)
			{
				const FSTJobLeg& Each = State.Job.Legs[Leg];
				if (Each.Station == Identifier && Each.Work.ContainsByPredicate([&](const FSTCarWork& Work) { return Work.Item == Rule.Item && Work.bLoad == bSupply; }))
				{
					return true;
				}
			}
			return false;
		});
		const int32 Own = FindNode(Nodes, Identifier, Rule.Item, Config->Role);
		if (bInbound || Own == INDEX_NONE)
		{
			continue;
		}
		const FSTNode& Node = Nodes[Own];

		TArray<int32> Partners;
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			const FSTNode& Each = Nodes[Index];
			if (Each.Role == Other && Each.Station.Get() != Identifier && Each.TrackGraph == Graph && (Each.Item == Rule.Item || (bSupply && Each.bAnyItem)))
			{
				Partners.Add(Index);
			}
		}
		if (Partners.Num() == 0)
		{
			Lines.Add(FString::Printf(TEXT("%s: no %s platform for it on this rail network."), *Name, bSupply ? TEXT("demand") : TEXT("supply")));
			continue;
		}

		int32 Serving = 0;
		int32 Fitting = 0;
		int32 FreeFitting = 0;
		int32 BestCapacity = 0;
		FString Problem;
		for (const FSTTrainState& State : mTrains)
		{
			if (!State.bManaged || !IsValid(State.Train) || !CanServe(State, Identifier))
			{
				continue;
			}
			++Serving;
			bool bFits = false;
			for (const int32 Partner : Partners)
			{
				const AFGTrainStationIdentifier* PartnerStation = Nodes[Partner].Station.Get();
				if (!CanServe(State, PartnerStation))
				{
					continue;
				}
				const FSTFit Fit = bSupply ? ComputeFit(State.Train, Identifier, PartnerStation, Rule.Item, true) : ComputeFit(State.Train, PartnerStation, Identifier, Rule.Item, true);
				if (Fit.Capacity > 0)
				{
					bFits = true;
					BestCapacity = FMath::Max(BestCapacity, Fit.Capacity);
				}
				else if (Problem.IsEmpty())
				{
					Problem = Fit.Problem;
				}
			}
			Fitting += bFits ? 1 : 0;
			// Free: no job.
			FreeFitting += bFits && !State.bHasJob ? 1 : 0;
		}
		if (Serving == 0)
		{
			Lines.Add(FString::Printf(TEXT("%s: no managed train can reach this station."), *Name));
			continue;
		}
		if (Fitting == 0)
		{
			Lines.Add(FString::Printf(TEXT("%s: no managed train fits (%s)."), *Name, *Problem));
			continue;
		}

		if (bSupply)
		{
			const double Batch = BestCapacity * STNames::SendAtPercent(Rule.SendAtPercent) / 100.0;
			if (Node.Stock - Node.Reserved < Batch * 0.5)
			{
				Lines.Add(FString::Printf(TEXT("%s: waiting for a batch, %s of %s at this station."), *Name, *STNames::Amount(Rule.Item, Node.Stock),
					*STNames::Amount(Rule.Item, Batch)));
				continue;
			}
			const bool bAnyNeeds = Partners.ContainsByPredicate([&](int32 Partner)
			{
				const FSTNode& Consumer = Nodes[Partner];
				return !Consumer.bAnyItem && Consumer.Stock + Consumer.Reserved < Consumer.Capacity * Consumer.TargetPercent / 100.0;
			});
			if (!bAnyNeeds && Node.Stock < 0.9 * Node.Capacity)
			{
				Lines.Add(FString::Printf(TEXT("%s: every consumer is stocked to its target."), *Name));
				continue;
			}
		}
		else
		{
			const double Target = Node.Capacity * Node.TargetPercent / 100.0;
			if (Node.Stock + Node.Reserved >= Target)
			{
				Lines.Add(FString::Printf(TEXT("%s: stocked. The station's %d platforms for it hold %s%s, target %s."), *Name, Node.Platforms.Num(),
					*STNames::Amount(Rule.Item, Node.Stock), Node.Reserved > 0 ? *FString::Printf(TEXT(" with %s on the way"), *STNames::Amount(Rule.Item, Node.Reserved)) : TEXT(""),
					*STNames::Amount(Rule.Item, Target)));
				continue;
			}
			const bool bReady = Partners.ContainsByPredicate([&](int32 Partner) { return Nodes[Partner].Stock > Nodes[Partner].Reserved; });
			if (!bReady)
			{
				Lines.Add(FString::Printf(TEXT("%s: no supplier has any ready."), *Name));
				continue;
			}
		}
		if (FreeFitting == 0)
		{
			Lines.Add(FString::Printf(TEXT("%s: all %d managed trains that fit are busy."), *Name, Fitting));
			continue;
		}
		Lines.Add(FString::Printf(TEXT("%s: %d free trains fit; a job should follow at the next dispatch round."), *Name, FreeFitting));
	}
	return Lines;
}

FSTTrainView ASTSubsystem::MakeTrainView(const AFGTrain* Train, int32 AppliedEdit) const
{
	FSTTrainView View;
	View.Train = const_cast<AFGTrain*>(Train);
	View.AppliedEdit = AppliedEdit;
	const FSTTrainState* State = mTrains.FindByPredicate([Train](const FSTTrainState& Each) { return Each.Train == Train; });
	View.bManaged = State && State->bManaged;
	View.bKeepVanilla = State && State->bKeepVanilla;
	View.bHeld = State && State->bHeld;
	View.bHasJob = State && State->bHasJob && !State->Job.bPark;
	if (View.bKeepVanilla)
	{
		View.CannotManage = TEXT("it is set to keep vanilla (/st train keepvanilla off)");
	}
	else if (STLayout::GetPossibleDockingLocomotives(Train).Num() == 0)
	{
		View.CannotManage = TEXT("it has no locomotive that can dock");
	}
	if (State && State->bManaged)
	{
		View.Lines.Add(DescribeWork(*State));
	}
	if (!View.bManaged)
	{
		View.Status = View.bKeepVanilla ? TEXT("Kept vanilla: SmartTrains and its migration leave this train alone.") : TEXT("Not managed: the timetable is yours.");
	}
	else if (State->bHeld)
	{
		View.Status = State->bHasJob ? FString::Printf(TEXT("Held (Resume to go on with job %d: %s)."), State->Job.Id, *State->Job.Summary)
			: FString(TEXT("Held: no new trips until Resume."));
	}
	else if (State->bHasJob)
	{
		View.Status = FString::Printf(TEXT("%sJob %d: %s"), mSettings.bObserveOnly ? TEXT("(observe only) ") : TEXT(""), State->Job.Id, *State->Job.Summary);
		for (int32 Leg = 0; Leg < State->Job.Legs.Num(); ++Leg)
		{
			const FSTJobLeg& Each = State->Job.Legs[Leg];
			const FString What = Each.HasWork() ? DescribeLeg(Train, Each, Leg < State->Job.Leg) : FString(TEXT("drive there"));
			View.Lines.Add(FString::Printf(TEXT("%s %d. %s: %s%s"), Leg == State->Job.Leg ? TEXT(">") : (Leg < State->Job.Leg ? TEXT("v") : TEXT(" ")), Leg + 1,
				*STLayout::Name(Each.Station.Get()), *What,
				Leg >= State->Job.Leg ? *FString::Printf(TEXT(", ETA %.0f s"), FMath::Max(0.0, Each.PlannedArrival - Now())) : TEXT("")));
		}
	}
	else
	{
		View.Status = FString::Printf(TEXT("%s%s: waiting for a job."), mSettings.bObserveOnly ? TEXT("(observe only) ") : TEXT(""),
			State->Phase == ESTTrainPhase::Parked ? TEXT("Parked") : TEXT("Idle"));
		if (const FString* Observed = mObserved.Find(const_cast<AFGTrain*>(Train)))
		{
			View.Lines.Add(TEXT("Would do: ") + *Observed);
		}
		if (const FString* Reason = mIdleReasons.Find(const_cast<AFGTrain*>(Train)))
		{
			View.Lines.Add(TEXT("Why no trip: ") + *Reason);
		}
	}
	for (const TPair<UClass*, int32>& Pair : GetCargo(Train))
	{
		View.Lines.Add(FString::Printf(TEXT("Carries %s %s"), *STNames::Amount(Pair.Key, Pair.Value), *STNames::Item(Pair.Key)));
	}
	if (State && State->UnwantedCargoSince > 0.0)
	{
		View.Lines.Add(FString::Printf(TEXT("No consumer below its target wants this cargo (for %.0f min); after %.0f min it may go to any consumer with room."),
			(Now() - State->UnwantedCargoSince) / 60.0, mSettings.LeftoverMinutes));
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
	int32 Waiting = 0;
	for (const FSTTrainState& State : mTrains)
	{
		Waiting += State.bManaged && !State.bHasJob && State.LastStation == Station ? 1 : 0;
	}
	if (Waiting > 0)
	{
		View.Lines.Add(FString::Printf(TEXT("%d managed trains wait here for a job."), Waiting));
	}
	for (const FString& Line : DescribeStatus())
	{
		View.NetworkLines.Add(Line.TrimStart());
	}
	return View;
}
