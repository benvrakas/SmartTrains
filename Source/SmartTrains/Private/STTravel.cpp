#include "STTravel.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableRailroadTrack.h"
#include "FGRailroadTrackConnectionComponent.h"
#include "FGTrainPlatformConnection.h"
#include "FGTrainStationIdentifier.h"
#include "SmartTrains.h"
#include "STLayout.h"

namespace
{
	// The station track's connections: Platform = the end the platforms are on (trains arrive through it), Far = the
	// other end (trains leave through it).
	bool GetStationEnds(const AFGTrainStationIdentifier* Identifier, UFGRailroadTrackConnectionComponent*& OutPlatform, UFGRailroadTrackConnectionComponent*& OutFar, float& OutLength)
	{
		AFGBuildableRailroadStation* Station = Identifier ? Identifier->GetStation() : nullptr;
		AFGBuildableRailroadTrack* Track = Station ? Station->GetTrackPosition().Track.Get() : nullptr;
		UFGTrainPlatformConnection* Output = Station ? Station->GetStationOutputConnection() : nullptr;
		UFGRailroadTrackConnectionComponent* PlatformEnd = Output ? Output->GetRailroadConnectionReference() : nullptr;
		if (!Track || !PlatformEnd)
		{
			return false;
		}
		UFGRailroadTrackConnectionComponent* A = Track->GetConnection(0);
		UFGRailroadTrackConnectionComponent* B = Track->GetConnection(1);
		if (PlatformEnd != A && PlatformEnd != B)
		{
			// The reference may sit on the first platform's track; pick the station end nearest to it.
			const FVector Where = PlatformEnd->GetConnectorLocation();
			PlatformEnd = (A && B && FVector::DistSquared(A->GetConnectorLocation(), Where) > FVector::DistSquared(B->GetConnectorLocation(), Where)) ? B : A;
		}
		OutPlatform = PlatformEnd;
		OutFar = PlatformEnd == A ? B : A;
		OutLength = Track->GetLength();
		return OutPlatform && OutFar;
	}
}

void FSTTravel::Reset()
{
	mRows.Reset();
	mOrder.Reset();
	mNext = 0;
}

void FSTTravel::Step(const TArray<AFGTrainStationIdentifier*>& Stations)
{
	bool bChanged = Stations.Num() != mOrder.Num();
	for (int32 Index = 0; !bChanged && Index < Stations.Num(); ++Index)
	{
		bChanged = mOrder[Index].Get() != Stations[Index];
	}
	if (bChanged)
	{
		mOrder.Reset();
		for (AFGTrainStationIdentifier* Station : Stations)
		{
			mOrder.Add(Station);
		}
		mNext = 0;
		for (auto It = mRows.CreateIterator(); It; ++It)
		{
			if (!Stations.Contains(It.Key().Get()))
			{
				It.RemoveCurrent();
			}
		}
	}
	if (mOrder.Num() == 0)
	{
		return;
	}
	mNext = mNext % mOrder.Num();
	AFGTrainStationIdentifier* Source = mOrder[mNext++].Get();

	TMap<UFGRailroadTrackConnectionComponent*, AFGTrainStationIdentifier*> Arrivals;
	TMap<UFGRailroadTrackConnectionComponent*, AFGTrainStationIdentifier*> BackArrivals;
	TMap<AFGTrainStationIdentifier*, float> ArrivalExtra;
	for (AFGTrainStationIdentifier* Station : Stations)
	{
		UFGRailroadTrackConnectionComponent* PlatformEnd = nullptr;
		UFGRailroadTrackConnectionComponent* FarEnd = nullptr;
		float Length = 0.f;
		if (GetStationEnds(Station, PlatformEnd, FarEnd, Length))
		{
			Arrivals.Add(PlatformEnd, Station);
			BackArrivals.Add(FarEnd, Station);
			ArrivalExtra.Add(Station, Length);
		}
	}

	UFGRailroadTrackConnectionComponent* PlatformEnd = nullptr;
	UFGRailroadTrackConnectionComponent* FarEnd = nullptr;
	float Length = 0.f;
	if (!Source || !GetStationEnds(Source, PlatformEnd, FarEnd, Length))
	{
		UE_LOG(LogSmartTrains, Verbose, TEXT("Travel: station %s has no track ends, skipped"), *STLayout::Name(Source));
		return;
	}
	const double Started = FPlatformTime::Seconds();
	FRow Row;
	Search(FarEnd, Arrivals, BackArrivals, ArrivalExtra, Row.Forward, Row.ForwardBack);
	Search(PlatformEnd, Arrivals, BackArrivals, ArrivalExtra, Row.Reverse, Row.ReverseBack);
	UE_LOG(LogSmartTrains, Verbose, TEXT("Travel: from %s reaches %d stations forward, %d reversing; push-pull trains also %d forward, %d reversing from the far end (%.1f ms)"),
		*STLayout::Name(Source), Row.Forward.Num(), Row.Reverse.Num(), Row.ForwardBack.Num(), Row.ReverseBack.Num(), (FPlatformTime::Seconds() - Started) * 1000.0);
	mRows.Add(Source, MoveTemp(Row));
}

