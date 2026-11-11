// Depot signals (Docs/Architecture.md, "Depot signals"): a copy of the vanilla block signal whose siding behind it is a
// depot, its content classes made from the vanilla block signal's at load, the siding check and the route to a depot.

#include "STDepotSignal.h"
#include "Algo/Reverse.h"
#include "Buildables/FGBuildableRailroadTrack.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "FGColoredInstanceMeshProxy.h"
#include "FGLocomotive.h"
#include "FGRailroadTrackConnectionComponent.h"
#include "FGSchematicCategory.h"
#include "SmartTrains.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

namespace
{
	const TCHAR* BlockSignalClass = TEXT("/Game/FactoryGame/Buildable/Factory/Train/Signal/Build_RailroadBlockSignal.Build_RailroadBlockSignal_C");
	const TCHAR* BlockSignalDesc = TEXT("/Game/FactoryGame/Buildable/Factory/Train/Signal/Desc_RailroadBlockSignal.Desc_RailroadBlockSignal_C");
	const TCHAR* BlockSignalRecipe = TEXT("/Game/FactoryGame/Buildable/Factory/Train/Signal/Recipe_RailroadBlockSignal.Recipe_RailroadBlockSignal_C");
	const TCHAR* BlockSignalMesh = TEXT("/Game/FactoryGame/Buildable/Factory/TrainSignalType1/Mesh/SM_TrainSignalType01_01.SM_TrainSignalType01_01");
	const TCHAR* BlockSignalIcon = TEXT("/Game/FactoryGame/Buildable/Factory/Train/Signal/UI/IconDesc_BlockSignal_512.IconDesc_BlockSignal_512");
	const TCHAR* BlockSignalIcon256 = TEXT("/Game/FactoryGame/Buildable/Factory/Train/Signal/UI/IconDesc_BlockSignal_256.IconDesc_BlockSignal_256");
	const TCHAR* ItemCoupon = TEXT("/Game/FactoryGame/Resource/Parts/ResourceSinkCoupon/Desc_ResourceSinkCoupon.Desc_ResourceSinkCoupon_C");
	const TCHAR* ShopCategoryVehicles = TEXT("/Game/FactoryGame/Schematics/ResourceSinkShopCategories/SC_RSS_Vehicles.SC_RSS_Vehicles_C");
	const TCHAR* BuildGun = TEXT("/Game/FactoryGame/Equipment/BuildGun/BP_BuildGun.BP_BuildGun_C");
	// Route search: most track exits looked at. [connections]
	constexpr int32 RouteSearchLimit = 50000;
	// Siding walk: most tracks from the signal back to its entry signal.
	constexpr int32 SidingWalkLimit = 200;

	// Copies From's editable property values declared in FirstOwner and its subclasses up to Common onto To; object
	// references to From's own subobjects (components) are left out. Returns how many were copied.
	int32 CopyEditable(const UObject* From, UObject* To, const UClass* Common, const UClass* FirstOwner, const TSet<FName>& Skip)
	{
		int32 Copied = 0;
		for (TFieldIterator<FProperty> It(Common, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			const UClass* Owner = Property->GetOwnerClass();
			if (!Owner || !Owner->IsChildOf(FirstOwner) || !Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient)
				|| Skip.Contains(Property->GetFName()) || Property->IsA<FMulticastDelegateProperty>())
			{
				continue;
			}
			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				const UObject* Value = ObjectProperty->GetObjectPropertyValue_InContainer(From);
				if (Value && Value->GetOuter() == From)
				{
					continue;
				}
			}
			Property->CopyCompleteValue_InContainer(To, From);
			++Copied;
		}
		return Copied;
	}

	// The connections at one joint: Connection and those it is joined to.
	bool HasSignalAt(const UFGRailroadTrackConnectionComponent* Connection, const AFGBuildableRailroadSignal* Except)
	{
		auto Signalled = [Except](const UFGRailroadTrackConnectionComponent* Each)
		{
			return Each && ((Each->GetFacingSignal() && Each->GetFacingSignal() != Except) || (Each->GetTrailingSignal() && Each->GetTrailingSignal() != Except));
		};
		if (Signalled(Connection))
		{
			return true;
		}
		for (const UFGRailroadTrackConnectionComponent* Other : Connection ? Connection->GetConnections() : TArray<UFGRailroadTrackConnectionComponent*>())
		{
			if (Signalled(Other))
			{
				return true;
			}
		}
		return false;
	}
}

