﻿// Fill out your copyright notice in the Description page of Project Settings.


#include "ChanneldNetDriver.h"

#include "Net/DataReplication.h"
#include "Net/RepLayout.h"
#include "Misc/ScopeExit.h"
#include "google/protobuf/message_lite.h"
#include "Engine/NetConnection.h"
#include "PacketHandler.h"
#include "Net/Core/Misc/PacketAudit.h"
#include "ChanneldGameInstanceSubsystem.h"
#include "ChanneldWorldSettings.h"

UChanneldNetDriver::UChanneldNetDriver(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

UChanneldNetConnection* UChanneldNetDriver::OnClientConnected(ConnectionId ClientConnId)
{
	auto ClientConnection = NewObject<UChanneldNetConnection>(GetTransientPackage(), NetConnectionClass);
	ClientConnection->bDisableHandshaking = bDisableHandshaking;
	// Server always sees a connected client (forwarded from channeld) as authenticated.
	ClientConnection->bChanneldAuthenticated = true;
	ClientConnection->InitRemoteConnection(this, GetSocket(), InitBaseURL, ConnIdToAddr(ClientConnId).Get(), EConnectionState::USOCK_Open);

	Notify->NotifyAcceptedConnection(ClientConnection);
	AddClientConnection(ClientConnection);

	ClientConnectionMap.Add(ClientConnId, ClientConnection);

	if (!bDisableHandshaking && ConnectionlessHandler.IsValid() && StatelessConnectComponent.IsValid())
	{
		ClientConnection->bInConnectionlessHandshake = true;
	}
	return ClientConnection;
}

ConnectionId UChanneldNetDriver::AddrToConnId(const FInternetAddr& Addr)
{
	uint32 ConnId;
	Addr.GetIp(ConnId);
	return ConnId;
}

TSharedRef<FInternetAddr> UChanneldNetDriver::ConnIdToAddr(ConnectionId ConnId)
{
	auto AddrPtr = CachedAddr.Find(ConnId);
	if (AddrPtr == nullptr)
	{
		auto Addr = GetSocketSubsystem()->CreateInternetAddr();
		Addr->SetIp(ConnId);
		CachedAddr.Add(ConnId, Addr);
		AddrPtr = &Addr;
	}
	return *AddrPtr;
}

void UChanneldNetDriver::PostInitProperties()
{
	Super::PostInitProperties();
}

bool UChanneldNetDriver::IsAvailable() const
{
	return true;
}

bool UChanneldNetDriver::InitConnectionClass()
{
	NetConnectionClass = UChanneldNetConnection::StaticClass();
	return true;
}

bool UChanneldNetDriver::InitBase(bool bInitAsClient, FNetworkNotify* InNotify, const FURL& URL,
	bool bReuseAddressAndPort, FString& Error)
{
	UWorld* TheWorld = GetWorld();
	if (!TheWorld)
	{
		TheWorld = GEngine->GetWorldContextFromPendingNetGameNetDriver(this)->World();
	}
	auto GameInstance = TheWorld->GetGameInstance();
	if (GameInstance)
	{
		auto Subsystem = GameInstance->GetSubsystem<UChanneldGameInstanceSubsystem>();
		if (Subsystem)
		{
			// Share the same ChanneldConnection with the subsystem if it exists.
			// In the standalone net game, NetDriver::InitBase is called when starting client travel, so the connection in the subsystem should be already created.
			ConnToChanneld = Subsystem->GetConnection();

			LowLevelSendToChannelId = Subsystem->LowLevelSendToChannelId;
		}
	}
	if (ConnToChanneld == nullptr)
	{
		ConnToChanneld = NewObject<UChanneldConnection>();
	}

	ConnToChanneld->OnUserSpaceMessageReceived.AddLambda([&](ChannelId ChId, ConnectionId ClientConnId, const std::string& Payload)
		{
			if (ConnToChanneld->IsClient())
			{
				const auto MyServerConnection = GetServerConnection();
				if (MyServerConnection)
				{
					MyServerConnection->ReceivedRawPacket((uint8*)Payload.data(), Payload.size());
				}
				else
				{
					UE_LOG(LogChanneld, Error, TEXT("ServerConnection doesn't exist"));
				}
			}
			else
			{
				auto ClientConnection = ClientConnectionMap.FindRef(ClientConnId);
				// Server's ClientConnection is created when the first packet from client arrives.
				if (ClientConnection == nullptr)
				{
					ClientConnection = OnClientConnected(ClientConnId);
				}
				ClientConnection->ReceivedRawPacket((uint8*)Payload.data(), Payload.size());
			}
		});

	InitBaseURL = URL;

	if (!ConnToChanneld->IsConnected())
	{
		ConnToChanneld->OnAuthenticated.AddUObject(this, &UChanneldNetDriver::OnChanneldAuthenticated);

		FString Host;
		int Port;
		if (bInitAsClient)
		{
			Host = ChanneldIpForClient;
			Port = ChanneldPortForClient;
		}
		else
		{
			Host = ChanneldIpForServer;
			Port = ChanneldPortForServer;
		}

		if (ConnToChanneld->Connect(bInitAsClient, Host, Port, Error))
		{
			ConnToChanneld->Auth(TEXT("test_pit"), TEXT("test_lt"));
		}
		else
		{
			Error = TEXT("Failed to connect to channeld");
			return false;
		}
	}

	return UNetDriver::InitBase(bInitAsClient, InNotify, URL, bReuseAddressAndPort, Error);
}

bool UChanneldNetDriver::InitConnect(FNetworkNotify* InNotify, const FURL& ConnectURL, FString& Error)
{
	ISocketSubsystem* SocketSubsystem = GetSocketSubsystem();
	if (SocketSubsystem == nullptr)
	{
		UE_LOG(LogNet, Warning, TEXT("Unable to find socket subsystem"));
		return false;
	}

	if (!InitBase(true, InNotify, ConnectURL, false, Error))
	{
		UE_LOG(LogNet, Warning, TEXT("Failed to init net driver ConnectURL: %s: %s"), *ConnectURL.ToString(), *Error);
		return false;
	}

	/* Driver->ConnectionlessHandler is only used in server
	if (!bDisableHandshaking)
	{
		InitConnectionlessHandler();
	}
	*/

	// Create new connection.
	ServerConnection = NewObject<UNetConnection>(GetTransientPackage(), NetConnectionClass);
	UChanneldNetConnection* NetConnection = CastChecked<UChanneldNetConnection>(ServerConnection);

	if (NetConnection == nullptr)
	{
		Error = TEXT("Could not cast the ServerConnection into the base connection class for this netdriver!");
		return false;
	}

	NetConnection->bDisableHandshaking = bDisableHandshaking;
	ServerConnection->InitLocalConnection(this, GetSocket(), ConnectURL, USOCK_Open);
	//NetConnection->bInConnectionlessHandshake = true;

	UE_LOG(LogNet, Log, TEXT("Game client on port %i, rate %i"), ConnectURL.Port, ServerConnection->CurrentNetSpeed);
	CreateInitialClientChannels();

	if (ConnToChanneld->IsAuthenticated())
	{
		// Connection is already authenticated via the subsystem
		OnChanneldAuthenticated(ConnToChanneld);
	}

	return true;
}

bool UChanneldNetDriver::InitListen(FNetworkNotify* InNotify, FURL& LocalURL, bool bReuseAddressAndPort, FString& Error)
{

	if (!InitBase(false, InNotify, LocalURL, bReuseAddressAndPort, Error))
	{
		UE_LOG(LogNet, Warning, TEXT("Failed to init net driver ListenURL: %s: %s"), *LocalURL.ToString(), *Error);
		return false;
	}

	if (!bDisableHandshaking)
	{
		InitConnectionlessHandler();
	}
	return true;

	//return Super::InitListen(InNotify, LocalURL, bReuseAddressAndPort, Error);
}

ISocketSubsystem* UChanneldNetDriver::GetSocketSubsystem()
{
	return ISocketSubsystem::Get();
}

FSocket* UChanneldNetDriver::GetSocket()
{
	//return Super::GetSocket();
	// Return the Socket to channeld
	return ConnToChanneld->GetSocket();

	// SetSocket can't be overridden
}

void UChanneldNetDriver::LowLevelSend(TSharedPtr<const FInternetAddr> Address, void* Data, int32 CountBits,
	FOutPacketTraits& Traits)
{
	//Super::LowLevelSend(Address, Data, CountBits, Traits);
	
	if (ConnToChanneld->IsConnected() && Address.IsValid() && Address->IsValid())
	{
		// Copied from UIpNetDriver::LowLevelSend
		uint8* DataToSend = reinterpret_cast<uint8*>(Data);
		int32 DataSize = FMath::DivideAndRoundUp(CountBits, 8);

		FPacketAudit::NotifyLowLevelReceive(DataToSend, DataSize);

		if (!bDisableHandshaking && ConnectionlessHandler.IsValid())
		{
			const ProcessedPacket ProcessedData =
				ConnectionlessHandler->OutgoingConnectionless(Address, (uint8*)DataToSend, CountBits, Traits);

			if (!ProcessedData.bError)
			{
				DataToSend = ProcessedData.Data;
				DataSize = FMath::DivideAndRoundUp(ProcessedData.CountBits, 8);
			}
			else
			{
				return;
			}
		}

		if (ConnToChanneld->IsServer())
		{
			ConnectionId ClientConnId = AddrToConnId(*Address);
			channeldpb::ServerForwardMessage ServerForwardMessage;
			ServerForwardMessage.set_clientconnid(ClientConnId);
			ServerForwardMessage.set_payload(DataToSend, DataSize);
			CLOCK_CYCLES(SendCycles);
			ConnToChanneld->Send(LowLevelSendToChannelId.Get(), channeldpb::USER_SPACE_START, ServerForwardMessage, channeldpb::SINGLE_CONNECTION);
			UNCLOCK_CYCLES(SendCycles);
		}
		else
		{
			CLOCK_CYCLES(SendCycles);
			ConnToChanneld->SendRaw(LowLevelSendToChannelId.Get(), channeldpb::USER_SPACE_START, DataToSend, DataSize);
			UNCLOCK_CYCLES(SendCycles);
		}
	}
	
}

void UChanneldNetDriver::LowLevelDestroy()
{
	if (ChannelDataView)
	{
		ChannelDataView->Unintialize();
		ChannelDataView = NULL;
	}

	Super::LowLevelDestroy();

	if (ConnToChanneld)
	{
		ConnToChanneld->Disconnect(true);
	}
	
	ClientConnectionMap.Reset();
	
	ConnToChanneld = NULL;
}

bool UChanneldNetDriver::IsNetResourceValid()
{
	if ((ConnToChanneld->IsServer() && !ServerConnection)//  Server
		|| (ConnToChanneld->IsClient() && ServerConnection) // client
		)
	{
		return true;
	}

	return false;
}

int32 UChanneldNetDriver::ServerReplicateActors(float DeltaSeconds)
{
	auto const Result = Super::ServerReplicateActors(DeltaSeconds);
	return Result;
}


void UChanneldNetDriver::TickDispatch(float DeltaTime)
{
	//Super::TickDispatch(DeltaTime);
	UNetDriver::TickDispatch(DeltaTime);

	if (IsValid(ConnToChanneld) && ConnToChanneld->IsConnected())
		ConnToChanneld->TickIncoming();
}

void UChanneldNetDriver::TickFlush(float DeltaSeconds)
{
	// Trigger the callings of LowLevelSend()
	UNetDriver::TickFlush(DeltaSeconds);

	if (IsValid(ConnToChanneld) && ConnToChanneld->IsConnected())
	{
		if (ChannelDataView)
		{
			ChannelDataView->SendAllChannelUpdates();
		}
		ConnToChanneld->TickOutgoing();
	}
}

void UChanneldNetDriver::OnChanneldAuthenticated(UChanneldConnection* _)
{
	if (ConnToChanneld->IsServer())
	{
		/* Moved to ChannelDataView
		ConnToChanneld->CreateChannel(channeldpb::GLOBAL, TEXT("test123"), nullptr, nullptr, nullptr,
			[&](const channeldpb::CreateChannelResultMessage* ResultMsg)
			{
				UE_LOG(LogChanneld, Log, TEXT("[%s] Created channel: %d, type: %s, owner connId: %d, metadata: %s"), 
					*GetWorld()->GetDebugDisplayName(),
					ResultMsg->channelid(), 
					UTF8_TO_TCHAR(channeldpb::ChannelType_Name(ResultMsg->channeltype()).c_str()), 
					ResultMsg->ownerconnid(), 
					UTF8_TO_TCHAR(ResultMsg->metadata().c_str()));

				for (auto const Provider : ChannelDataProviders)
				{
					if (Provider->GetChannelType() == ResultMsg->channeltype())
					{
						Provider->SetChannelId(ResultMsg->channelid());
					}
				}

				//FlushUnauthData();
			});
		*/
	}
	else
	{
		auto MyServerConnection = GetServerConnection();
		MyServerConnection->RemoteAddr = ConnIdToAddr(ConnToChanneld->GetConnId());
		MyServerConnection->bChanneldAuthenticated = true;
		MyServerConnection->FlushUnauthData();

		/* Moved to ChannelDataView
		ChannelId ChIdToSub = GlobalChannelId;
		ConnToChanneld->SubToChannel(ChIdToSub, nullptr, [&, ChIdToSub, MyServerConnection](const channeldpb::SubscribedToChannelResultMessage* Msg)
			{
				UE_LOG(LogChanneld, Log, TEXT("[%s] Sub to channel: %d, connId: %d"), *GetWorld()->GetDebugDisplayName(), ChIdToSub, Msg->connid());
				MyServerConnection->bChanneldAuthenticated = true;
				MyServerConnection->FlushUnauthData();
			});
		*/
	}

	InitChannelDataView();
}

void UChanneldNetDriver::InitChannelDataView()
{
	UClass* ChannelDataViewClass = nullptr;

	UWorld* TheWorld = GetWorld();
	if (!TheWorld)
	{
		TheWorld = GEngine->GetWorldContextFromPendingNetGameNetDriver(this)->World();
	}
	// Use the class in the WorldSettings first
	auto WorldSettings = Cast<AChanneldWorldSettings>(TheWorld->GetWorldSettings());
	if (WorldSettings)
	{
		ChannelDataViewClass = WorldSettings->ChannelDataViewClass;
	}

	// If not exist, use the class name in ChanneldUE.ini
	if (!ChannelDataViewClass && ChannelDataViewClassName != TEXT(""))
	{
		if (ChannelDataViewClassName.StartsWith("Blueprint'"))
		{
			auto BpClass = TSoftClassPtr<UChannelDataView>(FSoftObjectPath(ChannelDataViewClassName));
			ChannelDataViewClass = BpClass.LoadSynchronous();
		}
		else
		{
			ChannelDataViewClass = LoadClass<UChannelDataView>(this, *ChannelDataViewClassName, NULL, LOAD_None, NULL);
		}

		if (ChannelDataViewClass == NULL)
		{
			UE_LOG(LogChanneld, Error, TEXT("Failed to load class '%s'"), *ChannelDataViewClassName);
		}
	}

	if (ChannelDataViewClass)
	{
		ChannelDataView = NewObject<UChannelDataView>(this, ChannelDataViewClass);
		//ConnToChanneld->OnAuthenticated.AddUObject(ChannelDataView, &UChannelDataView::Initialize);
		ChannelDataView->Initialize(ConnToChanneld);
	}
	else
	{
		UE_LOG(LogChanneld, Warning, TEXT("Failed to load any ChannelDataView"));
	}
}


