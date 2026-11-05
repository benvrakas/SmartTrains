#include "STTrace.h"
#include "Buildables/FGBuildableConveyorBase.h"
#include "Buildables/FGBuildableConveyorLift.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildablePassthrough.h"
#include "Buildables/FGBuildableResourceExtractor.h"
#include "Buildables/FGBuildableResourceExtractorBase.h"
#include "Buildables/FGBuildableSplitterSmart.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGFactoryConnectionComponent.h"
#include "FGFluidIntegrantInterface.h"
#include "FGPipeConnectionComponent.h"
#include "FGRecipe.h"
#include "STLayout.h"
#include "Resources/FGAnyUndefinedDescriptor.h"
#include "Resources/FGExtractableResourceInterface.h"
#include "Resources/FGItemDescriptor.h"
#include "Resources/FGNoneDescriptor.h"
#include "Resources/FGOverflowDescriptor.h"
#include "Resources/FGWildCardDescriptor.h"
#include "SmartTrains.h"
#include "STTypes.h"
#include "UObject/UnrealType.h"

namespace
{
	// Which items may pass along a path: all, only Allowed (when bRestricted), and never Excluded.
	struct FItemFilter
	{
		bool bRestricted = false;
		TArray<UClass*> Allowed;
		TArray<UClass*> Excluded;

		bool Passes(UClass* Item) const
		{
			return (!bRestricted || Allowed.Contains(Item)) && !Excluded.Contains(Item);
		}

		bool IsEmpty() const
		{
			return bRestricted && Allowed.Num() == 0;
		}

		void Restrict(const TArray<UClass*>& Items)
		{
			if (!bRestricted)
			{
				bRestricted = true;
				Allowed = Items;
				return;
			}
			Allowed.RemoveAll([&Items](UClass* Item) { return !Items.Contains(Item); });
		}

		void Exclude(const TArray<UClass*>& Items)
		{
			for (UClass* Item : Items)
			{
				Excluded.AddUnique(Item);
			}
		}
	};

	// One walker position. Belt: our side of a belt connection (the walk continues at its partner); Arrive: a
	// connection already reached directly (the far side of a floor hole); Pipe: our side of a pipe connection.
	struct FStep
	{
		UFGFactoryConnectionComponent* Belt = nullptr;
		UFGFactoryConnectionComponent* Arrive = nullptr;
		UFGPipeConnectionComponent* Pipe = nullptr;
		bool bShared = false;
		FItemFilter Filter;
		TArray<FString> Labels;
	};

	FString Label(const AActor* Actor)
	{
		FString Name = Actor ? Actor->GetClass()->GetName() : FString(TEXT("?"));
		Name.RemoveFromStart(TEXT("Build_"));
		Name.RemoveFromEnd(TEXT("_C"));
		return Name;
	}

	// "Belt x3 > Splitter > Constructor": consecutive repeats are folded.
	FString PathText(const TArray<FString>& Labels)
	{
		FString Text;
		for (int32 Index = 0; Index < Labels.Num();)
		{
			int32 End = Index;
			while (End + 1 < Labels.Num() && Labels[End + 1] == Labels[Index])
			{
				++End;
			}
			Text += (Text.IsEmpty() ? TEXT("") : TEXT(" > ")) + Labels[Index];
			if (End > Index)
			{
				Text += FString::Printf(TEXT(" x%d"), End - Index + 1);
			}
			Index = End + 1;
		}
		return Text;
	}

	bool IsFluid(TSubclassOf<UFGItemDescriptor> Item)
	{
		const EResourceForm Form = UFGItemDescriptor::GetForm(Item);
		return Form == EResourceForm::RF_LIQUID || Form == EResourceForm::RF_GAS;
	}

	void AddUnique(TArray<FString>& Notes, const FString& Note)
	{
		Notes.AddUnique(Note);
	}

	//~ Modular Load Balancers (optional mod), read by reflection: a group of modules moves items between its modules
	//~ without belts, so the whole group is one building for the trace.