//~ Siding

FSTDepotSiding STDepots::Check(const AFGBuildableRailroadSignal* Signal)
{
	FSTDepotSiding Siding;
	if (!IsValid(Signal))
	{
		Siding.Problem = TEXT("no signal");
		return Siding;
	}
	// The exit a train stops at: of the connections at the signal, the one the signal faces a train passing through.
	UFGRailroadTrackConnectionComponent* Approach = nullptr;
	for (UFGRailroadTrackConnectionComponent* Guarded : Signal->GetGuardedConnections())
	{
		TArray<UFGRailroadTrackConnectionComponent*> Candidates = {Guarded};
		Candidates.Append(Guarded ? Guarded->GetConnections() : TArray<UFGRailroadTrackConnectionComponent*>());
		for (UFGRailroadTrackConnectionComponent* Each : Candidates)
		{
			if (Each && Each->GetFacingSignal() == Signal)
			{
				Approach = Each;
				break;
			}
		}
		if (!Approach && Guarded)
		{
			Approach = Guarded;
		}
		if (Approach)
		{
			break;
		}
	}
	if (!Approach || !Approach->GetTrack())
	{
		Siding.Problem = TEXT("not on a track yet");
		return Siding;
	}
	Siding.Goal = Approach;
	Siding.TrackGraph = Approach->GetTrack()->GetTrackGraphID();
	// Back along the approach: whole tracks until a signal (the siding's entry), stopping at a switch, station or end.
	UFGRailroadTrackConnectionComponent* Exit = Approach;
	bool bEntry = false;
	for (int32 Step = 0; Step < SidingWalkLimit && Exit; ++Step)
	{
		AFGBuildableRailroadTrack* Track = Exit->GetTrack();
		if (!Track)
		{
			break;
		}
		if (Track->GetIsOwnedByPlatform())
		{
			Siding.Problem = TEXT("a station is on the siding behind it");
			break;
		}
		Siding.Tracks.Add(Track);
		Siding.Length += Track->GetLength();
		UFGRailroadTrackConnectionComponent* Entry = Exit->GetOpposite();
		if (!Entry)
		{
			break;
		}
		if (HasSignalAt(Entry, Signal))
		{
			bEntry = true;
			break;
		}
		const TArray<UFGRailroadTrackConnectionComponent*> Before = Entry->GetConnections();
		if (Before.Num() == 0)
		{
			Siding.Problem = TEXT("the track behind it ends without an entry signal (dead-end sidings aren't supported yet)");
			break;
		}
		if (Before.Num() > 1 || (Before[0] && Before[0]->GetConnections().Num() > 1))
		{
			Siding.Problem = TEXT("a switch lies between it and the signal before it: put an entry signal after the switch, so the siding is a block of its own");
			break;
		}
		Exit = Before[0];
	}
	if (Siding.Problem.IsEmpty() && !bEntry)
	{
		Siding.Problem = TEXT("no entry signal found behind it within 200 tracks");
	}
	return Siding;
}

FString STDepots::Describe(const FSTDepotSiding& Siding)
{
	if (!Siding.Problem.IsEmpty())
	{
		return FString::Printf(TEXT("SmartTrains depot: not usable: %s."), *Siding.Problem);
	}
	return FString::Printf(TEXT("SmartTrains depot: ready, %.0f m of siding behind it (fits trains up to %.0f m)."), Siding.Length / 100.f,
		FMath::Max(0.f, Siding.Length - Margin) / 100.f);
}

