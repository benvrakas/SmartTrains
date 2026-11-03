#pragma once

#include "CoreMinimal.h"
#include "Command/ChatCommandInstance.h"
#include "STChatCommand.generated.h"

// /st: platform roles, managed trains, readouts, migration and save copies (Docs/Commands.md).
UCLASS()
class SMARTTRAINS_API ASTChatCommand : public AChatCommandInstance
{
	GENERATED_BODY()

public:
	ASTChatCommand();

	virtual EExecutionStatus ExecuteCommand_Implementation(UCommandSender* Sender, const TArray<FString>& Arguments, const FString& Label) override;
};