	bool IsBalancer(const AActor* Actor)
	{
		for (const UClass* Class = Actor ? Actor->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
		{
			if (Class->GetFName() == TEXT("LBBuild_ModularLoadBalancer"))
			{
				return true;
			}
		}
		return false;
	}

	UObject* GetObjectField(const UObject* Object, FName Name)
	{
		const FObjectPropertyBase* Property = Object ? CastField<FObjectPropertyBase>(Object->GetClass()->FindPropertyByName(Name)) : nullptr;
		return Property ? Property->GetObjectPropertyValue_InContainer(Object) : nullptr;
	}

	TArray<UObject*> GetObjectArrayField(const UObject* Object, FName Name)
	{
		TArray<UObject*> Out;
		const FArrayProperty* Property = Object ? CastField<FArrayProperty>(Object->GetClass()->FindPropertyByName(Name)) : nullptr;
		const FObjectPropertyBase* Inner = Property ? CastField<FObjectPropertyBase>(Property->Inner) : nullptr;
		if (!Inner)
		{
			return Out;
		}
		FScriptArrayHelper Helper(Property, Property->ContainerPtrToValuePtr<void>(Object));
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			if (UObject* Each = Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index)))
			{
				Out.Add(Each);
			}
		}
		return Out;
	}

	// The modules of Module's balancer group (Module itself included).
	TArray<AFGBuildable*> GetBalancerGroup(AFGBuildable* Module)
	{
		TArray<AFGBuildable*> Group;
		UObject* Leader = GetObjectField(Module, TEXT("GroupLeader"));
		Leader = Leader ? Leader : Module;
		for (UObject* Each : GetObjectArrayField(Leader, TEXT("mGroupModules")))
		{
			if (AFGBuildable* Buildable = Cast<AFGBuildable>(Each))
			{
				Group.AddUnique(Buildable);
			}
		}
		Group.AddUnique(Cast<AFGBuildable>(Leader));
		Group.AddUnique(Module);
		Group.Remove(nullptr);
		return Group;
	}

	// Items a balancer filter module is set to.
	TArray<UClass*> GetBalancerFilter(const AFGBuildable* Module)
	{
		TArray<UClass*> Items;
		for (UObject* Each : GetObjectArrayField(Module, TEXT("mFilteredItems")))
		{
			if (UClass* Item = Cast<UClass>(Each))
			{
				Items.AddUnique(Item);
			}
		}
		if (UClass* Single = Cast<UClass>(GetObjectField(Module, TEXT("mFilteredItem"))))
		{
			Items.AddUnique(Single);
		}
		return Items;
	}

	class FTracer
	{
	public:
		FTracer(bool bInLoadMode, int32 InLimit, STTrace::FResult& InResult) : bUpstream(bInLoadMode), Limit(InLimit), Result(InResult) {}

		void Run(const AFGBuildableTrainPlatformCargo* Platform)
		{
			TInlineComponentArray<UFGFactoryConnectionComponent*> Belts;
			Platform->GetComponents(Belts);
			const EFactoryConnectionDirection Wanted = bUpstream ? EFactoryConnectionDirection::FCD_INPUT : EFactoryConnectionDirection::FCD_OUTPUT;
			for (UFGFactoryConnectionComponent* Belt : Belts)
			{
				if (Belt && Belt->IsConnected() && Belt->GetDirection() == Wanted)
				{
					FStep Step;
					Step.Belt = Belt;
					Stack.Add(MoveTemp(Step));
				}
			}
			for (UFGPipeConnectionComponent* Pipe : FSTPlatformAccess::GetPipeConnections(Platform, bUpstream))
			{
				if (!Result.PipeFluidHint)
				{
					Result.PipeFluidHint = Pipe->GetFluidDescriptor();
				}
				FStep Step;
				Step.Pipe = Pipe;
				Stack.Add(MoveTemp(Step));
			}
			Visited.Add(Platform);
			while (Stack.Num() > 0)
			{
				FStep Step = Stack.Pop(EAllowShrinking::No);
				if (Visited.Num() >= Limit)
				{
					Result.bIncomplete = true;
					AddUnique(Result.Notes, FString::Printf(TEXT("stopped at the trace limit (%d buildings and belts)"), Limit));
					break;
				}
				if (Step.Belt || Step.Arrive)
				{
					StepBelt(Step);
				}
				else if (Step.Pipe)
				{
					StepPipe(Step);
				}
			}
			Result.Nodes = Visited.Num();
		}

	private:
		bool bUpstream;
		int32 Limit;
		STTrace::FResult& Result;
		TArray<FStep> Stack;
		TSet<const AActor*> Visited;

		void AddFlow(TSubclassOf<UFGItemDescriptor> Item, float PerMinute, const FStep& Step, const AActor* Machine)
		{
			if (!Item)
			{
				return;
			}
			if (!Step.Filter.Passes(Item.Get()))
			{
				++Result.Filtered;
				AddUnique(Result.Notes, FString::Printf(TEXT("%s from %s is filtered away on the way"), *STNames::Item(Item), *Label(Machine)));
				return;
			}
			for (STTrace::FFlow& Flow : Result.Flows)
			{
				if (Flow.Item == Item)
				{
					Flow.PerMinute += PerMinute;
					Flow.bShared &= Step.bShared;
					return;
				}
			}
			STTrace::FFlow Flow;
			Flow.Item = Item;
			Flow.PerMinute = PerMinute;
			Flow.bShared = Step.bShared;
			TArray<FString> Labels = Step.Labels;
			Labels.Add(Label(Machine));
			Flow.Path = PathText(Labels);
			Result.Flows.Add(MoveTemp(Flow));
		}

		// A machine's recipe: what it makes (upstream) or what it eats (downstream). Fluids and solids are kept apart by bWantFluid.
		bool ReadMachine(AActor* Owner, const FStep& Step, bool bWantFluid)
		{
			if (const AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Owner))
			{
				++Result.Machines;
				const TSubclassOf<UFGRecipe> Recipe = Manufacturer->GetCurrentRecipe();
				if (!Recipe)
				{
					AddUnique(Result.Notes, FString::Printf(TEXT("%s has no recipe set"), *Label(Owner)));
					return true;
				}
				const float Duration = UFGRecipe::GetManufacturingDuration(Recipe);
				float Clock = Manufacturer->GetCurrentPotential();
				Clock = Clock > 0.0f ? Clock : 1.0f;
				const TArray<FItemAmount> Amounts = bUpstream ? UFGRecipe::GetProducts(Recipe) : UFGRecipe::GetIngredients(Owner, Recipe);
				for (const FItemAmount& Amount : Amounts)
				{
					if (!Amount.ItemClass || IsFluid(Amount.ItemClass) != bWantFluid)
					{
						continue;
					}
					const float Units = IsFluid(Amount.ItemClass) ? Amount.Amount / 1000.0f : static_cast<float>(Amount.Amount);
					AddFlow(Amount.ItemClass, Duration > 0.0f ? Units * 60.0f / Duration * Clock : 0.0f, Step, Owner);
				}
				return true;
			}
			if (const AFGBuildableResourceExtractorBase* Extractor = Cast<AFGBuildableResourceExtractorBase>(Owner))
			{
				++Result.Machines;
				if (!bUpstream)
				{
					AddUnique(Result.Notes, FString::Printf(TEXT("%s only produces"), *Label(Owner)));
					return true;
				}
				IFGExtractableResourceInterface* Resource = Extractor->GetExtractableResource().GetInterface();
				if (!Resource)
				{
					AddUnique(Result.Notes, FString::Printf(TEXT("%s has no resource"), *Label(Owner)));
					return true;
				}
				TSubclassOf<UFGItemDescriptor> Item = Resource->GetResourceClass();
				float PerMinute = 0.0f;
				if (const AFGBuildableResourceExtractor* Simple = Cast<AFGBuildableResourceExtractor>(Owner))
				{
					const float Cycle = Simple->GetDefaultExtractCycleTime();
					PerMinute = Cycle > 0.0f ? Simple->GetNumExtractedItemsPerCycleConverted() * 60.0f / Cycle : 0.0f;
				}
				if (Item && IsFluid(Item) == bWantFluid)
				{
					AddFlow(Item, PerMinute, Step, Owner);
				}
				return true;
			}
			return false;
		}

		// Items a smart or programmable splitter sends to OutputIndex: whatever its rules let through. Overflow is
		// unknown in advance, so it lets everything through (the path is then marked shared).
		static void SplitterOutputFilter(const AFGBuildableSplitterSmart* Splitter, int32 OutputIndex, FItemFilter& InOutFilter, bool& bOutShared)
		{
			TArray<UClass*> Explicit;
			TArray<UClass*> ElsewhereExplicit;
			bool bAny = false;
			bool bUndefined = false;
			bool bOverflow = false;
			for (const FSplitterSortRule& Rule : Splitter->GetSortRules())
			{
				UClass* Item = Rule.ItemClass.Get();
				if (!Item)
				{
					continue;
				}
				const bool bSpecial = Item->IsChildOf(UFGWildCardDescriptor::StaticClass()) || Item->IsChildOf(UFGAnyUndefinedDescriptor::StaticClass())
					|| Item->IsChildOf(UFGOverflowDescriptor::StaticClass()) || Item->IsChildOf(UFGNoneDescriptor::StaticClass());
				if (Rule.OutputIndex != OutputIndex)
				{
					if (!bSpecial)
					{
						ElsewhereExplicit.AddUnique(Item);
					}
					continue;
				}
				if (Item->IsChildOf(UFGWildCardDescriptor::StaticClass()))
				{
					bAny = true;
				}
				else if (Item->IsChildOf(UFGAnyUndefinedDescriptor::StaticClass()))
				{
					bUndefined = true;
				}
				else if (Item->IsChildOf(UFGOverflowDescriptor::StaticClass()))
				{
					bOverflow = true;
				}
				else if (!bSpecial)
				{
					Explicit.AddUnique(Item);
				}
			}
			if (bAny || bOverflow)
			{
				bOutShared |= bOverflow;
				return;
			}
			if (bUndefined)
			{
				InOutFilter.Exclude(ElsewhereExplicit);
				return;
			}
			InOutFilter.Restrict(Explicit);
		}

		void Push(const FStep& From, UFGFactoryConnectionComponent* Belt, UFGFactoryConnectionComponent* Arrive, const FString& StepLabel, const FItemFilter& Filter, bool bShared)
		{
			if (Filter.IsEmpty())
			{
				return;
			}
			FStep Next;
			Next.Belt = Belt;
			Next.Arrive = Arrive;
			Next.bShared = From.bShared || bShared;
			Next.Filter = Filter;
			Next.Labels = From.Labels;
			Next.Labels.Add(StepLabel);
			Stack.Add(MoveTemp(Next));
		}

		void StepBelt(const FStep& Step)
		{
			UFGFactoryConnectionComponent* Neighbour = Step.Arrive ? Step.Arrive : Step.Belt->GetConnection();
			AFGBuildable* Owner = Neighbour ? Neighbour->GetOuterBuildable() : nullptr;
			if (!Owner)
			{
				AddUnique(Result.Notes, TEXT("a connection leads nowhere"));
				return;
			}
			// The neighbour must face us: upstream means its output meets our input.
			const EFactoryConnectionDirection Facing = bUpstream ? EFactoryConnectionDirection::FCD_OUTPUT : EFactoryConnectionDirection::FCD_INPUT;
			if (Neighbour->GetDirection() != Facing && Neighbour->GetDirection() != EFactoryConnectionDirection::FCD_ANY)
			{
				AddUnique(Result.Notes, FString::Printf(TEXT("%s faces the wrong way"), *Label(Owner)));
				return;
			}
			if (Visited.Contains(Owner))
			{
				return;
			}
			if (!IsBalancer(Owner))
			{
				Visited.Add(Owner);
			}
			if (ReadMachine(Owner, Step, false))
			{
				return;
			}
			const EFactoryConnectionDirection ExitDirection = bUpstream ? EFactoryConnectionDirection::FCD_INPUT : EFactoryConnectionDirection::FCD_OUTPUT;
			const bool bBelt = Owner->IsA<AFGBuildableConveyorBase>();
			const FString StepLabel = bBelt ? FString(TEXT("Belt")) : Label(Owner);

			// Modular Load Balancers: the whole group, through any of its modules.
			if (IsBalancer(Owner))
			{
				const TArray<AFGBuildable*> Group = GetBalancerGroup(Owner);
				FItemFilter Filter = Step.Filter;
				// Upstream through a filter module's output: only its items; through another output: none of the group's filtered items.
				if (bUpstream)
				{
					const TArray<UClass*> Own = GetBalancerFilter(Owner);
					if (Own.Num() > 0)
					{
						Filter.Restrict(Own);
					}
					else
					{
						for (const AFGBuildable* Module : Group)
						{
							Filter.Exclude(GetBalancerFilter(Module));
						}
					}
				}
				int32 Exits = 0;
				for (AFGBuildable* Module : Group)
				{
					Visited.Add(Module);
					TInlineComponentArray<UFGFactoryConnectionComponent*> Connections;
					Module->GetComponents(Connections);
					const TArray<UClass*> ModuleFilter = bUpstream ? TArray<UClass*>() : GetBalancerFilter(Module);
					for (UFGFactoryConnectionComponent* Connection : Connections)
					{
						if (!Connection || Connection == Neighbour || !Connection->IsConnected() || Connection->GetDirection() != ExitDirection)
						{
							continue;
						}
						FItemFilter ExitFilter = Filter;
						if (ModuleFilter.Num() > 0)
						{
							ExitFilter.Restrict(ModuleFilter);
						}
						Push(Step, Connection, nullptr, TEXT("LoadBalancer"), ExitFilter, false);
						++Exits;
					}
				}
				Result.bBranched |= Exits > 1;
				if (Exits == 0)
				{
					AddUnique(Result.Notes, TEXT("a load balancer group has no other connected side"));
				}
				return;
			}

			// Belts and lifts: on through their other end, whatever direction its connection reports.
			if (AFGBuildableConveyorBase* Conveyor = Cast<AFGBuildableConveyorBase>(Owner))
			{
				UFGFactoryConnectionComponent* Other = Conveyor->GetConnection0() == Neighbour ? Conveyor->GetConnection1() : Conveyor->GetConnection0();
				if (Other && Other->IsConnected())
				{
					Push(Step, Other, nullptr, StepLabel, Step.Filter, false);
					return;
				}
				// A lift ending in a floor hole continues on the other side of the hole.
				if (AFGBuildableConveyorLift* Lift = Cast<AFGBuildableConveyorLift>(Conveyor))
				{
					bool bThrough = false;
					for (AFGBuildablePassthrough* Hole : Lift->GetSnappedPassthroughs())
					{
						if (!Hole)
						{
							continue;
						}
						for (UFGFactoryConnectionComponent* Far : {Hole->GetTopSnappedConnection<UFGFactoryConnectionComponent>(), Hole->GetBottomSnappedConnection<UFGFactoryConnectionComponent>()})
						{
							if (Far && Far->GetOuterBuildable() != Lift)
							{
								Push(Step, nullptr, Far, TEXT("Lift"), Step.Filter, false);
								bThrough = true;
							}
						}
					}
					if (bThrough)
					{
						return;
					}
				}
				AddUnique(Result.Notes, FString::Printf(TEXT("a %s ends unconnected"), bBelt && Owner->IsA<AFGBuildableConveyorLift>() ? TEXT("lift") : TEXT("belt")));
				return;
			}
			TInlineComponentArray<UFGFactoryConnectionComponent*> Connections;
			Owner->GetComponents(Connections);
			TArray<UFGFactoryConnectionComponent*> Exits;
			for (UFGFactoryConnectionComponent* Connection : Connections)
			{
				const EFactoryConnectionDirection Direction = Connection ? Connection->GetDirection() : EFactoryConnectionDirection::FCD_ANY;
				if (Connection && Connection != Neighbour && Connection->IsConnected() && (Direction == ExitDirection || Direction == EFactoryConnectionDirection::FCD_ANY))
				{
					Exits.Add(Connection);
				}
			}
			if (Exits.Num() == 0)
			{
				AddUnique(Result.Notes, FString::Printf(TEXT("%s is not a recognised source or sink"), *Label(Owner)));
				return;
			}
			Result.bBranched |= Exits.Num() > 1 && !bBelt;
			const AFGBuildableSplitterSmart* Smart = Cast<AFGBuildableSplitterSmart>(Owner);
			for (UFGFactoryConnectionComponent* Exit : Exits)
			{
				FItemFilter Filter = Step.Filter;
				bool bShared = false;
				if (Smart)
				{
					// Upstream we came in through one of its outputs; downstream we leave through one.
					const int32 Output = FSTPlatformAccess::GetAttachmentOutputIndex(Smart, bUpstream ? Neighbour : Exit);
					if (Output != INDEX_NONE)
					{
						SplitterOutputFilter(Smart, Output, Filter, bShared);
					}
					Result.bShared |= bShared;
				}
				Push(Step, Exit, nullptr, StepLabel, Filter, bShared);
			}
		}

		void StepPipe(const FStep& Step)
		{
			UFGPipeConnectionComponentBase* Neighbour = Step.Pipe->GetConnection();
			AActor* Owner = Neighbour ? Neighbour->GetOwner() : nullptr;
			if (!Owner)
			{
				AddUnique(Result.Notes, TEXT("a pipe connection leads nowhere"));
				return;
			}
			if (Visited.Contains(Owner))
			{
				return;
			}
			Visited.Add(Owner);
			// A machine on the other end must produce (upstream) or consume (downstream) at that connection.
			const EPipeConnectionType Type = Neighbour->GetPipeConnectionType();
			const EPipeConnectionType Wrong = bUpstream ? EPipeConnectionType::PCT_CONSUMER : EPipeConnectionType::PCT_PRODUCER;
			if (Type == Wrong)
			{
				AddUnique(Result.Notes, FString::Printf(TEXT("%s connects the wrong way round"), *Label(Owner)));
				return;
			}
			if (ReadMachine(Owner, Step, true))
			{
				return;
			}
			IFGFluidIntegrantInterface* Integrant = Cast<IFGFluidIntegrantInterface>(Owner);
			if (!Integrant)
			{
				AddUnique(Result.Notes, FString::Printf(TEXT("%s is not a recognised source or sink"), *Label(Owner)));
				return;
			}
			TArray<UFGPipeConnectionComponent*> Exits;
			for (UFGPipeConnectionComponent* Connection : Integrant->GetPipeConnections())
			{
				if (Connection && Connection != Neighbour && Connection->IsConnected())
				{
					Exits.Add(Connection);
				}
			}
			if (Exits.Num() > 1)
			{
				Result.bBranched = true;
			}
			if (Exits.Num() == 0)
			{
				AddUnique(Result.Notes, FString::Printf(TEXT("%s is a dead end"), *Label(Owner)));
			}
			for (UFGPipeConnectionComponent* Exit : Exits)
			{
				FStep Next;
				Next.Pipe = Exit;
				Next.bShared = Step.bShared;
				Next.Filter = Step.Filter;
				Next.Labels = Step.Labels;
				Next.Labels.Add(Label(Owner));
				Stack.Add(MoveTemp(Next));
			}
		}
	};
}

