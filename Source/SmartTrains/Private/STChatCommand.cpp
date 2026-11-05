#include "STChatCommand.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "Command/CommandSender.h"
#include "EngineUtils.h"
#include "FGCharacterPlayer.h"
#include "FGPlayerController.h"
#include "FGRailroadSubsystem.h"
#include "FGRailroadVehicle.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "GameFramework/PlayerState.h"
#include "Resources/FGItemDescriptor.h"
#include "SmartTrains.h"
#include "STLayout.h"
#include "STMigration.h"
#include "STRemoteCallObject.h"
#include "STSubsystem.h"
#include "UObject/UObjectIterator.h"

namespace
{
	FString SenderName(UCommandSender* Sender)
	{
		const AFGPlayerController* Controller = Sender ? Sender->GetPlayer() : nullptr;
		const APlayerState* State = Controller ? Controller->GetPlayerState<APlayerState>() : nullptr;
		return State ? State->GetPlayerName() : FString(TEXT("server"));
	}

	void Send(UCommandSender* Sender, const TArray<FString>& Lines, const FLinearColor& Color = FLinearColor::White)
	{
		for (const FString& Line : Lines)
		{
			Sender->SendChatMessage(Line, Color);
		}
	}

	// The actor of class T nearest to where the player looks, within 60 m and 12 m of the line of sight.
	template <typename T>
	T* FindAimed(UCommandSender* Sender)
	{
		AFGPlayerController* Controller = Sender ? Sender->GetPlayer() : nullptr;
		UWorld* World = Controller ? Controller->GetWorld() : nullptr;
		if (!World)
		{
			return nullptr;
		}
		FVector Origin;
		FRotator Rotation;
		Controller->GetPlayerViewPoint(Origin, Rotation);
		const FVector Direction = Rotation.Vector();
		T* Best = nullptr;
		double BestScore = TNumericLimits<double>::Max();
		for (TActorIterator<T> It(World); It; ++It)
		{
			const FVector Offset = It->GetActorLocation() - Origin;
			const double Along = FVector::DotProduct(Offset, Direction);
			if (Along < 0.0 || Along > 6000.0)
			{
				continue;
			}
			const double Across = (Offset - Direction * Along).Size();
			if (Across > 1200.0)
			{
				continue;
			}
			const double Score = Across + Along * 0.1;
			if (Score < BestScore)
			{
				BestScore = Score;
				Best = *It;
			}
		}
		return Best;
	}

	// The train the player drives, else the one they look at.
	AFGTrain* FindTrain(UCommandSender* Sender)
	{
		AFGPlayerController* Controller = Sender ? Sender->GetPlayer() : nullptr;
		const AFGCharacterPlayer* Character = Controller ? Cast<AFGCharacterPlayer>(Controller->GetCharacter()) : nullptr;
		const AFGRailroadVehicle* Vehicle = Character ? Cast<AFGRailroadVehicle>(Character->GetDrivenVehicle()) : nullptr;
		if (!Vehicle && Controller)
		{
			Vehicle = Cast<AFGRailroadVehicle>(Controller->GetPawn());
		}
		if (!Vehicle)
		{
			Vehicle = FindAimed<AFGRailroadVehicle>(Sender);
		}
		return Vehicle ? Vehicle->GetTrain() : nullptr;
	}

	// The station the player looks at, directly or through one of its platforms.
	AFGTrainStationIdentifier* FindStation(UCommandSender* Sender)
	{
		const AFGBuildableRailroadStation* Station = FindAimed<AFGBuildableRailroadStation>(Sender);
		AFGBuildableTrainPlatformCargo* Platform = FindAimed<AFGBuildableTrainPlatformCargo>(Sender);
		AFGPlayerController* Controller = Sender ? Sender->GetPlayer() : nullptr;
		FVector Origin = FVector::ZeroVector;
		FRotator Rotation;
		if (Controller)
		{
			Controller->GetPlayerViewPoint(Origin, Rotation);
		}
		if (Platform && (!Station || FVector::DistSquared(Platform->GetActorLocation(), Origin) < FVector::DistSquared(Station->GetActorLocation(), Origin)))
		{
			Station = STLayout::FindStation(Platform);
		}
		return STLayout::GetIdentifier(Station);
	}

