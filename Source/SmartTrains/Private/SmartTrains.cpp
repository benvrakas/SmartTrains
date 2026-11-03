#include "SmartTrains.h"

DEFINE_LOG_CATEGORY(LogSmartTrains);

void FSmartTrainsModule::StartupModule()
{
	UE_LOG(LogSmartTrains, Log, TEXT("SmartTrains module starting"));
}

void FSmartTrainsModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FSmartTrainsModule, SmartTrains)
