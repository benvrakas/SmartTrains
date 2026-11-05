#include "STTrace.h"
#include "Buildables/FGBuildableConveyorBase.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableResourceExtractor.h"
#include "Buildables/FGBuildableResourceExtractorBase.h"
#include "Buildables/FGBuildableSplitterSmart.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGFactoryConnectionComponent.h"
#include "FGFluidIntegrantInterface.h"
#include "FGPipeConnectionComponent.h"
#include "FGRecipe.h"
#include "STLayout.h"
#include "Resources/FGExtractableResourceInterface.h"
#include "Resources/FGItemDescriptor.h"
#include "SmartTrains.h"
#include "STTypes.h"

namespace
{
	// Hops from the platform and machines/nodes visited before the walk gives up (marked incomplete).
	constexpr int32 MaxHops = 40;
	constexpr int32 MaxNodes = 80;

	// One walker position: the connection to step through, the path so far and what the path has filtered.
	struct FStep
	{
		UFGFactoryConnectionComponent* Belt = nullptr;
		UFGPipeConnectionComponent* Pipe = nullptr;
		int32 Hops = 0;
		bool bShared = false;
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

	class FTracer
	{
	public:
		FTracer(bool bInLoadMode, STTrace::FResult& InResult) : bUpstream(bInLoadMode), Result(InResult) {}

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
			while (Stack.Num() > 0)
			{
				FStep Step = Stack.Pop();
				if (Visited.Num() >= MaxNodes)
				{
					Result.bIncomplete = true;
					AddUnique(Result.Notes, FString::Printf(TEXT("stopped after %d nodes"), MaxNodes));
					break;
				}
				if (Step.Belt)
				{
					StepBelt(Step);
				}
				else if (Step.Pipe)
				{
					StepPipe(Step);
				}
			}
		}

	private:
		bool bUpstream;
		STTrace::FResult& Result;
		TArray<FStep> Stack;
		TSet<const AActor*> Visited;

		void AddFlow(TSubclassOf<UFGItemDescriptor> Item, float PerMinute, const FStep& Step, const AActor* Machine)
		{
			if (!Item)
			{
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

		void StepBelt(const FStep& Step)
		{
			UFGFactoryConnectionComponent* Neighbour = Step.Belt->GetConnection();
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
			Visited.Add(Owner);
			if (ReadMachine(Owner, Step, false))
			{
				return;
			}
			if (Step.Hops >= MaxHops)
			{
				Result.bIncomplete = true;
				AddUnique(Result.Notes, FString::Printf(TEXT("stopped after %d hops"), MaxHops));
				return;
			}
			const bool bSmart = Owner->IsA<AFGBuildableSplitterSmart>();
			const bool bBelt = Owner->IsA<AFGBuildableConveyorBase>();
			TInlineComponentArray<UFGFactoryConnectionComponent*> Connections;
			Owner->GetComponents(Connections);
			const EFactoryConnectionDirection ExitDirection = bUpstream ? EFactoryConnectionDirection::FCD_INPUT : EFactoryConnectionDirection::FCD_OUTPUT;
			TArray<UFGFactoryConnectionComponent*> Exits;
			for (UFGFactoryConnectionComponent* Connection : Connections)
			{
				if (Connection && Connection != Neighbour && Connection->IsConnected() && Connection->GetDirection() == ExitDirection)
				{
					Exits.Add(Connection);
				}
			}
			if (Exits.Num() == 0)
			{
				AddUnique(Result.Notes, FString::Printf(TEXT("%s is not a recognised source or sink"), *Label(Owner)));
				return;
			}
			if (Exits.Num() > 1)
			{
				Result.bBranched = true;
			}
			for (UFGFactoryConnectionComponent* Exit : Exits)
			{
				FStep Next;
				Next.Belt = Exit;
				Next.Hops = Step.Hops + 1;
				Next.bShared = Step.bShared || bSmart;
				Next.Labels = Step.Labels;
				Next.Labels.Add(bBelt ? FString(TEXT("Belt")) : Label(Owner));
				Stack.Add(MoveTemp(Next));
			}
			if (bSmart)
			{
				Result.bShared = true;
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
			if (Step.Hops >= MaxHops)
			{
				Result.bIncomplete = true;
				AddUnique(Result.Notes, FString::Printf(TEXT("stopped after %d hops"), MaxHops));
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
				Next.Hops = Step.Hops + 1;
				Next.bShared = Step.bShared;
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
		Text = TEXT("no machine reached");
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

STTrace::FResult STTrace::Trace(const AFGBuildableTrainPlatformCargo* Platform, bool bLoadMode)
{
	FResult Result;
	if (!Platform)
	{
		return Result;
	}
	FTracer(bLoadMode, Result).Run(Platform);
	Result.Nodes = Result.Machines;

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
	UE_LOG(LogSmartTrains, Verbose, TEXT("TRACE %s %s: %s"), *GetNameSafe(Platform), bLoadMode ? TEXT("upstream") : TEXT("downstream"), *Result.Summary());
	return Result;
}