	AFGTrainStationIdentifier* FindStationByName(UWorld* World, const FString& Name)
	{
		AFGRailroadSubsystem* Railroad = AFGRailroadSubsystem::Get(World);
		TArray<AFGTrainStationIdentifier*> Stations;
		if (Railroad)
		{
			Railroad->GetAllTrainStations(Stations);
		}
		AFGTrainStationIdentifier* Partial = nullptr;
		int32 Partials = 0;
		for (AFGTrainStationIdentifier* Station : Stations)
		{
			const FString StationName = STLayout::Name(Station);
			if (StationName.Equals(Name, ESearchCase::IgnoreCase))
			{
				return Station;
			}
			if (StationName.Contains(Name, ESearchCase::IgnoreCase))
			{
				Partial = Station;
				++Partials;
			}
		}
		return Partials == 1 ? Partial : nullptr;
	}

	// Finds an item by its display name: an exact match, else the only one containing Name. "any" is the any-item sink.
	bool FindItem(const FString& Name, TSubclassOf<UFGItemDescriptor>& OutItem, FString& OutError)
	{
		const FString Wanted = Name.TrimStartAndEnd();
		if (Wanted.Equals(TEXT("any"), ESearchCase::IgnoreCase) || Wanted.Equals(TEXT("anything"), ESearchCase::IgnoreCase))
		{
			OutItem = nullptr;
			return true;
		}
		TArray<UClass*> Partial;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class->IsChildOf(UFGItemDescriptor::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists)
				|| Class->GetName().StartsWith(TEXT("SKEL_")) || Class->GetName().StartsWith(TEXT("REINST_")))
			{
				continue;
			}
			const FString ItemName = UFGItemDescriptor::GetItemName(Class).ToString();
			if (ItemName.IsEmpty())
			{
				continue;
			}
			if (ItemName.Equals(Wanted, ESearchCase::IgnoreCase))
			{
				OutItem = Class;
				return true;
			}
			if (ItemName.Contains(Wanted, ESearchCase::IgnoreCase))
			{
				Partial.AddUnique(Class);
			}
		}
		if (Partial.Num() == 1)
		{
			OutItem = Partial[0];
			return true;
		}
		OutError = Partial.Num() == 0 ? FString::Printf(TEXT("no item called '%s'"), *Wanted)
			: FString::Printf(TEXT("'%s' matches %d items, e.g. %s and %s"), *Wanted, Partial.Num(), *UFGItemDescriptor::GetItemName(Partial[0]).ToString(),
				*UFGItemDescriptor::GetItemName(Partial[1]).ToString());
		return false;
	}

	bool IsOn(const FString& Word)
	{
		return Word.Equals(TEXT("on"), ESearchCase::IgnoreCase) || Word.Equals(TEXT("yes"), ESearchCase::IgnoreCase) || Word.Equals(TEXT("true"), ESearchCase::IgnoreCase);
	}

	const TCHAR* Help[] = {
		TEXT("/st status | jobs | fit"),
		TEXT("/st platform [supply|demand|off] [item, item...] [target %] [sendat %] [priority low|normal|high|critical] [rate n/min] | /st platform copy"),
		TEXT("/st station [depot on|off] [same on|off|auto]   (same: editing one platform sets every platform of that kind in the station)"),
		TEXT("/st train [manage|unmanage] [keepvanilla on|off]"),
		TEXT("/st job <item> from <station> to <station>   (a test job for the train you drive or look at)"),
		TEXT("/st migrate   (opens the migration window)"),
		TEXT("/st migrate preview [tracelimit 5000] [allitems] | apply all [confirm] | apply stations 1,2 trains 3 | undo"),
		TEXT("/st testcopy [session name] | /st backup"),
	};
}

ASTChatCommand::ASTChatCommand()
{
	CommandName = TEXT("st");
	Aliases.Add(TEXT("smarttrains"));
	MinNumberOfArguments = 0;
	bOnlyUsableByPlayer = false;
	Usage = NSLOCTEXT("SmartTrains", "ChatUsage", "/st help");
}