void FSTTravel::Search(UFGRailroadTrackConnectionComponent* Exit, const TMap<UFGRailroadTrackConnectionComponent*, AFGTrainStationIdentifier*>& Arrivals,
	const TMap<UFGRailroadTrackConnectionComponent*, AFGTrainStationIdentifier*>& BackArrivals, const TMap<AFGTrainStationIdentifier*, float>& ArrivalExtra,
	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, float>& Out, TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, float>& OutBack)
{
	// Dijkstra over track exits: a node is "about to leave a track through this connection".
	struct FEntry
	{
		float Distance;
		UFGRailroadTrackConnectionComponent* Exit;
		bool operator<(const FEntry& Other) const { return Distance < Other.Distance; }
	};
	TMap<UFGRailroadTrackConnectionComponent*, float> Best;
	TArray<FEntry> Heap;
	Heap.HeapPush({0.f, Exit});
	Best.Add(Exit, 0.f);
	int32 Visited = 0;
	while (Heap.Num() > 0 && Visited < 400000)
	{
		FEntry Entry;
		Heap.HeapPop(Entry);
		const float* Known = Best.Find(Entry.Exit);
		if (!Known || *Known < Entry.Distance)
		{
			continue;
		}
		++Visited;
		for (UFGRailroadTrackConnectionComponent* Entrance : Entry.Exit->GetConnections())
		{
			if (!Entrance)
			{
				continue;
			}
			if (AFGTrainStationIdentifier* const* Station = Arrivals.Find(Entrance))
			{
				const float Arrival = Entry.Distance + ArrivalExtra.FindRef(*Station);
				float& Recorded = Out.FindOrAdd(*Station, TNumericLimits<float>::Max());
				Recorded = FMath::Min(Recorded, Arrival);
			}
			if (AFGTrainStationIdentifier* const* Station = BackArrivals.Find(Entrance))
			{
				const float Arrival = Entry.Distance + ArrivalExtra.FindRef(*Station);
				float& Recorded = OutBack.FindOrAdd(*Station, TNumericLimits<float>::Max());
				Recorded = FMath::Min(Recorded, Arrival);
			}
			AFGBuildableRailroadTrack* Track = Entrance->GetTrack();
			UFGRailroadTrackConnectionComponent* Next = Entrance->GetOpposite();
			if (!Track || !Next)
			{
				continue;
			}
			const float Distance = Entry.Distance + Track->GetLength();
			float& Recorded = Best.FindOrAdd(Next, TNumericLimits<float>::Max());
			if (Distance < Recorded)
			{
				Recorded = Distance;
				Heap.HeapPush({Distance, Next});
			}
		}
	}
}

float FSTTravel::GetDistance(const AFGTrainStationIdentifier* From, const AFGTrainStationIdentifier* To, bool* bReversing, bool bPushPull) const
{
	if (bReversing)
	{
		*bReversing = false;
	}
	if (From == To && From)
	{
		return 0.f;
	}
	const FRow* Row = mRows.Find(const_cast<AFGTrainStationIdentifier*>(From));
	if (!Row)
	{
		return -1.f;
	}
	// The shortest way the train can drive: forward first, then (push-pull) any way it can leave and arrive.
	AFGTrainStationIdentifier* Key = const_cast<AFGTrainStationIdentifier*>(To);
	float Best = TNumericLimits<float>::Max();
	bool bBestReversing = false;
	auto Consider = [&](const TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, float>& Map, bool bReverse)
	{
		if (const float* Found = Map.Find(Key); Found && *Found < Best)
		{
			Best = *Found;
			bBestReversing = bReverse;
		}
	};
	Consider(Row->Forward, false);
	if (Best == TNumericLimits<float>::Max())
	{
		Consider(Row->Reverse, true);
	}
	if (bPushPull)
	{
		Consider(Row->Forward, false);
		Consider(Row->Reverse, true);
		Consider(Row->ForwardBack, false);
		Consider(Row->ReverseBack, true);
	}
	if (Best == TNumericLimits<float>::Max())
	{
		return -2.f;
	}
	if (bReversing)
	{
		*bReversing = bBestReversing;
	}
	return Best;
}
