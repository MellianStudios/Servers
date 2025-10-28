#include "AccountLoginHandler.h"

#include "Server-Game/ECS/Components/AccountInfo.h"
#include "Server-Game/ECS/Components/AuthenticationInfo.h"
#include "Server-Game/ECS/Components/CharacterListInfo.h"
#include "Server-Game/ECS/Components/DisplayInfo.h"
#include "Server-Game/ECS/Components/Events.h"
#include "Server-Game/ECS/Components/NetInfo.h"
#include "Server-Game/ECS/Components/ObjectInfo.h"
#include "Server-Game/ECS/Components/PlayerContainers.h"
#include "Server-Game/ECS/Components/TargetInfo.h"
#include "Server-Game/ECS/Components/Transform.h"
#include "Server-Game/ECS/Components/UnitStatsComponent.h"
#include "Server-Game/ECS/Singletons/GameCache.h"
#include "Server-Game/ECS/Singletons/NetworkState.h"
#include "Server-Game/ECS/Singletons/WorldState.h"
#include "Server-Game/ECS/Util/MessageBuilderUtil.h"
#include "Server-Game/ECS/Util/UnitUtil.h"
#include "Server-Game/ECS/Util/Cache/CacheUtil.h"
#include "Server-Game/ECS/Util/Network/NetworkUtil.h"

#include <Server-Common/Database/DBController.h>
#include <Server-Common/Database/Util/AccountUtils.h>
#include <Server-Common/Database/Util/CharacterUtils.h>

#include <Base/Util/DebugHandler.h>
#include <Base/Util/StringUtils.h>

#include <Meta/Generated/Shared/NetworkPacket.h>
#include <Meta/Generated/Shared/PacketList.h>

#include <Network/Server.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <spake2-ee/crypto_spake.h>

namespace ECS::Systems
{
    void AccountLoginHandler::Init(entt::registry& registry)
    {
    }