EExecutionStatus ASTChatCommand::ExecuteCommand_Implementation(UCommandSender* Sender, const TArray<FString>& Arguments, const FString& Label)
{
	ASTSubsystem* Subsystem = ASTSubsystem::Get(this);
	UWorld* World = GetWorld();
	if (!Subsystem || !World)
	{
		Sender->SendChatMessage(TEXT("SmartTrains isn't ready in this world."), FLinearColor::Red);
		return EExecutionStatus::UNCOMPLETED;
	}
	const FString Who = SenderName(Sender);
	const FString Verb = Arguments.Num() > 0 ? Arguments[0].ToLower() : FString(TEXT("help"));
	UE_LOG(LogSmartTrains, Log, TEXT("/st %s by %s"), *FString::Join(Arguments, TEXT(" ")), *Who);

	if (Verb == TEXT("help"))
	{
		for (const TCHAR* Line : Help)
		{
			Sender->SendChatMessage(Line);
		}
		return EExecutionStatus::COMPLETED;
	}
	if (Verb == TEXT("status"))
	{
		Send(Sender, Subsystem->DescribeStatus());
		return EExecutionStatus::COMPLETED;
	}
	if (Verb == TEXT("jobs"))
	{
		Send(Sender, Subsystem->DescribeJobs());
		return EExecutionStatus::COMPLETED;
	}
	if (Verb == TEXT("fit"))
	{
		AFGTrain* Train = FindTrain(Sender);
		if (!Train)
		{
			Sender->SendChatMessage(TEXT("Drive or look at a train first."), FLinearColor::Red);
			return EExecutionStatus::UNCOMPLETED;
		}
		Send(Sender, Subsystem->DescribeTrain(Train));
		Send(Sender, Subsystem->DescribeFit(Train));
		return EExecutionStatus::COMPLETED;
	}

	if (Verb == TEXT("platform"))
	{
		AFGBuildableTrainPlatformCargo* Platform = FindAimed<AFGBuildableTrainPlatformCargo>(Sender);
		if (!Platform)
		{
			Sender->SendChatMessage(TEXT("Look at a freight or fluid platform first."), FLinearColor::Red);
			return EExecutionStatus::UNCOMPLETED;
		}
		if (Arguments.Num() >= 2 && Arguments[1].Equals(TEXT("copy"), ESearchCase::IgnoreCase))
		{
			FString Result;
			const int32 Copied = Subsystem->CopyPlatformToStation(Platform, Who + TEXT(" (copy)"), Result);
			Sender->SendChatMessage(Result, Copied < 0 ? FLinearColor::Red : FLinearColor::White);
			return Copied < 0 ? EExecutionStatus::UNCOMPLETED : EExecutionStatus::COMPLETED;
		}
		if (Arguments.Num() >= 2)
		{
			const TOptional<ESTRole> WantedRole = STNames::ParseRole(Arguments[1]);
			if (!WantedRole.IsSet())
			{
				Sender->SendChatMessage(TEXT("Role must be supply, demand or off."), FLinearColor::Red);
				return EExecutionStatus::BAD_ARGUMENTS;
			}
			const FSTPlatformConfig* Existing = Subsystem->FindPlatformConfig(Platform);
			TArray<FSTItemRule> Rules = Existing ? Existing->Items : TArray<FSTItemRule>();
			float Target = -1.f;
			float SendAt = -1.f;
			float Rate = -1.f;
			TOptional<ESTPriority> Priority;
			FString ItemText;
			for (int32 Index = 2; Index < Arguments.Num(); ++Index)
			{
				const FString Word = Arguments[Index].ToLower();
				const bool bHasValue = Index + 1 < Arguments.Num();
				if (Word == TEXT("target") && bHasValue)
				{
					Target = FCString::Atof(*Arguments[++Index].Replace(TEXT("%"), TEXT("")));
				}
				else if (Word == TEXT("sendat") && bHasValue)
				{
					SendAt = FCString::Atof(*Arguments[++Index].Replace(TEXT("%"), TEXT("")));
				}
				else if (Word == TEXT("rate") && bHasValue)
				{
					Rate = FCString::Atof(*Arguments[++Index]);
				}
				else if (Word == TEXT("priority") && bHasValue)
				{
					Priority = STNames::ParsePriority(Arguments[++Index]);
				}
				else
				{
					ItemText += (ItemText.IsEmpty() ? TEXT("") : TEXT(" ")) + Arguments[Index];
				}
			}
			if (!ItemText.IsEmpty())
			{
				Rules.Reset();
				TArray<FString> Names;
				ItemText.ParseIntoArray(Names, TEXT(","));
				for (const FString& Name : Names)
				{
					TSubclassOf<UFGItemDescriptor> Item;
					FString Error;
					if (!FindItem(Name, Item, Error))
					{
						Sender->SendChatMessage(Error, FLinearColor::Red);
						return EExecutionStatus::BAD_ARGUMENTS;
					}
					if (!Item && WantedRole.GetValue() != ESTRole::Demand)
					{
						Sender->SendChatMessage(TEXT("'any' only works on a demand platform (a sink)."), FLinearColor::Red);
						return EExecutionStatus::BAD_ARGUMENTS;
					}
					if (Item && !STLayout::ItemFitsCargoType(Item, STLayout::GetPlatformCargoType(Platform)))
					{
						Sender->SendChatMessage(FString::Printf(TEXT("%s doesn't fit this kind of platform."), *STNames::Item(Item)), FLinearColor::Red);
						return EExecutionStatus::BAD_ARGUMENTS;
					}
					FSTItemRule& Rule = Rules.AddDefaulted_GetRef();
					Rule.Item = Item;
					Rule.Priority = Item ? ESTPriority::Normal : ESTPriority::Low;
				}
			}
			if (WantedRole.GetValue() != ESTRole::Off && Rules.Num() == 0)
			{
				Sender->SendChatMessage(TEXT("Name at least one item, e.g. /st platform supply Iron Plate"), FLinearColor::Red);
				return EExecutionStatus::BAD_ARGUMENTS;
			}
			for (FSTItemRule& Rule : Rules)
			{
				Rule.TargetPercent = Target >= 0.f ? FMath::Clamp(Target, 1.f, 100.f) : Rule.TargetPercent;
				Rule.SendAtPercent = SendAt >= 0.f ? FMath::Clamp(SendAt, 0.f, 100.f) : Rule.SendAtPercent;
				Rule.ManualRatePerMinute = Rate >= 0.f ? (STNames::IsFluid(Rule.Item) ? Rate * 1000.f : Rate) : Rule.ManualRatePerMinute;
				Rule.Priority = Priority.IsSet() ? Priority.GetValue() : Rule.Priority;
			}
			Subsystem->SetPlatformConfig(Platform, WantedRole.GetValue(), WantedRole.GetValue() == ESTRole::Off ? TArray<FSTItemRule>() : Rules, Who);
		}
		Send(Sender, Subsystem->DescribePlatform(Platform));
		return EExecutionStatus::COMPLETED;
	}

	if (Verb == TEXT("station"))
	{
		AFGTrainStationIdentifier* Station = FindStation(Sender);
		if (!Station)
		{
			Sender->SendChatMessage(TEXT("Look at a station or one of its platforms first."), FLinearColor::Red);
			return EExecutionStatus::UNCOMPLETED;
		}
		for (int32 Index = 1; Index + 1 < Arguments.Num(); Index += 2)
		{
			const FString Word = Arguments[Index].ToLower();
			if (Word == TEXT("depot"))
			{
				Subsystem->SetStationDepot(Station, IsOn(Arguments[Index + 1]), Who);
			}
			else if (Word == TEXT("same"))
			{
				const FString Value = Arguments[Index + 1].ToLower();
				const ESTPlatformLink Link = Value == TEXT("auto") ? ESTPlatformLink::Auto : (IsOn(Value) ? ESTPlatformLink::Same : ESTPlatformLink::Separate);
				// The platform looked at, if any, is the one whose settings the others get.
				Subsystem->SetStationLink(Station, Link, FindAimed<AFGBuildableTrainPlatformCargo>(Sender), Who);
			}
		}
		Send(Sender, Subsystem->DescribeStation(Station));
		return EExecutionStatus::COMPLETED;
	}

	if (Verb == TEXT("train"))
	{
		AFGTrain* Train = FindTrain(Sender);
		if (!Train)
		{
			Sender->SendChatMessage(TEXT("Drive or look at a train first."), FLinearColor::Red);
			return EExecutionStatus::UNCOMPLETED;
		}
		for (int32 Index = 1; Index < Arguments.Num(); ++Index)
		{
			const FString Word = Arguments[Index].ToLower();
			FString Why;
			if (Word == TEXT("manage") || Word == TEXT("unmanage"))
			{
				if (!Subsystem->SetTrainManaged(Train, Word == TEXT("manage"), Who, Why))
				{
					Sender->SendChatMessage(FString::Printf(TEXT("Can't manage %s: %s"), *STLayout::Name(Train), *Why), FLinearColor::Red);
				}
			}
			else if (Word == TEXT("keepvanilla") && Index + 1 < Arguments.Num())
			{
				Subsystem->SetTrainKeepVanilla(Train, IsOn(Arguments[++Index]), Who);
			}
		}
		Send(Sender, Subsystem->DescribeTrain(Train));
		return EExecutionStatus::COMPLETED;
	}

	if (Verb == TEXT("job"))
	{
		const FString Rest = FString::Join(TArray<FString>(Arguments.GetData() + 1, Arguments.Num() - 1), TEXT(" "));
		FString ItemName;
		FString FromName;
		FString ToName;
		FString AfterFrom;
		if (!Rest.Split(TEXT(" from "), &ItemName, &AfterFrom, ESearchCase::IgnoreCase) || !AfterFrom.Split(TEXT(" to "), &FromName, &ToName, ESearchCase::IgnoreCase))
		{
			Sender->SendChatMessage(TEXT("Usage: /st job <item> from <station> to <station>"), FLinearColor::Red);
			return EExecutionStatus::BAD_ARGUMENTS;
		}
		AFGTrain* Train = FindTrain(Sender);
		TSubclassOf<UFGItemDescriptor> Item;
		FString Error;
		AFGTrainStationIdentifier* From = FindStationByName(World, FromName.TrimStartAndEnd());
		AFGTrainStationIdentifier* To = FindStationByName(World, ToName.TrimStartAndEnd());
		if (!Train || !FindItem(ItemName, Item, Error) || !Item || !From || !To)
		{
			Sender->SendChatMessage(FString::Printf(TEXT("Need a train you drive or look at, an item and two stations by name: train %s, item %s, from %s, to %s"),
				Train ? TEXT("ok") : TEXT("missing"), Item ? TEXT("ok") : *Error, From ? TEXT("ok") : TEXT("not found"), To ? TEXT("ok") : TEXT("not found")), FLinearColor::Red);
			return EExecutionStatus::BAD_ARGUMENTS;
		}
		FString Result;
		const bool bOk = Subsystem->AssignManualJob(Train, From, To, Item, Result);
		Sender->SendChatMessage(Result, bOk ? FLinearColor::White : FLinearColor::Red);
		return bOk ? EExecutionStatus::COMPLETED : EExecutionStatus::UNCOMPLETED;
	}

	if (Verb == TEXT("migrate"))
	{
		AFGPlayerController* Controller = Sender ? Sender->GetPlayer() : nullptr;
		USTRemoteCallObject* RCO = Controller ? Controller->GetRemoteCallObjectOfClass<USTRemoteCallObject>() : nullptr;
		// A player without a step gets the window; the server console gets the preview.
		const FString Step = Arguments.Num() > 1 ? Arguments[1].ToLower() : FString(RCO ? TEXT("window") : TEXT("preview"));
		if (Step == TEXT("window"))
		{
			if (!RCO)
			{
				Sender->SendChatMessage(TEXT("The migration window needs a player; use /st migrate preview."), FLinearColor::Red);
				return EExecutionStatus::UNCOMPLETED;
			}
			RCO->Client_OpenMigrationWindow();
		}
		else if (Step == TEXT("preview"))
		{
			Send(Sender, STMigration::Preview(Subsystem, World, STMigration::ParseTraceLimit(Arguments), STMigration::ParseOneItem(Arguments)));
		}
		else if (Step == TEXT("apply"))
		{
			Send(Sender, STMigration::Apply(Subsystem, World, TArray<FString>(Arguments.GetData() + 2, FMath::Max(0, Arguments.Num() - 2)), Who));
		}
		else if (Step == TEXT("undo"))
		{
			Send(Sender, STMigration::Undo(Subsystem, World, Who));
		}
		else
		{
			Sender->SendChatMessage(TEXT("/st migrate [window] | preview | apply ... | undo"), FLinearColor::Red);
			return EExecutionStatus::BAD_ARGUMENTS;
		}
		return EExecutionStatus::COMPLETED;
	}

	if (Verb == TEXT("testcopy") || Verb == TEXT("backup"))
	{
		if (!Subsystem->GetSettings().bBetaTools)
		{
			Sender->SendChatMessage(TEXT("Turn on 'Beta testing tools' in the Mods menu first."), FLinearColor::Red);
			return EExecutionStatus::UNCOMPLETED;
		}
		FString Message;
		const bool bOk = Verb == TEXT("backup") ? STSaveTools::Backup(World, Message)
			: STSaveTools::TestCopy(World, FString::Join(TArray<FString>(Arguments.GetData() + 1, Arguments.Num() - 1), TEXT(" ")), Message);
		Subsystem->bMadeBackupThisSession |= bOk;
		Sender->SendChatMessage(Message, bOk ? FLinearColor::White : FLinearColor::Red);
		return bOk ? EExecutionStatus::COMPLETED : EExecutionStatus::UNCOMPLETED;
	}

	PrintCommandUsage(Sender);
	return EExecutionStatus::BAD_ARGUMENTS;
}
