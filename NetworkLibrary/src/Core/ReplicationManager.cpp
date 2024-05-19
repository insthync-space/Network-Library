#include <sstream>
#include <cassert>

#include "ReplicationManager.h"
#include "Logger.h"
#include "MessageFactory.h"

namespace NetLib
{
	std::unique_ptr<ReplicationMessage> ReplicationManager::CreateCreateReplicationMessage(uint32_t entityType, uint32_t networkEntityId, const Buffer& dataBuffer)
	{
		//Get message from message factory
		MessageFactory& messageFactory = MessageFactory::GetInstance();
		std::unique_ptr<Message> message = messageFactory.LendMessage(MessageType::Replication);

		//Set reliability and order
		message->SetOrdered(true);
		message->SetReliability(true);

		//Set specific replication message data
		std::unique_ptr<ReplicationMessage> replicationMessage(static_cast<ReplicationMessage*>(message.release()));
		replicationMessage->replicationAction = ReplicationActionType::RAT_CREATE;
		replicationMessage->networkEntityId = networkEntityId;
		replicationMessage->replicatedClassId = entityType;
		replicationMessage->data = dataBuffer.GetData();
		replicationMessage->dataSize = dataBuffer.GetSize();

		return std::move(replicationMessage);
	}

	std::unique_ptr<ReplicationMessage> ReplicationManager::CreateUpdateReplicationMessage(uint32_t networkEntityId, const Buffer& buffer)
	{
		//Get message from message factory
		MessageFactory& messageFactory = MessageFactory::GetInstance();
		std::unique_ptr<Message> message = messageFactory.LendMessage(MessageType::Replication);

		//Set reliability and order
		message->SetOrdered(true);
		message->SetReliability(false);

		//Set specific replication message data
		std::unique_ptr<ReplicationMessage> replicationMessage(static_cast<ReplicationMessage*>(message.release()));
		replicationMessage->replicationAction = ReplicationActionType::RAT_UPDATE;
		replicationMessage->networkEntityId = networkEntityId;
		replicationMessage->dataSize = buffer.GetSize();
		replicationMessage->data = buffer.GetData();

		return std::move(replicationMessage);
	}

	std::unique_ptr<ReplicationMessage> ReplicationManager::CreateDestroyReplicationMessage(uint32_t networkEntityId)
	{
		//Get message from message factory
		MessageFactory& messageFactory = MessageFactory::GetInstance();
		std::unique_ptr<Message> message = messageFactory.LendMessage(MessageType::Replication);

		//Set reliability and order
		message->SetOrdered(true);
		message->SetReliability(true);

		//Set specific replication message data
		std::unique_ptr<ReplicationMessage> replicationMessage(static_cast<ReplicationMessage*>(message.release()));
		replicationMessage->replicationAction = ReplicationActionType::RAT_DESTROY;
		replicationMessage->networkEntityId = networkEntityId;

		return std::move(replicationMessage);
	}

	void ReplicationManager::ProcessReceivedCreateReplicationMessage(const ReplicationMessage& replicationMessage)
	{
		uint32_t networkEntityId = replicationMessage.networkEntityId;
		if (_networkEntitiesStorage.HasNetworkEntityId(networkEntityId))
		{
			std::stringstream ss;
			ss << "Replication: Trying to create a network entity that is already created. Entity ID: " << static_cast<int>(networkEntityId) << ".Ignoring message...";
			Common::LOG_INFO(ss.str());
			return;
		}

		//Create network entity through its custom factory
		Buffer buffer(replicationMessage.data, replicationMessage.dataSize);
		std::stringstream ss;
		ss << "DATA SIZE: " << (int)replicationMessage.dataSize;
		Common::LOG_INFO(ss.str());
		float posX = buffer.ReadFloat();
		float posY = buffer.ReadFloat();
		int gameEntity = _networkEntityFactory->CreateNetworkEntityObject(replicationMessage.replicatedClassId, networkEntityId, posX, posY, &_networkVariableChangesHandler);
		assert(gameEntity != -1);

		//Add it to the network entities storage in order to avoid loosing it
		_networkEntitiesStorage.AddNetworkEntity(networkEntityId, gameEntity);
	}