    void AccountLoginHandler::Update(entt::registry& registry, f32 deltaTime)
    {
        entt::registry::context& ctx = registry.ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();
        auto& worldState = ctx.get<Singletons::WorldState>();
        auto& networkState = ctx.get<Singletons::NetworkState>();

        auto databaseConn = gameCache.dbController->GetConnection(::Database::DBType::Character);

        AccountLoginRequest request;
        while (networkState.accountLoginRequest.try_dequeue(request))
        {
            Network::SocketID socketID = request.socketID;

            pqxx::result databaseResult;
            if (Database::Util::Account::AccountGetInfoByName(databaseConn, request.name, databaseResult))
            {
                auto accountID = databaseResult[0][0].as<u64>();

                Network::SocketID linkedSocketID;
                bool isAccountLoggedIn = Util::Network::GetSocketIDFromAccountID(networkState, accountID, linkedSocketID);

                // TODO : Implement disconnecting previous session if already logged in
                if (!isAccountLoggedIn)
                {
                    auto flags = databaseResult[0][1].as<u64>();

                    std::string raw = databaseResult[0][6].as<std::string>();
                    pqxx::bytes blob = databaseConn->connection->unesc_bin(raw);
                    if (blob.size() != crypto_spake_STOREDBYTES)
                    {
                        Util::Network::SendPacket(networkState, socketID, Generated::ConnectResultPacket{
                            .result = static_cast<u8>(Network::ConnectResult::Failed)
                        });

                        networkState.server->CloseSocketID(socketID);
                        continue;
                    }

                    entt::entity accountEntity = registry.create();
                    Util::Network::LinkSocketToAccount(networkState, socketID, accountID, accountEntity);

                    auto& accountInfo = registry.emplace<Components::AccountInfo>(accountEntity);
                    accountInfo.id = accountID;
                    accountInfo.name = request.name;
                    accountInfo.email = databaseResult[0][3].as<std::string>();
                    accountInfo.flags = flags;
                    accountInfo.creationTimestamp = databaseResult[0][4].as<u64>();
                    accountInfo.lastLoginTimestamp = databaseResult[0][5].as<u64>();

                    auto& authInfo = registry.emplace<Components::AuthenticationInfo>(accountEntity);
                    authInfo.state = AuthenticationState::Step1;
                    memcpy(authInfo.blob, blob.data(), crypto_spake_STOREDBYTES);

                    unsigned char public_data[crypto_spake_PUBLICDATABYTES];
                    i32 result = crypto_spake_step0(&authInfo.serverState, public_data, authInfo.blob);
                    if (result != 0)
                    {
                        Util::Network::SendPacket(networkState, socketID, Generated::ConnectResultPacket{
                            .result = static_cast<u8>(Network::ConnectResult::Failed)
                            });

                        networkState.server->CloseSocketID(socketID);
                        continue;
                    }

                    // Send Public Data Packet
                    {
                        Generated::ServerAuthChallengePacket challenge;
                        memcpy(challenge.challenge.data(), public_data, crypto_spake_PUBLICDATABYTES);

                        Util::Network::SendPacket(networkState, socketID, challenge);
                    }

                    continue;
                }
            }

            Util::Network::SendPacket(networkState, socketID, Generated::ConnectResultPacket{
                .result = static_cast<u8>(Network::ConnectResult::Failed)
            });

            networkState.server->CloseSocketID(socketID);
        }

        CharacterListRequest charListRequest;
        while (networkState.characterListRequest.try_dequeue(charListRequest))
        {
            Network::SocketID socketID = charListRequest.socketID;
            entt::entity entity = charListRequest.socketEntity;
            if (!Util::Network::IsSocketActive(networkState, socketID) || !registry.valid(entity))
                continue;

            auto& accountInfo = registry.get<Components::AccountInfo>(entity);
            auto& characterListInfo = registry.get_or_emplace<Components::CharacterListInfo>(entity);
            characterListInfo.list.clear();

            std::shared_ptr<Bytebuffer> buffer = Bytebuffer::Borrow<1024>();
            bool result = Util::MessageBuilder::CreatePacket(buffer, (Network::OpcodeType)Generated::PacketListEnum::ServerCharacterList, [&networkState, &databaseConn, &accountInfo, &characterListInfo, &buffer]()
            {
                pqxx::result databaseResult;
                if (Database::Util::Character::CharacterGetInfosByAccount(databaseConn, accountInfo.id, databaseResult))
                {
                    u32 numCharacters = static_cast<u32>(databaseResult.size());
                    characterListInfo.list.reserve(numCharacters);

                    buffer->PutU8(static_cast<u8>(numCharacters));

                    for (const auto& row : databaseResult)
                    {
                        CharacterListEntry& entry = characterListInfo.list.emplace_back();
                        entry.name = row[2].as<std::string>();
                        entry.level = row[8].as<u16>();
                        entry.mapID = row[10].as<u32>();

                        u16 raceGenderClass = row[7].as<u16>();
                        u8 race = raceGenderClass & 0x7F;
                        u8 gender = (raceGenderClass >> 7) & 0x3;
                        u8 unitClass = (raceGenderClass >> 9) & 0x1FF;

                        entry.race = race;
                        entry.gender = gender;
                        entry.unitClass = unitClass;

                        buffer->PutString(entry.name);
                        buffer->PutU8(entry.race);
                        buffer->PutU8(entry.gender);
                        buffer->PutU8(entry.unitClass);
                        buffer->PutU16(entry.level);
                        buffer->PutU16(entry.mapID);
                    }
                }
                else
                {
                    // No character info found, send 0 characters
                    buffer->PutU8(0);
                }
            });

            if (!result)
            {
                networkState.server->CloseSocketID(socketID);
                continue;
            }

            Util::Network::SendPacket(networkState, socketID, buffer);
        }
    }
}