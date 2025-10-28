#pragma once
#include "Server-Game/Network/MessageRouter.h"

#include <Base/Container/ConcurrentQueue.h>

#include <Gameplay/GameDefine.h>

#include <Network/Define.h>

#include <memory>

#include <entt/fwd.hpp>
#include <robinhood/robinhood.h>
#include <spake2-ee/crypto_spake.h>

namespace Network
{
    class Server;
}

namespace ECS
{
    struct AccountLoginRequest
    {
    public:
        Network::SocketID socketID;
        std::string name;
    };
    struct CharacterListRequest
    {
    public:
        Network::SocketID socketID;
        entt::entity socketEntity;
    };
    struct CharacterLoginRequest
    {
    public:
        Network::SocketID socketID;
        std::string name;
    };

    namespace Singletons
    {
        struct NetworkState
        {
        public:
            std::unique_ptr<Network::Server> server;
            std::unique_ptr<Network::MessageRouter> messageRouter;

            robin_hood::unordered_set<Network::SocketID> socketIDRequestedToClose;
            robin_hood::unordered_set<Network::SocketID> activeSocketIDs;
            robin_hood::unordered_map<Network::SocketID, entt::entity> socketIDToAccountEntity;
            robin_hood::unordered_map<Network::SocketID, entt::entity> socketIDToCharacterEntity;
            robin_hood::unordered_map<Network::SocketID, crypto_spake_shared_keys> socketIDToSessionKeys;

            robin_hood::unordered_map<u64, entt::entity> accountIDToEntity;
            robin_hood::unordered_map<u64, Network::SocketID> accountIDToSocketID;
            robin_hood::unordered_map<u64, entt::entity> characterIDToEntity;
            robin_hood::unordered_map<u64, Network::SocketID> characterIDToSocketID;

            moodycamel::ConcurrentQueue<AccountLoginRequest> accountLoginRequest;
            moodycamel::ConcurrentQueue<CharacterListRequest> characterListRequest;
            moodycamel::ConcurrentQueue<CharacterLoginRequest> characterLoginRequest;
        };
    }
}