//~ Route

TArray<UFGRailroadTrackConnectionComponent*> STDepots::FindRoute(const AFGLocomotive* Locomotive, const TSet<const UFGRailroadTrackConnectionComponent*>& Goals,
	const UFGRailroadTrackConnectionComponent*& OutGoal, float& OutLength)
{
	OutGoal = nullptr;
	OutLength = 0.f;
	TArray<UFGRailroadTrackConnectionComponent*> Route;
	if (!Locomotive || Goals.Num() == 0)
	{
		return Route;
	}
	const FRailroadTrackPosition& Position = Locomotive->GetTrackPosition();
	AFGBuildableRailroadTrack* Track = Position.Track.Get();
	if (!Track)
	{
		return Route;
	}
	// The exit ahead of the locomotive, the way it faces.
	UFGRailroadTrackConnectionComponent* Start = Track->GetConnection(Position.Forward >= 0.f ? 1 : 0);
	if (!Start)
	{
		return Route;
	}
	const float StartOffset = FMath::Abs(Start->GetTrackPosition().Offset - Position.Offset);
	// Dijkstra over track exits: from an exit, through each track joined there, to that track's other end.
	TMap<const UFGRailroadTrackConnectionComponent*, float> Distance;
	TMap<const UFGRailroadTrackConnectionComponent*, UFGRailroadTrackConnectionComponent*> Parent;
	TArray<TPair<float, UFGRailroadTrackConnectionComponent*>> Open;
	auto Push = [&Open](float Cost, UFGRailroadTrackConnectionComponent* Exit)
	{
		Open.HeapPush(TPair<float, UFGRailroadTrackConnectionComponent*>(Cost, Exit), [](const TPair<float, UFGRailroadTrackConnectionComponent*>& A, const TPair<float, UFGRailroadTrackConnectionComponent*>& B) { return A.Key < B.Key; });
	};
	Distance.Add(Start, StartOffset);
	Push(StartOffset, Start);
	int32 Visited = 0;
	while (Open.Num() > 0 && Visited < RouteSearchLimit)
	{
		TPair<float, UFGRailroadTrackConnectionComponent*> Top;
		Open.HeapPop(Top, [](const TPair<float, UFGRailroadTrackConnectionComponent*>& A, const TPair<float, UFGRailroadTrackConnectionComponent*>& B) { return A.Key < B.Key; });
		UFGRailroadTrackConnectionComponent* Exit = Top.Value;
		if (Top.Key > Distance.FindRef(Exit) + 1.f)
		{
			continue;
		}
		++Visited;
		if (Goals.Contains(Exit))
		{
			OutGoal = Exit;
			OutLength = Top.Key;
			for (UFGRailroadTrackConnectionComponent* Each = Exit; Each; Each = Parent.FindRef(Each))
			{
				Route.Add(Each);
			}
			Algo::Reverse(Route);
			return Route;
		}
		for (UFGRailroadTrackConnectionComponent* Entry : Exit->GetConnections())
		{
			UFGRailroadTrackConnectionComponent* Next = Entry ? Entry->GetOpposite() : nullptr;
			const AFGBuildableRailroadTrack* NextTrack = Next ? Next->GetTrack() : nullptr;
			if (!NextTrack)
			{
				continue;
			}
			const float Cost = Top.Key + NextTrack->GetLength();
			if (const float* Known = Distance.Find(Next); Known && *Known <= Cost)
			{
				continue;
			}
			Distance.Add(Next, Cost);
			Parent.Add(Next, Exit);
			Push(Cost, Next);
		}
	}
	return Route;
}

//~ Content

FText ASTBuildableDepotSignal::GetLookAtDecription_Implementation(AFGCharacterPlayer* ByCharacter, const FUseState& State) const
{
	const FText Vanilla = Super::GetLookAtDecription_Implementation(ByCharacter, State);
	const FString Depot = STDepots::Describe(STDepots::Check(this));
	return Vanilla.IsEmpty() ? FText::FromString(Depot) : FText::FromString(Vanilla.ToString() + TEXT("\n") + Depot);
}