	void ReplicationManager::ProcessReceivedUpdateReplicationMessage(const ReplicationMessage& replicationMessage)
	{
		uint32_t networkEntityId = replicationMessage.networkEntityId;
		if (!_networkEntitiesStorage.HasNetworkEntityId(networkEntityId))
		{
			std::stringstream ss;
			ss << "Replication: Trying to update a network entity that doesn't exist. Entity ID: " << static_cast<int>(networkEntityId) << ".Creating a new entity...";
			Common::LOG_INFO(ss.str());

			//If not found create a new one and update it
			int gameEntity = _networkEntityFactory->CreateNetworkEntityObject(replicationMessage.replicatedClassId, networkEntityId, 0.f, 0.f, &_networkVariableChangesHandler);
			assert(gameEntity != -1);

			//Add it to the network entities storage in order to avoid loosing it
			_networkEntitiesStorage.AddNetworkEntity(networkEntityId, gameEntity);
			return;
		}

		//TODO Pass entity state to target entity
		Buffer buffer(replicationMessage.data, replicationMessage.dataSize);
		_networkVariableChangesHandler.ProcessVariableChanges(buffer);
	}

	void ReplicationManager::ProcessReceivedDestroyReplicationMessage(const ReplicationMessage& replicationMessage)
	{
		uint32_t networkEntityId = replicationMessage.networkEntityId;
		RemoveNetworkEntity(networkEntityId);
	}

	void ReplicationManager::RegisterNetworkEntityFactory(INetworkEntityFactory* entityFactory)
	{
		_networkEntityFactory = entityFactory;
	}

	uint32_t ReplicationManager::CreateNetworkEntity(uint32_t entityType, float posX, float posY)
	{
		//Create object through its custom factory
		int gameEntityId = _networkEntityFactory->CreateNetworkEntityObject(entityType, _nextNetworkEntityId, posX, posY, &_networkVariableChangesHandler);
		assert(gameEntityId != -1);

		//Add it to the network entities storage in order to avoid loosing it
		_networkEntitiesStorage.AddNetworkEntity(_nextNetworkEntityId, gameEntityId);

		//Prepare a Create replication message for interested clients
		uint8_t* data = new uint8_t[8];
		Buffer buffer(data, 8);
		buffer.WriteFloat(posX);
		buffer.WriteFloat(posY);
		std::unique_ptr<ReplicationMessage> createMessage = CreateCreateReplicationMessage(entityType, _nextNetworkEntityId, buffer);

		//Store it into queue before broadcasting it
		_pendingReplicationActionMessages.push(std::move(createMessage));

		CalculateNextNetworkEntityId();

		return static_cast<uint32_t>(gameEntityId);
	}

	void ReplicationManager::RemoveNetworkEntity(uint32_t networkEntityId)
	{
		//Get game entity Id from network entity Id
		uint32_t gameEntity;
		bool foundSuccesfully = _networkEntitiesStorage.TryGetNetworkEntityFromId(networkEntityId, gameEntity);
		if (!foundSuccesfully)
		{
			std::stringstream ss;
			ss << "Replication: Trying to remove a network entity that doesn't exist. Network entity ID: " << static_cast<int>(networkEntityId) << ".Ignoring it...";
			Common::LOG_INFO(ss.str());
			return;
		}

		//Destroy object through its custom factory
		_networkEntityFactory->DestroyNetworkEntityObject(gameEntity);

		std::unique_ptr<ReplicationMessage> destroyMessage = CreateDestroyReplicationMessage(networkEntityId);

		//Store it into queue before broadcasting it
		_pendingReplicationActionMessages.push(std::move(destroyMessage));
	}

