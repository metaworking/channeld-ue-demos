// Copyright Epic Games, Inc. All Rights Reserved.

#include "ChanneldIntegration.h"

#include "TestRepGameStateReplicator.h"
#include "Modules/ModuleManager.h"
#include "ChanneldUE/Replication/ChanneldReplication.h"
#include "TestRepPlayerControllerReplicator.h"

//IMPLEMENT_PRIMARY_GAME_MODULE( FDefaultGameModuleImpl, ChanneldIntegration, "ChanneldIntegration" );

void FChanneldIntegrationModule::StartupModule()
{
	REGISTER_REPLICATOR_BP(FTestRepPlayerControllerReplicator, "BlueprintGeneratedClass'/Game/Blueprints/BP_TestRepPlayerController.BP_TestRepPlayerController_C'");
	REGISTER_REPLICATOR_BP(FTestRepGameStateReplicator, "BlueprintGeneratedClass'/Game/Blueprints/BP_RepGameState.BP_RepGameState_C'");
}

void FChanneldIntegrationModule::ShutdownModule()
{

}

IMPLEMENT_PRIMARY_GAME_MODULE(FChanneldIntegrationModule, ChanneldIntegration, "ChanneldIntegration");
