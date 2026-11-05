#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGTrainStationIdentifier.h"
#include "SmartTrains.h"
#include "STLayout.h"
#include "STSubsystem.h"

// Station platform linking: "Same for every platform in this station" (Docs/Architecture.md, "Station linking").

namespace
{
	bool SameRules(const TArray<FSTItemRule>& A, const TArray<FSTItemRule>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			const FSTItemRule& X = A[Index];
			const FSTItemRule& Y = B[Index];
			if (X.Item != Y.Item || X.Priority != Y.Priority || X.TargetPercent != Y.TargetPercent || X.SendAtPercent != Y.SendAtPercent
				|| X.ManualRatePerMinute != Y.ManualRatePerMinute)
			{
				return false;
			}
		}
		return true;
	}

	const TCHAR* LinkName(ESTPlatformLink Link)
	{
		switch (Link)
		{
		case ESTPlatformLink::Same: return TEXT("same");
		case ESTPlatformLink::Separate: return TEXT("separate");
		default: return TEXT("auto");
		}
	}
}

ESTPlatformLink ASTSubsystem::GetStationLink(const AFGTrainStationIdentifier* Station) const
{
	const FSTStationConfig* Config = mStations.FindByPredicate([Station](const FSTStationConfig& Each) { return Each.Station == Station; });
	return Config ? Config->Link : ESTPlatformLink::Auto;
}

void ASTSubsystem::SetStationLink(AFGTrainStationIdentifier* Station, ESTPlatformLink Link, AFGBuildableTrainPlatformCargo* Source, const FString& Cause)
{
	if (!Station)
	{
		return;
	}
	FSTStationConfig* Config = FindStationConfig(Station);
	if (!Config)
	{
		Config = &mStations.AddDefaulted_GetRef();
		Config->Station = Station;
	}
	Config->Link = Link;
	UE_LOG(LogSmartTrains, Log, TEXT("STATION %s: platforms %s by %s"), *STLayout::Name(Station), LinkName(Link), *Cause);
	if (Source && IsPlatformLinked(Source))
	{
		FString Result;
		const int32 Copied = CopyPlatformToStation(Source, Cause + TEXT(" (same for the station)"), Result);
		UE_LOG(LogSmartTrains, Log, TEXT("STATION %s: %s settings copied to %d platforms"), *STLayout::Name(Station), *Source->GetName(), FMath::Max(0, Copied));
	}
}

bool ASTSubsystem::DoStationPlatformsMatch(const AFGBuildableTrainPlatformCargo* Platform) const
{
	const AFGBuildableRailroadStation* Station = STLayout::FindStation(Platform);
	if (!Station)
	{
		return false;
	}
	const EFreightCargoType Type = STLayout::GetPlatformCargoType(Platform);
	const FSTPlatformConfig* First = nullptr;
	bool bHaveFirst = false;
	for (AFGBuildableTrainPlatform* Each : STLayout::GetPlatforms(Station))
	{
		const AFGBuildableTrainPlatformCargo* Cargo = Cast<AFGBuildableTrainPlatformCargo>(Each);
		if (!Cargo || STLayout::GetPlatformCargoType(Cargo) != Type)
		{
			continue;
		}
		const FSTPlatformConfig* Config = FindPlatformConfig(Cargo);
		if (!bHaveFirst)
		{
			First = Config;
			bHaveFirst = true;
			continue;
		}
		const ESTRole RoleA = First ? First->Role : ESTRole::Off;
		const ESTRole RoleB = Config ? Config->Role : ESTRole::Off;
		if (RoleA != RoleB)
		{
			return false;
		}
		if (RoleA != ESTRole::Off && !SameRules(First->Items, Config->Items))
		{
			return false;
		}
	}
	return true;
}

bool ASTSubsystem::IsPlatformLinked(const AFGBuildableTrainPlatformCargo* Platform) const
{
	const AFGTrainStationIdentifier* Station = STLayout::GetIdentifier(STLayout::FindStation(Platform));
	if (!Station)
	{
		return false;
	}
	switch (GetStationLink(Station))
	{
	case ESTPlatformLink::Same:
		return true;
	case ESTPlatformLink::Separate:
		return false;
	default:
		return !IsFccAvailable() && DoStationPlatformsMatch(Platform);
	}
}