const TCHAR* STTrace::ConfidenceName(EConfidence Confidence)
{
	switch (Confidence)
	{
	case EConfidence::High: return TEXT("high");
	case EConfidence::Medium: return TEXT("medium");
	default: return TEXT("unclear");
	}
}

TArray<TSubclassOf<UFGItemDescriptor>> STTrace::FResult::Items() const
{
	TArray<TSubclassOf<UFGItemDescriptor>> Out;
	for (const FFlow& Flow : Flows)
	{
		Out.AddUnique(Flow.Item);
	}
	return Out;
}

FString STTrace::FResult::Summary() const
{
	FString Text;
	for (const FFlow& Flow : Flows)
	{
		Text += FString::Printf(TEXT("%s%s%s"), Text.IsEmpty() ? TEXT("") : TEXT(", "), *STNames::Item(Flow.Item),
			Flow.PerMinute > 0.0f ? *FString::Printf(TEXT(" x%.0f/min"), Flow.PerMinute) : TEXT(""));
	}
	if (Text.IsEmpty())
	{
		Text = Machines > 0 ? FString::Printf(TEXT("%d machines reached, but their items are filtered away or of the other cargo kind"), Machines)
			: FString(TEXT("no machine reached"));
	}
	FString Out = FString::Printf(TEXT("%s: %s"), ConfidenceName(Confidence), *Text);
	if (Flows.Num() > 0)
	{
		Out += FString::Printf(TEXT(" via %s"), *Flows[0].Path);
	}
	if (Notes.Num() > 0)
	{
		Out += TEXT(" (") + FString::Join(Notes, TEXT("; ")) + TEXT(")");
	}
	return Out;
}

