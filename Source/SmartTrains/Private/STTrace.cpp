#include "STTrace.h"
#include "STMinCostFlow.h"
#include "Buildables/FGBuildableConveyorBase.h"
#include "Buildables/FGBuildableConveyorLift.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildablePassthrough.h"
#include "Buildables/FGBuildableResourceExtractor.h"
#include "Buildables/FGBuildableResourceExtractorBase.h"
#include "Buildables/FGBuildableSplitterSmart.h"
#include "Buildables/FGBuildableTrainPlatform.h"
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
		// The buildings that narrowed the filter, for the report.
		TArray<FString> By;

		bool Passes(UClass* Item) const
		{
			return (!bRestricted || Allowed.Contains(Item)) && !Excluded.Contains(Item);
		}

		bool IsEmpty() const
		{
			return bRestricted && Allowed.Num() == 0;
		}

		void Restrict(const TArray<UClass*>& Items, const FString& Where)
		{
			By.AddUnique(Where);
			if (!bRestricted)
			{
				bRestricted = true;
				Allowed = Items;
				return;
			}
			Allowed.RemoveAll([&Items](UClass* Item) { return !Items.Contains(Item); });
		}

		void Exclude(const TArray<UClass*>& Items, const FString& Where)
		{
			if (Items.Num() > 0)
			{
				By.AddUnique(Where);
			}
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

	// Items a balancer filter module is set to; empty for every other kind of module.
	TArray<UClass*> GetBalancerFilter(const AFGBuildable* Module)
	{
		TArray<UClass*> Items;
		const FString ClassName = Module ? Module->GetClass()->GetName() : FString();
		if (!ClassName.Contains(TEXT("Filter")) && !ClassName.Contains(TEXT("Programmable")))
		{
			return Items;
		}
		for (UObject* Each : GetObjectArrayField(Module, TEXT("mFilteredItems")))
		{
			if (UClass* Item = Cast<UClass>(Each); Item && Item->IsChildOf(UFGItemDescriptor::StaticClass()))
			{
				Items.AddUnique(Item);
			}
		}
		if (UClass* Single = Cast<UClass>(GetObjectField(Module, TEXT("mFilteredItem"))); Single && Single->IsChildOf(UFGItemDescriptor::StaticClass()))
		{
			Items.AddUnique(Single);
		}
		// Special descriptors (any, none, overflow) are not item filters.
		Items.RemoveAll([](UClass* Item)
		{
			return Item->IsChildOf(UFGWildCardDescriptor::StaticClass()) || Item->IsChildOf(UFGAnyUndefinedDescriptor::StaticClass())
				|| Item->IsChildOf(UFGOverflowDescriptor::StaticClass()) || Item->IsChildOf(UFGNoneDescriptor::StaticClass());
		});
		return Items;
	}

	class FTracer
	{
	public:
		FTracer(bool bInLoadMode, int32 InLimit, STTrace::FResult& InResult) : bUpstream(bInLoadMode), Limit(InLimit), Result(InResult) {}

		void Run(const AFGBuildableTrainPlatformCargo* Platform)
		{
			SeedBelts(Platform);
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
			Walk(Platform);
		}

	private:
		void SeedBelts(const AActor* Start)
		{
			TInlineComponentArray<UFGFactoryConnectionComponent*> Belts;
			Start->GetComponents(Belts);
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
		}

		void Walk(const AActor* Start)
		{
			Visited.Add(Start);
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
				AddUnique(Result.Notes, FString::Printf(TEXT("%s from %s is filtered away at %s"), *STNames::Item(Item), *Label(Machine), *FString::Join(Step.Filter.By, TEXT(", "))));
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

		// A train platform on the way: noted, and the walk ends there (it is another station's, or this one's).
		bool StopAtPlatform(const AActor* Owner)
		{
			if (!Owner->IsA<AFGBuildableTrainPlatform>())
			{
				return false;
			}
			Result.PlatformActors.AddUnique(Owner);
			return true;
		}

		// A machine's recipe: what it makes (upstream) or what it eats (downstream). Fluids and solids are kept apart by bWantFluid.
		bool ReadMachine(AActor* Owner, const FStep& Step, bool bWantFluid)
		{
			if (const AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Owner))
			{
				++Result.Machines;
				Result.MachineActors.AddUnique(Owner);
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
				Result.MachineActors.AddUnique(Owner);
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
			// Without rules it sorts nothing and works as a plain splitter.
			if (Splitter->GetSortRules().Num() == 0)
			{
				return;
			}
			const FString Where = FString::Printf(TEXT("%s output %d"), *Label(Splitter), OutputIndex + 1);
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
				InOutFilter.Exclude(ElsewhereExplicit, Where);
				return;
			}
			InOutFilter.Restrict(Explicit, Where);
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
			if (StopAtPlatform(Owner))
			{
				return;
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
						Filter.Restrict(Own, Label(Owner));
					}
					else
					{
						for (const AFGBuildable* Module : Group)
						{
							Filter.Exclude(GetBalancerFilter(Module), Label(Module));
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
							ExitFilter.Restrict(ModuleFilter, Label(Module));
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
			if (StopAtPlatform(Owner))
			{
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

STTrace::FFlood STTrace::Flood(const TArray<const AActor*>& Starts, bool bUpstream, int32 Limit)
{
	FFlood Result;
	// One position of the walk: a belt or pipe connection to follow, and the machine the walk last passed (the one that
	// feeds what comes next downstream, or is fed by it upstream).
	struct FItem
	{
		UFGFactoryConnectionComponent* Belt = nullptr;
		UFGFactoryConnectionComponent* Arrive = nullptr;
		UFGPipeConnectionComponent* Pipe = nullptr;
		const AActor* Machine = nullptr;
	};
	TArray<FItem> Stack;
	TSet<const AActor*> Visited;
	const EFactoryConnectionDirection Out = bUpstream ? EFactoryConnectionDirection::FCD_INPUT : EFactoryConnectionDirection::FCD_OUTPUT;
	const EFactoryConnectionDirection Facing = bUpstream ? EFactoryConnectionDirection::FCD_OUTPUT : EFactoryConnectionDirection::FCD_INPUT;
	const EPipeConnectionType MachinePipe = bUpstream ? EPipeConnectionType::PCT_CONSUMER : EPipeConnectionType::PCT_PRODUCER;
	const EPipeConnectionType WrongPipe = bUpstream ? EPipeConnectionType::PCT_CONSUMER : EPipeConnectionType::PCT_PRODUCER;
	// A machine's connections on the walking side.
	auto SeedMachine = [&](const AActor* Machine)
	{
		TInlineComponentArray<UFGFactoryConnectionComponent*> Belts;
		Machine->GetComponents(Belts);
		for (UFGFactoryConnectionComponent* Belt : Belts)
		{
			if (Belt && Belt->IsConnected() && Belt->GetDirection() == Out)
			{
				Stack.Add({Belt, nullptr, nullptr, Machine});
			}
		}
		TInlineComponentArray<UFGPipeConnectionComponent*> Pipes;
		Machine->GetComponents(Pipes);
		for (UFGPipeConnectionComponent* Pipe : Pipes)
		{
			if (Pipe && Pipe->IsConnected() && Pipe->GetPipeConnectionType() == MachinePipe)
			{
				Stack.Add({nullptr, nullptr, Pipe, Machine});
			}
		}
	};
	auto IsMachine = [](const AActor* Actor) { return Actor->IsA<AFGBuildableManufacturer>() || Actor->IsA<AFGBuildableResourceExtractorBase>(); };
	// Reached Owner from Item: a platform ends the walk there, a machine joins and the walk goes on through it.
	auto Reach = [&](const AActor* Owner, const FItem& Item) -> bool
	{
		if (Owner->IsA<AFGBuildableTrainPlatform>())
		{
			Result.Platforms.AddUnique(Owner);
			if (Item.Machine)
			{
				Result.PlatformsByMachine.FindOrAdd(Item.Machine).AddUnique(Owner);
			}
			return true;
		}
		if (IsMachine(Owner))
		{
			Result.Machines.Add(Owner);
			SeedMachine(Owner);
			return true;
		}
		return false;
	};
	for (const AActor* Start : Starts)
	{
		if (Start && !Visited.Contains(Start))
		{
			Visited.Add(Start);
			Result.Machines.Add(Start);
			SeedMachine(Start);
		}
	}
	while (Stack.Num() > 0)
	{
		if (Visited.Num() >= Limit)
		{
			Result.bCut = true;
			break;
		}
		const FItem Item = Stack.Pop(EAllowShrinking::No);
		if (Item.Belt || Item.Arrive)
		{
			UFGFactoryConnectionComponent* Neighbour = Item.Arrive ? Item.Arrive : Item.Belt->GetConnection();
			AFGBuildable* Owner = Neighbour ? Neighbour->GetOuterBuildable() : nullptr;
			if (!Owner || (Neighbour->GetDirection() != Facing && Neighbour->GetDirection() != EFactoryConnectionDirection::FCD_ANY) || Visited.Contains(Owner))
			{
				continue;
			}
			Visited.Add(Owner);
			if (Reach(Owner, Item))
			{
				continue;
			}
			// Modular Load Balancers: every module of the group, out of all its connections on the walking side.
			if (IsBalancer(Owner))
			{
				for (AFGBuildable* Module : GetBalancerGroup(Owner))
				{
					Visited.Add(Module);
					TInlineComponentArray<UFGFactoryConnectionComponent*> Connections;
					Module->GetComponents(Connections);
					for (UFGFactoryConnectionComponent* Connection : Connections)
					{
						if (Connection && Connection != Neighbour && Connection->IsConnected() && Connection->GetDirection() == Out)
						{
							Stack.Add({Connection, nullptr, nullptr, Item.Machine});
						}
					}
				}
				continue;
			}
			// Belts and lifts: their other end, or through the floor hole a lift ends in.
			if (AFGBuildableConveyorBase* Conveyor = Cast<AFGBuildableConveyorBase>(Owner))
			{
				UFGFactoryConnectionComponent* Other = Conveyor->GetConnection0() == Neighbour ? Conveyor->GetConnection1() : Conveyor->GetConnection0();
				if (Other && Other->IsConnected())
				{
					Stack.Add({Other, nullptr, nullptr, Item.Machine});
				}
				else if (AFGBuildableConveyorLift* Lift = Cast<AFGBuildableConveyorLift>(Conveyor))
				{
					for (AFGBuildablePassthrough* Hole : Lift->GetSnappedPassthroughs())
					{
						for (UFGFactoryConnectionComponent* Far : {Hole ? Hole->GetTopSnappedConnection<UFGFactoryConnectionComponent>() : nullptr,
							Hole ? Hole->GetBottomSnappedConnection<UFGFactoryConnectionComponent>() : nullptr})
						{
							if (Far && Far->GetOuterBuildable() != Lift)
							{
								Stack.Add({nullptr, Far, nullptr, Item.Machine});
							}
						}
					}
				}
				continue;
			}
			// Splitters, mergers, storage and the rest: on through every connection on the walking side.
			TInlineComponentArray<UFGFactoryConnectionComponent*> Connections;
			Owner->GetComponents(Connections);
			for (UFGFactoryConnectionComponent* Connection : Connections)
			{
				const EFactoryConnectionDirection Direction = Connection ? Connection->GetDirection() : EFactoryConnectionDirection::FCD_ANY;
				if (Connection && Connection != Neighbour && Connection->IsConnected() && (Direction == Out || Direction == EFactoryConnectionDirection::FCD_ANY))
				{
					Stack.Add({Connection, nullptr, nullptr, Item.Machine});
				}
			}
		}
		else if (Item.Pipe)
		{
			UFGPipeConnectionComponentBase* Neighbour = Item.Pipe->GetConnection();
			AActor* Owner = Neighbour ? Neighbour->GetOwner() : nullptr;
			// A machine reached through the wrong side of a pipe (its input while walking upstream) isn't marked: another
			// path may reach it the right way.
			if (!Owner || Visited.Contains(Owner) || (IsMachine(Owner) && Neighbour->GetPipeConnectionType() == WrongPipe))
			{
				continue;
			}
			Visited.Add(Owner);
			if (Reach(Owner, Item))
			{
				continue;
			}
			if (IFGFluidIntegrantInterface* Integrant = Cast<IFGFluidIntegrantInterface>(Owner))
			{
				for (UFGPipeConnectionComponent* Connection : Integrant->GetPipeConnections())
				{
					if (Connection && Connection != Neighbour && Connection->IsConnected())
					{
						Stack.Add({nullptr, nullptr, Connection, Item.Machine});
					}
				}
			}
		}
	}
	Result.Nodes = Visited.Num();
	return Result;
}

STTrace::FBeltCapacity STTrace::BeltCapacity(const TArray<const AActor*>& Sources, const TSet<const AActor*>& Sinks, int32 Limit)
{
	FBeltCapacity Result;
	// A link with no belt on it (a splitter plugged straight into a machine).
	constexpr double Unlimited = 1e12;
	constexpr int32 SourceNode = 0;
	constexpr int32 SinkNode = 1;
	struct FLink
	{
		int32 From = 0;
		int32 To = 0;
		double Capacity = Unlimited;
	};
	TArray<FLink> Links;
	// One node per building; a balancer group is one node (its leader).
	TMap<const AActor*, int32> NodeOf;
	TArray<const AActor*> Open;
	int32 NodeCount = 2;
	int32 Steps = 0;
	// Machines and platforms take what arrives; anything else passes it on through its outputs.
	auto PassesOn = [](const AActor* Actor)
	{
		return !Actor->IsA<AFGBuildableTrainPlatform>() && !Actor->IsA<AFGBuildableManufacturer>() && !Actor->IsA<AFGBuildableResourceExtractorBase>();
	};
	auto NodeFor = [&](const AActor* Actor) -> int32
	{
		const AActor* Key = Actor;
		if (IsBalancer(Actor))
		{
			if (const AActor* Leader = Cast<AActor>(GetObjectField(Actor, TEXT("GroupLeader"))))
			{
				Key = Leader;
			}
		}
		if (const int32* Found = NodeOf.Find(Key))
		{
			return *Found;
		}
		const int32 Node = NodeCount++;
		NodeOf.Add(Key, Node);
		if (Sinks.Contains(Actor))
		{
			Links.Add({Node, SinkNode, Unlimited});
		}
		else if (PassesOn(Actor))
		{
			Open.Add(Key);
		}
		return Node;
	};
	// From one output through a run of belts and lifts (and the floor holes lifts end in) to the next building.
	auto Follow = [&](int32 From, const UFGFactoryConnectionComponent* Output)
	{
		const UFGFactoryConnectionComponent* Partner = Output->GetConnection();
		double Capacity = Unlimited;
		TSet<const AActor*> Run;
		while (Partner)
		{
			if (++Steps > Limit)
			{
				Result.bCut = true;
				return;
			}
			AFGBuildable* Owner = Partner->GetOuterBuildable();
			// Against the flow (an "any" connection into the end of a belt), or round a loop of belts.
			if (!Owner || Partner->GetDirection() == EFactoryConnectionDirection::FCD_OUTPUT || Run.Contains(Owner))
			{
				return;
			}
			AFGBuildableConveyorBase* Conveyor = Cast<AFGBuildableConveyorBase>(Owner);
			if (!Conveyor)
			{
				Links.Add({From, NodeFor(Owner), Capacity});
				return;
			}
			Run.Add(Conveyor);
			// Belt speed is in cm/s; items sit ITEM_SPACING apart.
			Capacity = FMath::Min(Capacity, Conveyor->GetSpeed() * 60.0 / AFGBuildableConveyorBase::ITEM_SPACING);
			const UFGFactoryConnectionComponent* Other = Conveyor->GetConnection0() == Partner ? Conveyor->GetConnection1() : Conveyor->GetConnection0();
			Partner = nullptr;
			if (Other && Other->IsConnected())
			{
				Partner = Other->GetConnection();
			}
			else if (AFGBuildableConveyorLift* Lift = Cast<AFGBuildableConveyorLift>(Conveyor))
			{
				for (AFGBuildablePassthrough* Hole : Lift->GetSnappedPassthroughs())
				{
					for (UFGFactoryConnectionComponent* Far : {Hole ? Hole->GetTopSnappedConnection<UFGFactoryConnectionComponent>() : nullptr,
						Hole ? Hole->GetBottomSnappedConnection<UFGFactoryConnectionComponent>() : nullptr})
					{
						if (Far && Far->GetOuterBuildable() != Lift)
						{
							Partner = Far;
						}
					}
				}
			}
		}
	};
	// A building's outputs: a platform's output connections; for the rest also "any" ones (storage, mod buildings).
	auto Expand = [&](const AActor* Actor, int32 Node)
	{
		TArray<AFGBuildable*> Modules;
		if (IsBalancer(Actor))
		{
			Modules = GetBalancerGroup(const_cast<AFGBuildable*>(Cast<AFGBuildable>(Actor)));
		}
		else
		{
			Modules.Add(const_cast<AFGBuildable*>(Cast<AFGBuildable>(Actor)));
		}
		const bool bAnyToo = !Actor->IsA<AFGBuildableTrainPlatform>();
		for (const AFGBuildable* Module : Modules)
		{
			if (!Module)
			{
				continue;
			}
			TInlineComponentArray<UFGFactoryConnectionComponent*> Connections;
			Module->GetComponents(Connections);
			for (const UFGFactoryConnectionComponent* Connection : Connections)
			{
				const EFactoryConnectionDirection Direction = Connection ? Connection->GetDirection() : EFactoryConnectionDirection::FCD_INPUT;
				if (Connection && Connection->IsConnected() && (Direction == EFactoryConnectionDirection::FCD_OUTPUT || (bAnyToo && Direction == EFactoryConnectionDirection::FCD_ANY)))
				{
					Follow(Node, Connection);
				}
			}
		}
	};
	for (const AActor* Source : Sources)
	{
		if (Source && !NodeOf.Contains(Source))
		{
			const int32 Node = NodeCount++;
			NodeOf.Add(Source, Node);
			Links.Add({SourceNode, Node, Unlimited});
			Expand(Source, Node);
		}
	}
	while (Open.Num() > 0 && !Result.bCut)
	{
		const AActor* Actor = Open.Pop(EAllowShrinking::No);
		Expand(Actor, NodeOf[Actor]);
	}
	if (!Links.ContainsByPredicate([](const FLink& Link) { return Link.To == SinkNode; }))
	{
		return Result;
	}

	FSTMinCostFlow Flow(NodeCount);
	for (const FLink& Link : Links)
	{
		Flow.AddEdge(Link.From, Link.To, Link.Capacity, 0.0);
	}
	const double Total = Flow.SolveMaxFlow(SourceNode, SinkNode);
	if (Total <= 0.0 || Total >= Unlimited * 0.5)
	{
		// Nothing gets through, or something reaches a sink without any belt on the way: no belt limit to state.
		Result.PerMinute = Total <= 0.0 ? 0.0 : -1.0;
		return Result;
	}
	Result.PerMinute = Total;
	// The belt runs from the reachable side to the rest: the narrowest set the flow has to pass.
	const TArray<bool> Side = Flow.GetSourceSide(SourceNode);
	TMap<int32, int32> BySpeed;
	for (const FLink& Link : Links)
	{
		if (Side[Link.From] && !Side[Link.To] && Link.Capacity < Unlimited)
		{
			++BySpeed.FindOrAdd(FMath::RoundToInt(Link.Capacity));
			++Result.CutBelts;
		}
	}
	BySpeed.KeySort([](int32 A, int32 B) { return A > B; });
	TArray<FString> Parts;
	for (const TPair<int32, int32>& Each : BySpeed)
	{
		Parts.Add(FString::Printf(TEXT("%d belt%s at %d/min"), Each.Value, Each.Value == 1 ? TEXT("") : TEXT("s"), Each.Key));
	}
	Result.Cut = FString::Join(Parts, TEXT(" + "));
	return Result;
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