void ASTBuildableDepotSignal::ResolveVanillaAssets()
{
	UClass* Vanilla = LoadClass<AFGBuildableRailroadSignal>(nullptr, BlockSignalClass);
	ASTBuildableDepotSignal* Mine = GetMutableDefault<ASTBuildableDepotSignal>();
	if (!Vanilla)
	{
		UE_LOG(LogSmartTrains, Error, TEXT("DEPOT signal: the vanilla block signal %s wasn't found; the depot signal can't be built"), BlockSignalClass);
		return;
	}
	const AFGBuildableRailroadSignal* Theirs = Vanilla->GetDefaultObject<AFGBuildableRailroadSignal>();
	const int32 Copied = CopyEditable(Theirs, Mine, AFGBuildableRailroadSignal::StaticClass(), AFGBuildable::StaticClass(),
		{TEXT("mDisplayName"), TEXT("mDescription")});
	Mine->mDisplayName = LOCTEXT("DepotSignalName", "Depot Signal");
	Mine->mDescription = LOCTEXT("DepotSignalDescription",
		"A Block Signal that also marks the siding behind it as a SmartTrains depot: managed trains with nothing to do wait there.\n\n"
		"Build it at the exit of a siding that is a block of its own (an entry signal after the switch, no station on it), longer than your trains.");
	// The mesh: the vanilla component's, or the block signal mesh where the vanilla blueprint puts it.
	const UFGColoredInstanceMeshProxy* Source = Theirs->FindComponentByClass<UFGColoredInstanceMeshProxy>();
	if (Mine->mSignalComponent && Source && Source->GetStaticMesh())
	{
		Mine->mSignalComponent->SetStaticMesh(Source->GetStaticMesh());
		Mine->mSignalComponent->SetRelativeTransform(Source->GetRelativeTransform());
		Mine->mSignalComponent->OverrideMaterials = Source->OverrideMaterials;
	}
	else if (Mine->mSignalComponent)
	{
		if (UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, BlockSignalMesh))
		{
			Mine->mSignalComponent->SetStaticMesh(Mesh);
		}
		Mine->mSignalComponent->SetRelativeLocationAndRotation(FVector(0.f, 450.f, 0.f), FRotator(0.f, -90.f, 0.f));
	}
	UE_LOG(LogSmartTrains, Log, TEXT("DEPOT signal: %d settings copied from the vanilla block signal, mesh %s (%s), hologram %s"), Copied,
		Mine->mSignalComponent && Mine->mSignalComponent->GetStaticMesh() ? *Mine->mSignalComponent->GetStaticMesh()->GetName() : TEXT("MISSING"),
		Source ? TEXT("from its component") : TEXT("by path"), Mine->mHologramClass ? *Mine->mHologramClass->GetName() : TEXT("MISSING"));
}

USTDesc_DepotSignal::USTDesc_DepotSignal()
{
	mBuildableClass = ASTBuildableDepotSignal::StaticClass();
}

void USTDesc_DepotSignal::ResolveVanillaAssets()
{
	USTDesc_DepotSignal* Mine = GetMutableDefault<USTDesc_DepotSignal>();
	if (UClass* Vanilla = LoadClass<UFGBuildingDescriptor>(nullptr, BlockSignalDesc))
	{
		CopyEditable(Vanilla->GetDefaultObject(), Mine, UFGBuildingDescriptor::StaticClass(), UFGItemDescriptor::StaticClass(),
			{TEXT("mBuildableClass"), TEXT("mDisplayName"), TEXT("mDescription")});
		Mine->mMenuPriority += 0.01f;
	}
	if (UTexture2D* Icon = LoadObject<UTexture2D>(nullptr, BlockSignalIcon))
	{
		Mine->mSmallIcon = Icon;
		Mine->mPersistentBigIcon = Icon;
	}
	Mine->mBuildableClass = ASTBuildableDepotSignal::StaticClass();
	UE_LOG(LogSmartTrains, Log, TEXT("DEPOT signal descriptor: category %s, icon %s"), Mine->mCategory ? *Mine->mCategory->GetName() : TEXT("none"),
		Mine->mSmallIcon ? *Mine->mSmallIcon->GetName() : TEXT("none"));
}