	void ReplicationManager::Server_ReplicateWorldState()
	{
		_networkVariableChangesHandler.CollectAllChanges();

		auto cit = _networkEntitiesStorage.GetNetworkEntities();
		auto citPastToEnd = _networkEntitiesStorage.GetPastToEndNetworkEntities();

		MessageFactory& messageFactory = MessageFactory::GetInstance();
		while (cit != citPastToEnd)
		{
			//TODO check that it has not been created with RAT_CREATE in the current tick
			//Get network variable changes data
			const EntityNetworkVariableChanges* entityNetworkVariableChanges = _networkVariableChangesHandler.GetChangesFromEntity(cit->first);
			if (entityNetworkVariableChanges != nullptr)
			{
				size_t dataSize = entityNetworkVariableChanges->Size() + sizeof(uint16_t);
				uint8_t* data = new uint8_t[dataSize];
				Buffer buffer(data, dataSize);

				//Write the number of network variables within this message
				uint16_t numberOfChanges = entityNetworkVariableChanges->floatChanges.size();
				buffer.WriteShort(numberOfChanges);

				auto changesIt = entityNetworkVariableChanges->floatChanges.cbegin();
				for (; changesIt != entityNetworkVariableChanges->floatChanges.cend(); ++changesIt)
				{
					//TODO If we store multiple networkvariables of the same entity, we dont need to include each time the entityId. Create a number of variable changes
					//in order to only include entityId once
					buffer.WriteInteger(changesIt->networkVariableId);
					buffer.WriteInteger(changesIt->networkEntityId);
					buffer.WriteFloat(changesIt->value);
				}

				std::unique_ptr<ReplicationMessage> message = CreateUpdateReplicationMessage(cit->first, buffer);
				_pendingReplicationActionMessages.push(std::move(message));
			}
			++cit;
		}

		_networkVariableChangesHandler.Clear();
	}

	void ReplicationManager::Client_ProcessReceivedReplicationMessage(const ReplicationMessage& replicationMessage)
	{
		ReplicationActionType type = static_cast<ReplicationActionType>(replicationMessage.replicationAction);
		switch (type)
		{
		case ReplicationActionType::RAT_CREATE:
			ProcessReceivedCreateReplicationMessage(replicationMessage);
			break;
		case ReplicationActionType::RAT_UPDATE:
			ProcessReceivedUpdateReplicationMessage(replicationMessage);
			break;
		case ReplicationActionType::RAT_DESTROY:
			ProcessReceivedDestroyReplicationMessage(replicationMessage);
			break;
		default:
			Common::LOG_WARNING("Invalid replication action. Skipping it...");
		}
	}

	bool ReplicationManager::ArePendingReplicationMessages() const
	{
		return !_pendingReplicationActionMessages.empty();
	}

	const ReplicationMessage* ReplicationManager::GetPendingReplicationMessage()
	{
		if (!ArePendingReplicationMessages())
		{
			return nullptr;
		}

		std::unique_ptr<ReplicationMessage> pendingReplicationMessage = std::move(_pendingReplicationActionMessages.front());
		_pendingReplicationActionMessages.pop();

		const ReplicationMessage* result = pendingReplicationMessage.get();

		_sentReplicationMessages.push(std::move(pendingReplicationMessage));

		return result;
	}

	void ReplicationManager::ClearSentReplicationMessages()
	{
		MessageFactory& messageFactory = MessageFactory::GetInstance();
		while (!_sentReplicationMessages.empty())
		{
			std::unique_ptr<ReplicationMessage> sentReplicationMessage = std::move(_sentReplicationMessages.front());
			_sentReplicationMessages.pop();

			messageFactory.ReleaseMessage(std::move(sentReplicationMessage));
		}
	}

	void ReplicationManager::CalculateNextNetworkEntityId()
	{
		++_nextNetworkEntityId;

		if (_nextNetworkEntityId == INVALID_NETWORK_ENTITY_ID)
		{
			++_nextNetworkEntityId;
		}
	}
}
