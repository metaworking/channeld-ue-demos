// Copyright Epic Games, Inc. All Rights Reserved.


#include "ChanneldIntegrationGameModeBase.h"

#include "ServerAuthPlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetStringLibrary.h"

void AChanneldIntegrationGameModeBase::PreLogin(const FString& Options, const FString& Address,
	const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
	Super::PreLogin(Options, Address, UniqueId, ErrorMessage);
	if (ErrorMessage.IsEmpty())
	{
		// Check if it is a valid connid
		int32 ConnId = UKismetStringLibrary::Conv_StringToInt(UGameplayStatics::ParseOption(Options, TEXT("connid")).TrimStartAndEnd());
	}
}

FString AChanneldIntegrationGameModeBase::InitNewPlayer(APlayerController* NewPlayerController,
	const FUniqueNetIdRepl& UniqueId, const FString& Options, const FString& Portal)
{
	FString ErrorMessage = Super::InitNewPlayer(NewPlayerController, UniqueId, Options, Portal);
	if (GetWorld()->GetNetMode() == NM_DedicatedServer)
	{
		if (ErrorMessage.IsEmpty())
		{
			AServerAuthPlayerController* ServerAuthPlayerController = Cast<AServerAuthPlayerController>(NewPlayerController);
			if (ServerAuthPlayerController != nullptr)
			{
				int32 ConnId = UKismetStringLibrary::Conv_StringToInt(UGameplayStatics::ParseOption(Options, TEXT("connid")).TrimStartAndEnd());
				FString Alias = UGameplayStatics::ParseOption(Options, TEXT("alias")).TrimStartAndEnd();
				ServerAuthPlayerController->ChanneldConnid = ConnId;
				ServerAuthPlayerController->PlayerAlias = Alias;
			}
		}
	}

	return ErrorMessage;
}
