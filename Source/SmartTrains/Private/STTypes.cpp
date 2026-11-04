#include "STTypes.h"
#include "Resources/FGItemDescriptor.h"

const TCHAR* STNames::Role(ESTRole Role)
{
	switch (Role)
	{
	case ESTRole::Supply: return TEXT("Supply");
	case ESTRole::Demand: return TEXT("Demand");
	default: return TEXT("Off");
	}
}

const TCHAR* STNames::Priority(ESTPriority Priority)
{
	switch (Priority)
	{
	case ESTPriority::Low: return TEXT("Low");
	case ESTPriority::High: return TEXT("High");
	case ESTPriority::Critical: return TEXT("Critical");
	default: return TEXT("Normal");
	}
}

const TCHAR* STNames::Phase(ESTTrainPhase Phase)
{
	switch (Phase)
	{
	case ESTTrainPhase::Working: return TEXT("working");
	case ESTTrainPhase::Parked: return TEXT("parked");
	default: return TEXT("idle");
	}
}

TOptional<ESTRole> STNames::ParseRole(const FString& Text)
{
	if (Text.Equals(TEXT("supply"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("source"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("in"), ESearchCase::IgnoreCase))
	{
		return ESTRole::Supply;
	}
	if (Text.Equals(TEXT("demand"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("sink"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("out"), ESearchCase::IgnoreCase))
	{
		return ESTRole::Demand;
	}
	if (Text.Equals(TEXT("off"), ESearchCase::IgnoreCase))
	{
		return ESTRole::Off;
	}
	return {};
}

TOptional<ESTPriority> STNames::ParsePriority(const FString& Text)
{
	for (ESTPriority Priority : {ESTPriority::Low, ESTPriority::Normal, ESTPriority::High, ESTPriority::Critical})
	{
		if (Text.Equals(STNames::Priority(Priority), ESearchCase::IgnoreCase))
		{
			return Priority;
		}
	}
	return {};
}

float STNames::PriorityWeight(ESTPriority Priority)
{
	switch (Priority)
	{
	case ESTPriority::Low: return 0.5f;
	case ESTPriority::High: return 2.f;
	case ESTPriority::Critical: return 8.f;
	default: return 1.f;
	}
}

FString STNames::Item(TSubclassOf<UFGItemDescriptor> Item)
{
	return Item ? UFGItemDescriptor::GetItemName(Item).ToString() : FString(TEXT("any item"));
}

bool STNames::IsFluid(TSubclassOf<UFGItemDescriptor> Item)
{
	if (!Item)
	{
		return false;
	}
	const EResourceForm Form = UFGItemDescriptor::GetForm(Item);
	return Form == EResourceForm::RF_LIQUID || Form == EResourceForm::RF_GAS;
}

FString STNames::Amount(TSubclassOf<UFGItemDescriptor> Item, double Amount)
{
	if (IsFluid(Item))
	{
		return FString::Printf(TEXT("%.1f m3"), Amount / 1000.0);
	}
	return FString::Printf(TEXT("%.0f"), Amount);
}

const FSTItemRule* FSTPlatformConfig::FindRule(TSubclassOf<UFGItemDescriptor> Item) const
{
	const FSTItemRule* Any = nullptr;
	for (const FSTItemRule& Rule : Items)
	{
		if (Rule.Item == Item && Item)
		{
			return &Rule;
		}
		if (!Rule.Item && Role == ESTRole::Demand)
		{
			Any = &Rule;
		}
	}
	return Any;
}

FString STNames::Settings(ESTRole InRole, const TArray<FSTItemRule>& InItems)
{
	if (InRole == ESTRole::Off)
	{
		return TEXT("Off");
	}
	TArray<FString> Names;
	for (const FSTItemRule& Rule : InItems)
	{
		Names.Add(Item(Rule.Item));
	}
	return FString::Printf(TEXT("%s: %s"), Role(InRole), Names.Num() > 0 ? *FString::Join(Names, TEXT(", ")) : TEXT("no items"));
}