STTrace::FResult STTrace::Trace(const AFGBuildableTrainPlatformCargo* Platform, bool bLoadMode, int32 Limit)
{
	FResult Result;
	if (!Platform)
	{
		return Result;
	}
	FTracer(bLoadMode, FMath::Clamp(Limit, MinLimit, MaxLimit), Result).Run(Platform);

	// Several fluids on one network can't be told apart here.
	int32 Fluids = 0;
	for (const FFlow& Flow : Result.Flows)
	{
		Fluids += IsFluid(Flow.Item) ? 1 : 0;
	}
	if (Result.Machines == 0 || Result.Flows.Num() == 0 || Fluids > 1)
	{
		Result.Confidence = EConfidence::Unclear;
		if (Fluids > 1)
		{
			AddUnique(Result.Notes, TEXT("several fluids on one pipe network"));
		}
	}
	else if (Result.bBranched || Result.bShared || Result.bIncomplete || Result.Notes.Num() > 0)
	{
		Result.Confidence = EConfidence::Medium;
	}
	else
	{
		Result.Confidence = EConfidence::High;
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("TRACE %s %s (%d visited): %s"), *GetNameSafe(Platform), bLoadMode ? TEXT("upstream") : TEXT("downstream"), Result.Nodes,
		*Result.Summary());
	return Result;
}