USTRecipe_DepotSignal::USTRecipe_DepotSignal()
{
	mProduct.Add(FItemAmount(USTDesc_DepotSignal::StaticClass(), 1));
	mManufactoringDuration = 1.0f;
	mManualManufacturingMultiplier = 1.0f;
	mProducedIn.Add(TSoftClassPtr<UObject>(FSoftObjectPath(BuildGun)));
}

void USTRecipe_DepotSignal::ResolveVanillaAssets()
{
	USTRecipe_DepotSignal* Mine = GetMutableDefault<USTRecipe_DepotSignal>();
	if (UClass* Vanilla = LoadClass<UFGRecipe>(nullptr, BlockSignalRecipe))
	{
		CopyEditable(Vanilla->GetDefaultObject(), Mine, UFGRecipe::StaticClass(), UFGRecipe::StaticClass(),
			{TEXT("mProduct"), TEXT("mDisplayName"), TEXT("mDisplayNameOverride")});
	}
	Mine->mProduct = {FItemAmount(USTDesc_DepotSignal::StaticClass(), 1)};
	UE_LOG(LogSmartTrains, Log, TEXT("DEPOT signal recipe: %d ingredients"), Mine->mIngredients.Num());
}

USTUnlockRecipe_DepotSignal::USTUnlockRecipe_DepotSignal()
{
	mRecipes.Add(USTRecipe_DepotSignal::StaticClass());
}

USTSchematic_DepotSignal::USTSchematic_DepotSignal()
{
	mType = ESchematicType::EST_ResourceSink;
	mDisplayName = LOCTEXT("DepotSchematicName", "SmartTrains Depot Signal");
	mDescription = LOCTEXT("DepotSchematicDescription",
		"A Block Signal that marks the siding behind it as a depot where SmartTrains parks trains with nothing to do.");
	mMenuPriority = 5.f;
	mTechTier = 1;
	mTimeToComplete = 0.0f;
	mUnlocks.Add(CreateDefaultSubobject<USTUnlockRecipe_DepotSignal>(TEXT("UnlockDepotSignal")));
}

void USTSchematic_DepotSignal::ResolveVanillaAssets()
{
	USTSchematic_DepotSignal* Mine = GetMutableDefault<USTSchematic_DepotSignal>();
	if (TSubclassOf<UFGItemDescriptor> Coupon = LoadClass<UFGItemDescriptor>(nullptr, ItemCoupon))
	{
		Mine->mCost = {FItemAmount(Coupon, 1)};
	}
	if (TSubclassOf<UFGSchematicCategory> Category = LoadClass<UFGSchematicCategory>(nullptr, ShopCategoryVehicles))
	{
		Mine->mSubCategories = {Category};
	}
	if (UTexture2D* Icon = LoadObject<UTexture2D>(nullptr, BlockSignalIcon256))
	{
		Mine->mSchematicIcon.SetResourceObject(Icon);
		Mine->mSchematicIcon.ImageSize = FVector2D(256.0, 256.0);
		Mine->mSmallSchematicIcon = Icon;
	}
}

void STDepots::ResolveVanillaAssets()
{
	ASTBuildableDepotSignal::ResolveVanillaAssets();
	USTDesc_DepotSignal::ResolveVanillaAssets();
	USTRecipe_DepotSignal::ResolveVanillaAssets();
	USTSchematic_DepotSignal::ResolveVanillaAssets();
}

#undef LOCTEXT_NAMESPACE
