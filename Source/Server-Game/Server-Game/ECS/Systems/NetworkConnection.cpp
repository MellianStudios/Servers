#include "NetworkConnection.h"

#include "Server-Game/Application/EnttRegistries.h"
#include "Server-Game/ECS/Components/AABB.h"
#include "Server-Game/ECS/Components/AccountInfo.h"
#include "Server-Game/ECS/Components/AuthenticationInfo.h"
#include "Server-Game/ECS/Components/CharacterInfo.h"
#include "Server-Game/ECS/Components/CharacterListInfo.h"
#include "Server-Game/ECS/Components/CharacterSpellCastInfo.h"
#include "Server-Game/ECS/Components/CreatureAIInfo.h"
#include "Server-Game/ECS/Components/CreatureInfo.h"
#include "Server-Game/ECS/Components/DisplayInfo.h"
#include "Server-Game/ECS/Components/Events.h"
#include "Server-Game/ECS/Components/NetInfo.h"
#include "Server-Game/ECS/Components/ObjectInfo.h"
#include "Server-Game/ECS/Components/PlayerContainers.h"
#include "Server-Game/ECS/Components/ProximityTrigger.h"
#include "Server-Game/ECS/Components/SpellEffectInfo.h"
#include "Server-Game/ECS/Components/SpellInfo.h"
#include "Server-Game/ECS/Components/Tags.h"
#include "Server-Game/ECS/Components/TargetInfo.h"
#include "Server-Game/ECS/Components/Transform.h"
#include "Server-Game/ECS/Components/UnitSpellCooldownHistory.h"
#include "Server-Game/ECS/Components/UnitStatsComponent.h"
#include "Server-Game/ECS/Components/UnitVisualItems.h"
#include "Server-Game/ECS/Components/VisibilityInfo.h"
#include "Server-Game/ECS/Components/VisibilityUpdateInfo.h"
#include "Server-Game/ECS/Singletons/CombatEventState.h"
#include "Server-Game/ECS/Singletons/CreatureAIState.h"
#include "Server-Game/ECS/Singletons/GameCache.h"
#include "Server-Game/ECS/Singletons/NetworkState.h"
#include "Server-Game/ECS/Singletons/ProximityTriggers.h"
#include "Server-Game/ECS/Singletons/TimeState.h"
#include "Server-Game/ECS/Singletons/WorldState.h"
#include "Server-Game/ECS/Util/CollisionUtil.h"
#include "Server-Game/ECS/Util/CombatEventUtil.h"
#include "Server-Game/ECS/Util/ContainerUtil.h"
#include "Server-Game/ECS/Util/MessageBuilderUtil.h"
#include "Server-Game/ECS/Util/ProximityTriggerUtil.h"
#include "Server-Game/ECS/Util/UnitUtil.h"
#include "Server-Game/ECS/Util/Cache/CacheUtil.h"
#include "Server-Game/ECS/Util/Network/NetworkUtil.h"
#include "Server-Game/ECS/Util/Persistence/CharacterUtil.h"
#include "Server-Game/ECS/Util/Persistence/ItemUtil.h"
#include "Server-Game/Scripting/Util/NetworkUtil.h"
#include "Server-Game/Util/ServiceLocator.h"

#include <Server-Common/Database/DBController.h>
#include <Server-Common/Database/Util/CharacterUtils.h>
#include <Server-Common/Database/Util/ItemUtils.h>
#include <Server-Common/Database/Util/MapUtils.h>
#include <Server-Common/Database/Util/ProximityTriggerUtils.h>

#include <Base/Util/DebugHandler.h>
#include <Base/Util/StringUtils.h>

#include <FileFormat/Shared.h>

#include <Gameplay/GameDefine.h>
#include <Gameplay/ECS/Components/UnitFields.h>
#include <Gameplay/Network/GameMessageRouter.h>

#include <MetaGen/PacketList.h>
#include <MetaGen/Shared/ClientDB/ClientDB.h>
#include <MetaGen/Shared/Packet/Packet.h>
#include <MetaGen/Shared/Unit/Unit.h>

#include <Network/Server.h>

#include <Scripting/LuaManager.h>

#include <entt/entt.hpp>

#include <chrono>
#include <format>

namespace ECS::Systems
{
    bool HandleOnCheatCharacterAdd(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        struct Result
        {
            u8 NameIsInvalid : 1 = 0;
            u8 CharacterAlreadyExists : 1 = 0;
            u8 DatabaseTransactionFailed : 1 = 0;
            u8 Unused : 5 = 0;
        };

        std::string charName = "";
        if (!message.buffer->GetString(charName))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();
        auto& networkState = ctx.get<Singletons::NetworkState>();

        Result result = { 0 };
        u32 charNameHash = StringUtils::fnv1a_32(charName.c_str(), charName.length());
        if (!StringUtils::StringIsAlphaAndAtLeastLength(charName, 2))
        {
            result.NameIsInvalid = 1;
        }
        else if (Util::Cache::CharacterExistsByNameHash(gameCache, charNameHash))
        {
            result.CharacterAlreadyExists = 1;
        }

        u8 resultAsValue = *reinterpret_cast<u8*>(&result);
        if (resultAsValue != 0)
        {
            Util::Network::SendPacket(networkState, socketID, MetaGen::Shared::Packet::ServerCheatCommandResultPacket{
                .command = static_cast<u8>(MetaGen::Shared::Cheat::CheatCommandEnum::CharacterAdd),
                .result = resultAsValue,
                .response = "Unknown"
                });
            return true;
        }

        u64 characterID;
        ECS::Result creationResult = Util::Persistence::Character::CharacterCreate(gameCache, charName, 1, characterID);

        Result cheatCommandResult =
        {
            .NameIsInvalid = 0,
            .CharacterAlreadyExists = creationResult == ECS::Result::CharacterAlreadyExists,
            .DatabaseTransactionFailed = creationResult == ECS::Result::DatabaseError,
            .Unused = 0
        };

        u8 cheatCommandResultVal = *reinterpret_cast<u8*>(&cheatCommandResult);

        Util::Network::SendPacket(networkState, socketID, MetaGen::Shared::Packet::ServerCheatCommandResultPacket{
            .command = static_cast<u8>(MetaGen::Shared::Cheat::CheatCommandEnum::CharacterAdd),
            .result = cheatCommandResultVal,
            .response = "Unknown"
            });
        return true;
    }
    bool HandleOnCheatCharacterRemove(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        struct Result
        {
            u8 CharacterDoesNotExist : 1 = 0;
            u8 DatabaseTransactionFailed : 1 = 0;
            u8 InsufficientPermission : 1 = 0;
            u8 Unused : 5 = 0;
        };

        std::string charName = "";
        if (!message.buffer->GetString(charName))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();
        auto& networkState = ctx.get<Singletons::NetworkState>();

        Result result = { 0 };
        u32 charNameHash = StringUtils::fnv1a_32(charName.c_str(), charName.length());

        u64 characterIDToDelete;
        bool characterExists = Util::Cache::GetCharacterIDByNameHash(gameCache, charNameHash, characterIDToDelete);

        if (!characterExists || !StringUtils::StringIsAlphaAndAtLeastLength(charName, 2))
        {
            // Send back that the character does not exist
            result.CharacterDoesNotExist = 1;

            // Send Packet with Result
            u8 resultAsValue = *reinterpret_cast<u8*>(&result);

            Util::Network::SendPacket(networkState, socketID, MetaGen::Shared::Packet::ServerCheatCommandResultPacket{
                .command = static_cast<u8>(MetaGen::Shared::Cheat::CheatCommandEnum::CharacterRemove),
                .result = resultAsValue,
                .response = "Unknown"
                });
            return true;
        }

        // Disconnect Character if online
        {
            Network::SocketID characterSocketID;
            if (Util::Network::GetSocketIDFromCharacterID(networkState, characterIDToDelete, characterSocketID))
            {
                networkState.server->CloseSocketID(characterSocketID);
            }
        }

        ECS::Result deletionResult = Util::Persistence::Character::CharacterDelete(gameCache, characterIDToDelete);
        result.CharacterDoesNotExist = deletionResult == ECS::Result::CharacterNotFound;
        result.DatabaseTransactionFailed = deletionResult == ECS::Result::DatabaseError;

        u8 cheatCommandResultVal = *reinterpret_cast<u8*>(&result);

        Util::Network::SendPacket(networkState, socketID, MetaGen::Shared::Packet::ServerCheatCommandResultPacket{
            .command = static_cast<u8>(MetaGen::Shared::Cheat::CheatCommandEnum::CharacterRemove),
            .result = cheatCommandResultVal,
            .response = "Unknown"
            });

        return true;
    }

    bool HandleOnCheatCreatureAdd(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        u32 creatureTemplateID = 0;
        if (!message.buffer->GetU32(creatureTemplateID))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        GameDefine::Database::CreatureTemplate* creatureTemplate = nullptr;
        if (!Util::Cache::GetCreatureTemplateByID(gameCache, creatureTemplateID, creatureTemplate))
            return true;

        auto& playerTransform = world.Get<Components::Transform>(entity);

        entt::entity creatureEntity = world.CreateEntity();
        world.EmplaceOrReplace<Events::CreatureCreate>(creatureEntity, creatureTemplateID, creatureTemplate->displayID, playerTransform.mapID, playerTransform.position, vec3(creatureTemplate->scale), playerTransform.pitchYaw.y);

        return true;
    }
    bool HandleOnCheatCreatureRemove(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        ObjectGUID creatureGUID;
        if (!message.buffer->Deserialize(creatureGUID))
            return false;

        entt::entity creatureEntity = world.GetEntity(creatureGUID);
        if (creatureEntity == entt::null)
            return true;

        world.EmplaceOrReplace<Events::CreatureNeedsDeinitialization>(creatureEntity, creatureGUID);
        return true;
    }
    bool HandleOnCheatCreatureInfo(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        ObjectGUID creatureGUID;
        if (!message.buffer->Deserialize(creatureGUID))
            return false;

        entt::entity creatureEntity = world.GetEntity(creatureGUID);
        if (creatureEntity == entt::null)
            return true;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();
        auto& networkState = ctx.get<Singletons::NetworkState>();

        if (!world.AllOf<Components::CreatureInfo>(creatureEntity))
        {
            Util::Unit::SendChatMessage(world, networkState, socketID, "Failed to get creature info, entity is not a creature");
            return true;
        }

        auto& creatureInfo = world.Get<Components::CreatureInfo>(creatureEntity);

        GameDefine::Database::CreatureTemplate* creatureTemplate = nullptr;
        if (!Util::Cache::GetCreatureTemplateByID(gameCache, creatureInfo.templateID, creatureTemplate))
            return true;

        std::string response = std::format("Creature Info:\n- GUID : {}\n- TemplateID : {}\n- Name : {}\n- Display ID : {}\n- Mods : (Health : {}, Armor : {}, Resource : {})\n- AI Script Name : ",
            creatureGUID.ToString(),
            creatureInfo.templateID,
            creatureInfo.name,
            creatureTemplate->displayID,
            creatureTemplate->healthMod,
            creatureTemplate->armorMod,
            creatureTemplate->resourceMod);

        if (auto* creatureAIInfo = world.TryGet<Components::CreatureAIInfo>(creatureEntity))
        {
            response += std::format("(\"{}\")", creatureAIInfo->scriptName);
        }
        else
        {
            response += "(none)";
        }

        Util::Unit::SendChatMessage(world, networkState, socketID, response);

        return true;
    }

    bool HandleOnCheatDamage(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        u32 damage = 0;
        if (!message.buffer->GetU32(damage))
            return false;

        auto& combatEventState = world.GetSingleton<Singletons::CombatEventState>();

        entt::entity targetEntity = entity;
        if (auto* targetInfo = world.TryGet<Components::TargetInfo>(entity))
        {
            if (world.ValidEntity(targetInfo->target))
            {
                targetEntity = targetInfo->target;
            }
        }

        auto& srcObjectInfo = world.Get<Components::ObjectInfo>(entity);
        auto& targetObjectInfo = world.Get<Components::ObjectInfo>(targetEntity);

        ObjectGUID::Type targetType = targetObjectInfo.guid.GetType();
        if (targetType != ObjectGUID::Type::Creature && targetType != ObjectGUID::Type::Player)
            return true;

        Util::CombatEvent::AddDamageEvent(combatEventState, srcObjectInfo.guid, targetObjectInfo.guid, damage);

        return true;
    }
    bool HandleOnCheatKill(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        auto& combatEventState = world.GetSingleton<Singletons::CombatEventState>();

        entt::entity targetEntity = entity;
        if (auto* targetInfo = world.TryGet<Components::TargetInfo>(entity))
        {
            if (world.ValidEntity(targetInfo->target))
                targetEntity = targetInfo->target;
        }

        auto& srcObjectInfo = world.Get<Components::ObjectInfo>(entity);
        auto& targetObjectInfo = world.Get<Components::ObjectInfo>(targetEntity);

        ObjectGUID::Type targetType = targetObjectInfo.guid.GetType();
        if (targetType != ObjectGUID::Type::Creature && targetType != ObjectGUID::Type::Player)
            return true;

        constexpr u32 amount = std::numeric_limits<u32>().max();
        Util::CombatEvent::AddDamageEvent(combatEventState, srcObjectInfo.guid, targetObjectInfo.guid, amount);

        return true;
    }
    bool HandleOnCheatResurrect(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        auto& combatEventState = world.GetSingleton<Singletons::CombatEventState>();

        entt::entity targetEntity = entity;
        if (auto* targetInfo = world.TryGet<Components::TargetInfo>(entity))
        {
            if (world.ValidEntity(targetInfo->target))
            {
                targetEntity = targetInfo->target;
            }
        }

        auto& srcObjectInfo = world.Get<Components::ObjectInfo>(entity);
        auto& targetObjectInfo = world.Get<Components::ObjectInfo>(targetEntity);

        ObjectGUID::Type targetType = targetObjectInfo.guid.GetType();
        if (targetType != ObjectGUID::Type::Creature && targetType != ObjectGUID::Type::Player)
            return true;

        Util::CombatEvent::AddResurrectEvent(combatEventState, srcObjectInfo.guid, targetObjectInfo.guid);
        return true;
    }
    bool HandleOnCheatMorph(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        u32 displayID = 0;
        if (!message.buffer->GetU32(displayID))
            return false;

        auto& unitFields = world.Get<Components::UnitFields>(entity);
        Util::Unit::UpdateDisplayID(*world.registry, entity, unitFields, displayID);

        return true;
    }
    bool HandleOnCheatDemorph(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        auto& unitFields = world.Get<Components::UnitFields>(entity);

        u32 levelRaceGenderClassPacked = unitFields.fields.GetField<u32>(MetaGen::Shared::NetField::UnitNetFieldEnum::LevelRaceGenderClassPacked);
        GameDefine::UnitRace unitRace = static_cast<GameDefine::UnitRace>((levelRaceGenderClassPacked >> 16) & 0x7F);
        GameDefine::UnitGender unitGender = static_cast<GameDefine::UnitGender>((levelRaceGenderClassPacked >> 23) & 0x3);

        u32 nativeDisplayID = Util::Unit::GetDisplayIDFromRaceGender(unitRace, unitGender);
        Util::Unit::UpdateDisplayID(*world.registry, entity, unitFields, nativeDisplayID);

        return true;
    }
    bool HandleOnCheatUnitSetRace(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        GameDefine::UnitRace unitRace = GameDefine::UnitRace::None;
        if (!message.buffer->Get(unitRace))
            return false;

        if (unitRace == GameDefine::UnitRace::None || unitRace > GameDefine::UnitRace::Troll)
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();
        auto& objectInfo = world.Get<Components::ObjectInfo>(entity);
        u64 characterID = objectInfo.guid.GetCounter();

        if (Util::Persistence::Character::CharacterSetRace(gameCache, *registry, characterID, unitRace) != ECS::Result::Success)
            return false;

        auto& unitFields = world.Get<Components::UnitFields>(entity);
        Util::Unit::UpdateDisplayRace(*world.registry, entity, unitFields, unitRace);

        return true;
    }
    bool HandleOnCheatUnitSetGender(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        GameDefine::UnitGender unitGender = GameDefine::UnitGender::None;
        if (!message.buffer->Get(unitGender))
            return false;

        if (unitGender == GameDefine::UnitGender::None || unitGender > GameDefine::UnitGender::Other)
            return false;

        entt::registry::context& ctx = registry->ctx();
        Singletons::GameCache& gameCache = ctx.get<Singletons::GameCache>();
        auto& objectInfo = world.Get<Components::ObjectInfo>(entity);
        u64 characterID = objectInfo.guid.GetCounter();

        if (Util::Persistence::Character::CharacterSetGender(gameCache, *world.registry, characterID, unitGender) != ECS::Result::Success)
            return false;

        auto& unitFields = world.Get<Components::UnitFields>(entity);
        Util::Unit::UpdateDisplayGender(*world.registry, entity, unitFields, unitGender);
        
        return true;
    }
    bool HandleOnCheatUnitSetClass(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        GameDefine::UnitClass unitClass = GameDefine::UnitClass::None;
        if (!message.buffer->Get(unitClass))
            return false;

        if (unitClass == GameDefine::UnitClass::None || unitClass > GameDefine::UnitClass::Druid)
            return false;

        entt::registry::context& ctx = registry->ctx();
        Singletons::GameCache& gameCache = ctx.get<Singletons::GameCache>();
        auto& objectInfo = world.Get<Components::ObjectInfo>(entity);
        u64 characterID = objectInfo.guid.GetCounter();

        if (Util::Persistence::Character::CharacterSetClass(gameCache, *world.registry, characterID, unitClass) != ECS::Result::Success)
            return false;

        return true;
    }
    bool HandleOnCheatUnitSetLevel(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        u16 level = 0;
        if (!message.buffer->Get(level))
            return false;

        if (level == 0)
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();
        auto& objectInfo = world.Get<Components::ObjectInfo>(entity);
        u64 characterID = objectInfo.guid.GetCounter();

        if (Util::Persistence::Character::CharacterSetLevel(gameCache, *world.registry, characterID, level) != ECS::Result::Success)
            return false;

        return true;
    }

    bool HandleOnCheatItemAdd(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        u32 itemID = 0;
        u32 itemCount = 0;

        if (!message.buffer->GetU32(itemID))
            return false;

        if (!message.buffer->GetU32(itemCount))
            return false;

        if (itemCount == 0)
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();
        auto& networkState = ctx.get<Singletons::NetworkState>();

        bool isValidItemID = Util::Cache::ItemTemplateExistsByID(gameCache, itemID);
        bool playerHasBags = world.AllOf<Components::PlayerContainers>(entity);

        if (!playerHasBags || !isValidItemID)
            return true;

        auto& playerContainers = world.Get<Components::PlayerContainers>(entity);

        u16 containerIndex = Database::Container::INVALID_SLOT;
        u16 slotIndex = Database::Container::INVALID_SLOT;
        bool canAddItem = Util::Container::GetFirstFreeSlotInBags(playerContainers, containerIndex, slotIndex);
        if (!canAddItem)
            return true;

        u64 characterID = world.Get<Components::ObjectInfo>(entity).guid.GetCounter();

        const auto& baseContainerItem = playerContainers.equipment.GetItem(containerIndex);

        u64 containerID = baseContainerItem.objectGUID.GetCounter();
        Database::Container& container = playerContainers.bags[containerIndex];

        u64 itemInstanceID;
        if (Util::Persistence::Character::ItemAdd(gameCache, *world.registry, entity, characterID, itemID, container, containerID, slotIndex, itemInstanceID) == ECS::Result::Success)
        {
            Database::ItemInstance* itemInstance = nullptr;
            if (Util::Cache::GetItemInstanceByID(gameCache, itemInstanceID, itemInstance))
            {
                ObjectGUID itemInstanceGUID = ObjectGUID::CreateItem(itemInstanceID);
                Util::Network::SendPacket(networkState, socketID, MetaGen::Shared::Packet::ServerItemAddPacket{
                    .guid = itemInstanceGUID,
                    .itemID = itemInstance->itemID,
                    .count = itemInstance->count,
                    .durability = itemInstance->durability
                });
            }
        }

        return true;
    }
    bool HandleOnCheatItemRemove(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        u32 itemID = 0;
        u32 itemCount = 0;

        if (!message.buffer->GetU32(itemID))
            return false;

        if (!message.buffer->GetU32(itemCount))
            return false;

        if (itemCount == 0)
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        bool isValidItemID = Util::Cache::ItemTemplateExistsByID(gameCache, itemID);
        bool playerHasBags = world.AllOf<Components::PlayerContainers>(entity);

        if (!playerHasBags || !isValidItemID)
            return true;

        auto& playerContainers = world.Get<Components::PlayerContainers>(entity);
        Database::Container* container = nullptr;
        u64 containerID = 0;
        u16 containerIndex = Database::Container::INVALID_SLOT;
        u16 slotIndex = Database::Container::INVALID_SLOT;

        bool foundItem = Util::Container::GetFirstItemSlot(gameCache, playerContainers, itemID, container, containerID, containerIndex, slotIndex);
        if (!foundItem)
            return true;

        u64 characterID = world.Get<Components::ObjectInfo>(entity).guid.GetCounter();
        Util::Persistence::Character::ItemDelete(gameCache, *world.registry, entity, characterID, *container, containerID, slotIndex);

        return true;
    }
    bool HandleOnCheatItemSetTemplate(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        GameDefine::Database::ItemTemplate itemTemplate;
        if (!GameDefine::Database::ItemTemplate::Read(message.buffer, itemTemplate))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        if (auto conn = gameCache.dbController->GetConnection(Database::DBType::Character))
        {
            auto transaction = conn->NewTransaction();
            auto queryResult = transaction.exec(pqxx::prepped("SetItemTemplate"), pqxx::params{ itemTemplate.id, itemTemplate.displayID, (u16)itemTemplate.bind, (u16)itemTemplate.rarity, (u16)itemTemplate.category, (u16)itemTemplate.type, itemTemplate.virtualLevel, itemTemplate.requiredLevel, itemTemplate.durability, itemTemplate.iconID, itemTemplate.name, itemTemplate.description, itemTemplate.armor, itemTemplate.statTemplateID, itemTemplate.armorTemplateID, itemTemplate.weaponTemplateID, itemTemplate.shieldTemplateID });
            if (queryResult.affected_rows() == 0)
            {
                transaction.abort();
            }
            else
            {
                transaction.commit();
                gameCache.itemTables.templateIDToTemplateDefinition[itemTemplate.id] = itemTemplate;
            }
        }

        return true;
    }
    bool HandleOnCheatSetItemStatTemplate(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        GameDefine::Database::ItemStatTemplate itemStatTemplate;
        if (!GameDefine::Database::ItemStatTemplate::Read(message.buffer, itemStatTemplate))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        if (auto conn = gameCache.dbController->GetConnection(Database::DBType::Character))
        {
            auto transaction = conn->NewTransaction();
            auto queryResult = transaction.exec(pqxx::prepped("SetItemStatTemplate"), pqxx::params{ itemStatTemplate.id, (u16)itemStatTemplate.statTypes[0], (u16)itemStatTemplate.statTypes[1], (u16)itemStatTemplate.statTypes[2], (u16)itemStatTemplate.statTypes[3], (u16)itemStatTemplate.statTypes[4], (u16)itemStatTemplate.statTypes[5], (u16)itemStatTemplate.statTypes[6], (u16)itemStatTemplate.statTypes[7], itemStatTemplate.statValues[0], itemStatTemplate.statValues[1], itemStatTemplate.statValues[2], itemStatTemplate.statValues[3], itemStatTemplate.statValues[4], itemStatTemplate.statValues[5], itemStatTemplate.statValues[6], itemStatTemplate.statValues[7] });
            if (queryResult.affected_rows() == 0)
            {
                transaction.abort();
            }
            else
            {
                transaction.commit();
                gameCache.itemTables.statTemplateIDToTemplateDefinition[itemStatTemplate.id] = itemStatTemplate;
            }
        }

        return true;
    }
    bool HandleOnCheatSetItemArmorTemplate(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        GameDefine::Database::ItemArmorTemplate itemArmorTemplate;
        if (!GameDefine::Database::ItemArmorTemplate::Read(message.buffer, itemArmorTemplate))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        if (auto conn = gameCache.dbController->GetConnection(Database::DBType::Character))
        {
            auto transaction = conn->NewTransaction();
            auto queryResult = transaction.exec(pqxx::prepped("SetItemArmorTemplate"), pqxx::params{ itemArmorTemplate.id, (u16)itemArmorTemplate.equipType, itemArmorTemplate.bonusArmor });
            if (queryResult.affected_rows() == 0)
            {
                transaction.abort();
            }
            else
            {
                transaction.commit();
                gameCache.itemTables.armorTemplateIDToTemplateDefinition[itemArmorTemplate.id] = itemArmorTemplate;
            }
        }

        return true;
    }
    bool HandleOnCheatSetItemWeaponTemplate(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        GameDefine::Database::ItemWeaponTemplate itemWeaponTemplate;
        if (!GameDefine::Database::ItemWeaponTemplate::Read(message.buffer, itemWeaponTemplate))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        if (auto conn = gameCache.dbController->GetConnection(Database::DBType::Character))
        {
            auto transaction = conn->NewTransaction();
            auto queryResult = transaction.exec(pqxx::prepped("SetItemWeaponTemplate"), pqxx::params{ itemWeaponTemplate.id, (u16)itemWeaponTemplate.weaponStyle, itemWeaponTemplate.minDamage, itemWeaponTemplate.maxDamage, itemWeaponTemplate.speed });
            if (queryResult.affected_rows() == 0)
            {
                transaction.abort();
            }
            else
            {
                transaction.commit();
                gameCache.itemTables.weaponTemplateIDToTemplateDefinition[itemWeaponTemplate.id] = itemWeaponTemplate;
            }
        }

        return true;
    }
    bool HandleOnCheatSetItemShieldTemplate(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        GameDefine::Database::ItemShieldTemplate itemShieldTemplate;
        if (!GameDefine::Database::ItemShieldTemplate::Read(message.buffer, itemShieldTemplate))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        if (auto conn = gameCache.dbController->GetConnection(Database::DBType::Character))
        {
            auto transaction = conn->NewTransaction();
            auto queryResult = transaction.exec(pqxx::prepped("SetItemShieldTemplate"), pqxx::params{ itemShieldTemplate.id, itemShieldTemplate.bonusArmor, itemShieldTemplate.block });
            if (queryResult.affected_rows() == 0)
            {
                transaction.abort();
            }
            else
            {
                transaction.commit();
                gameCache.itemTables.shieldTemplateIDToTemplateDefinition[itemShieldTemplate.id] = itemShieldTemplate;
            }
        }

        return true;
    }

    bool HandleOnCheatMapAdd(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        GameDefine::Database::Map map;
        if (!GameDefine::Database::Map::Read(message.buffer, map))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        if (auto conn = gameCache.dbController->GetConnection(Database::DBType::Character))
        {
            auto transaction = conn->NewTransaction();
            auto queryResult = transaction.exec(pqxx::prepped("SetMap"), pqxx::params{ map.id, map.flags, map.internalName, map.name, map.type, map.maxPlayers });
            if (queryResult.affected_rows() == 0)
            {
                transaction.abort();
            }
            else
            {
                transaction.commit();
                gameCache.mapTables.idToDefinition[map.id] = map;
            }
        }

        return true;
    }
    bool HandleOnCheatGotoAdd(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        std::string locationName;
        u32 mapID = 0;
        vec3 position = vec3(0.0f);
        f32 orientation = 0.0f;

        if (!message.buffer->GetString(locationName))
            return false;

        if (!message.buffer->GetU32(mapID))
            return false;

        if (!message.buffer->Get(position))
            return false;

        if (!message.buffer->GetF32(orientation))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        StringUtils::ToLower(locationName);
        u32 locationNameHash = StringUtils::fnv1a_32(locationName.c_str(), locationName.length());
        if (locationName.length() < 2 || locationName[0] == ' ' || gameCache.mapTables.locationNameHashToID.contains(locationNameHash))
        {
            // Location is Invalid or Already Exists
            return true;
        }

        if (auto conn = gameCache.dbController->GetConnection(Database::DBType::Character))
        {
            auto transaction = conn->NewTransaction();
            u32 locationID;
            if (Database::Util::Map::MapLocationCreate(transaction, locationName, mapID, position, orientation, locationID))
            {
                transaction.commit();
                gameCache.mapTables.locationIDToDefinition[locationID] = GameDefine::Database::MapLocation{ locationID, locationName, mapID, position.x, position.y, position.z, orientation };
                gameCache.mapTables.locationNameHashToID[locationNameHash] = locationID;
            }
        }

        return true;
    }
    bool HandleOnCheatGotoAddHere(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        std::string locationName;
        if (!message.buffer->GetString(locationName))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        StringUtils::ToLower(locationName);
        u32 locationNameHash = StringUtils::fnv1a_32(locationName.c_str(), locationName.length());
        if (locationName.length() < 2 || locationName[0] == ' ' || gameCache.mapTables.locationNameHashToID.contains(locationNameHash))
        {
            // Location is Invalid or Already Exists
            return true;
        }

        auto& playerTransform = world.Get<Components::Transform>(entity);
        if (auto conn = gameCache.dbController->GetConnection(Database::DBType::Character))
        {
            auto transaction = conn->NewTransaction();

            u32 locationID;
            if (Database::Util::Map::MapLocationCreate(transaction, locationName, playerTransform.mapID, playerTransform.position, playerTransform.pitchYaw.y, locationID))
            {
                transaction.commit();
                gameCache.mapTables.locationIDToDefinition[locationID] = GameDefine::Database::MapLocation{ locationID, locationName, playerTransform.mapID, playerTransform.position.x, playerTransform.position.y, playerTransform.position.z, playerTransform.pitchYaw.y };
                gameCache.mapTables.locationNameHashToID[locationNameHash] = locationID;
            }
        }

        return true;
    }
    bool HandleOnCheatGotoRemove(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        std::string locationName;
        if (!message.buffer->GetString(locationName))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        StringUtils::ToLower(locationName);
        u32 locationNameHash = StringUtils::fnv1a_32(locationName.c_str(), locationName.length());
        if (locationName.length() < 2 || locationName[0] == ' ' || !gameCache.mapTables.locationNameHashToID.contains(locationNameHash))
        {
            // Location is Invalid or Already Exists
            return true;
        }

        if (auto conn = gameCache.dbController->GetConnection(Database::DBType::Character))
        {
            auto transaction = conn->NewTransaction();

            u32 locationID = gameCache.mapTables.locationNameHashToID[locationNameHash];
            if (Database::Util::Map::MapLocationDelete(transaction, locationID))
            {
                transaction.commit();
                gameCache.mapTables.locationIDToDefinition.erase(locationID);
                gameCache.mapTables.locationNameHashToID.erase(locationNameHash);
            }
        }

        return true;
    }
    bool HandleOnCheatGotoMap(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        u32 mapID = 0;
        if (!message.buffer->GetU32(mapID))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        GameDefine::Database::Map* mapDefinition = nullptr;
        if (!Util::Cache::GetMapByID(gameCache, mapID, mapDefinition))
            return true;

        std::string locationName = mapDefinition->name;
        StringUtils::ToLower(locationName);
        u32 locationNameHash = StringUtils::fnv1a_32(locationName.c_str(), locationName.length());

        auto& transform = world.Get<Components::Transform>(entity);

        vec3 position = transform.position;
        f32 orientation = transform.pitchYaw.y;

        GameDefine::Database::MapLocation* location = nullptr;
        if (Util::Cache::GetLocationByHash(gameCache, locationNameHash, location))
        {
            position = vec3(location->positionX, location->positionY, location->positionZ);
            orientation = location->orientation;
        }

        auto& networkState = ctx.get<Singletons::NetworkState>();
        auto& worldState = ctx.get<Singletons::WorldState>();
        auto& objectInfo = world.Get<Components::ObjectInfo>(entity);
        auto& visibilityInfo = world.Get<Components::VisibilityInfo>(entity);

        Util::Unit::TeleportToLocation(worldState, world, gameCache, networkState, entity, objectInfo, transform, visibilityInfo, mapID, position, orientation);

        return true;
    }
    bool HandleOnCheatGotoLocation(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        std::string locationName;
        if (!message.buffer->GetString(locationName))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        StringUtils::ToLower(locationName);
        u32 locationNameHash = StringUtils::fnv1a_32(locationName.c_str(), locationName.length());

        GameDefine::Database::MapLocation* location = nullptr;
        if (!Util::Cache::GetLocationByHash(gameCache, locationNameHash, location))
            return true;

        auto& networkState = ctx.get<Singletons::NetworkState>();
        auto& worldState = ctx.get<Singletons::WorldState>();
        auto& objectInfo = world.Get<Components::ObjectInfo>(entity);
        auto& transform = world.Get<Components::Transform>(entity);
        auto& visibilityInfo = world.Get<Components::VisibilityInfo>(entity);

        vec3 position = vec3(location->positionX, location->positionY, location->positionZ);

        Util::Unit::TeleportToLocation(worldState, world, gameCache, networkState, entity, objectInfo, transform, visibilityInfo, location->mapID, position, location->orientation);

        return true;
    }
    bool HandleOnCheatGotoXYZ(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        vec3 position = vec3(0.0f);
        if (!message.buffer->Get(position))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();
        auto& networkState = ctx.get<Singletons::NetworkState>();
        auto& objectInfo = world.Get<Components::ObjectInfo>(entity);
        auto& transform = world.Get<Components::Transform>(entity);
        auto& visibilityInfo = world.Get<Components::VisibilityInfo>(entity);

        Util::Unit::TeleportToXYZ(world, networkState, entity, objectInfo, transform, visibilityInfo, position, transform.pitchYaw.y);
        return true;
    }

    bool HandleOnCheatTriggerAdd(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        std::string name;
        u16 flags;
        u16 mapID;
        vec3 position;
        vec3 extents;

        if (!message.buffer->GetString(name))
            return false;

        if (!message.buffer->GetU16(flags))
            return false;

        if (!message.buffer->GetU16(mapID))
            return false;

        if (!message.buffer->Get(position))
            return false;

        if (!message.buffer->Get(extents))
            return false;

        auto& networkState = registry->ctx().get<Singletons::NetworkState>();

        const auto& playerTransform = world.Get<Components::Transform>(entity);
        if (playerTransform.mapID != mapID)
            return false;

        entt::entity triggerEntity = world.CreateEntity();

        Events::ProximityTriggerCreate event =
        {
            .name = name,
            .flags = static_cast<MetaGen::Shared::ProximityTrigger::ProximityTriggerFlagEnum>(flags),

            .mapID = mapID,
            .position = position,
            .extents = extents
        };

        world.Emplace<Events::ProximityTriggerCreate>(triggerEntity, event);

        return true;
    }
    bool HandleOnCheatTriggerRemove(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        u32 triggerID;

        if (!message.buffer->GetU32(triggerID))
            return false;

        auto& proximityTriggers = world.GetSingleton<Singletons::ProximityTriggers>();

        entt::entity triggerEntity;
        if (!Util::ProximityTrigger::ProximityTriggerGetByID(proximityTriggers, triggerID, triggerEntity))
            return true;

        world.EmplaceOrReplace<Events::ProximityTriggerNeedsDeinitialization>(triggerEntity, triggerID);

        return true;
    }

    bool HandleOnCheatSpellSet(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        GameDefine::Database::Spell spell;
        if (!GameDefine::Database::Spell::Read(message.buffer, spell))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        if (auto conn = gameCache.dbController->GetConnection(Database::DBType::Character))
        {
            auto transaction = conn->NewTransaction();
            auto queryResult = transaction.exec(pqxx::prepped("SetSpell"), pqxx::params{ spell.id, spell.name, spell.description, spell.auraDescription, spell.iconID, spell.castTime, spell.cooldown, spell.duration });
            if (queryResult.affected_rows() != 0)
            {
                transaction.commit();
                gameCache.spellTables.idToDefinition[spell.id] = spell;
            }
        }

        return true;
    }
    bool HandleOnCheatSpellEffectSet(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        GameDefine::Database::SpellEffect spellEffect;
        if (!GameDefine::Database::SpellEffect::Read(message.buffer, spellEffect))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        if (auto conn = gameCache.dbController->GetConnection(Database::DBType::Character))
        {
            auto transaction = conn->NewTransaction();
            auto queryResult = transaction.exec(pqxx::prepped("SetSpellEffect"), pqxx::params{ spellEffect.id, spellEffect.spellID, (u16)spellEffect.effectPriority, (u16)spellEffect.effectType, spellEffect.effectValue1, spellEffect.effectValue2, spellEffect.effectValue3, spellEffect.effectMiscValue1, spellEffect.effectMiscValue2, spellEffect.effectMiscValue3 });
            if (queryResult.affected_rows() == 0)
            {
                transaction.abort();
            }
            else
            {
                transaction.commit();

                // Insert or Update effect in cache
                Database::SpellEffectInfo& effectList = gameCache.spellTables.spellIDToEffects[spellEffect.spellID];
                auto it = std::find_if(effectList.effects.begin(), effectList.effects.end(), [&spellEffect](const GameDefine::Database::SpellEffect& effect) {
                    return effect.id == spellEffect.id;
                });

                if (it != effectList.effects.end())
                {
                    *it = spellEffect;
                }
                else
                {
                    effectList.effects.push_back(spellEffect);
                }

                // Sort effects by priority
                std::sort(effectList.effects.begin(), effectList.effects.end(), [](const GameDefine::Database::SpellEffect& a, const GameDefine::Database::SpellEffect& b) {
                    return a.effectPriority < b.effectPriority;
                });

                // TODO : Update Regular Effects Mask, Proc Effects Mask and Proc Phase Type to Proc Info Mask
            }
        }

        return true;
    }
    bool HandleOnCheatSpellProcDataSet(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        u32 spellProcDataID = 0;
        MetaGen::Shared::ClientDB::SpellProcDataRecord spellProcData;

        if (!message.buffer->GetU32(spellProcDataID))
            return false;

        if (!message.buffer->Deserialize(spellProcData))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        if (auto conn = gameCache.dbController->GetConnection(Database::DBType::Character))
        {
            auto transaction = conn->NewTransaction();
            auto queryResult = transaction.exec(pqxx::prepped("SetSpellProcData"), pqxx::params{ spellProcDataID, (i32)spellProcData.phaseMask, (i64)spellProcData.typeMask, (i64)spellProcData.hitMask, (i64)spellProcData.flags, spellProcData.procsPerMinute, spellProcData.chanceToProc, (i32)spellProcData.internalCooldownMS, spellProcData.charges });
            if (queryResult.affected_rows() == 0)
            {
                transaction.abort();
            }
            else
            {
                transaction.commit();
            }
        }

        return true;
    }
    bool HandleOnCheatSpellProcLinkSet(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        u32 spellProcLinkID = 0;
        MetaGen::Shared::ClientDB::SpellProcLinkRecord spellProcLink;

        if (!message.buffer->GetU32(spellProcLinkID))
            return false;

        if (!message.buffer->Deserialize(spellProcLink))
            return false;

        entt::registry::context& ctx = registry->ctx();
        auto& gameCache = ctx.get<Singletons::GameCache>();

        if (auto conn = gameCache.dbController->GetConnection(Database::DBType::Character))
        {
            auto transaction = conn->NewTransaction();
            auto queryResult = transaction.exec(pqxx::prepped("SetSpellProcLink"), pqxx::params{ spellProcLinkID, spellProcLink.spellID, (i64)spellProcLink.effectMask, spellProcLink.procDataID });
            if (queryResult.affected_rows() == 0)
            {
                transaction.abort();
            }
            else
            {
                transaction.commit();
            }
        }

        return true;
    }

    bool HandleOnCheatCreatureAddScript(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        std::string scriptName;
        if (!message.buffer->GetString(scriptName))
            return false;

        auto& creatureAIState = world.GetSingleton<Singletons::CreatureAIState>();

        u32 scriptNameHash = StringUtils::fnv1a_32(scriptName.c_str(), scriptName.length());
        if (!creatureAIState.loadedScriptNameHashes.contains(scriptNameHash))
            return true;

        if (auto* targetInfo = world.TryGet<Components::TargetInfo>(entity))
        {
            if (world.ValidEntity(targetInfo->target))
            {
                if (auto* creatureAIInfo = world.TryGet<Components::CreatureAIInfo>(targetInfo->target))
                {
                    if (creatureAIInfo->scriptName == scriptName)
                        return true;

                    world.Emplace<Events::CreatureRemoveScript>(targetInfo->target);
                }

                world.Emplace<Events::CreatureAddScript>(targetInfo->target, scriptName);
            }
        }

        return true;
    }
    bool HandleOnCheatCreatureRemoveScript(entt::registry* registry, World& world, Network::SocketID socketID, entt::entity entity, Network::Message& message)
    {
        if (auto* targetInfo = world.TryGet<Components::TargetInfo>(entity))
        {
            if (world.ValidEntity(targetInfo->target))
            {
                world.Emplace<Events::CreatureRemoveScript>(targetInfo->target);
            }
        }

        return true;
    }

    bool HandleOnSendCheatCommand(World* world, entt::entity entity, Network::SocketID socketID, Network::Message& message)
    {
        entt::registry* registry = ServiceLocator::GetEnttRegistries()->gameRegistry;
        entt::registry::context& ctx = registry->ctx();

        auto& networkState = ctx.get<Singletons::NetworkState>();
        auto& worldState = ctx.get<Singletons::WorldState>();
        
        auto command = MetaGen::Shared::Cheat::CheatCommandEnum::None;
        if (!message.buffer->Get(command))
            return false;

        switch (command)
        {
            case MetaGen::Shared::Cheat::CheatCommandEnum::CharacterAdd:
            {
                return HandleOnCheatCharacterAdd(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::CharacterRemove:
            {
                return HandleOnCheatCharacterRemove(registry, *world, socketID, entity, message);
            }

            case MetaGen::Shared::Cheat::CheatCommandEnum::CreatureAdd:
            {
                return HandleOnCheatCreatureAdd(registry, *world, socketID, entity, message);
            }

            case MetaGen::Shared::Cheat::CheatCommandEnum::CreatureRemove:
            {
                return HandleOnCheatCreatureRemove(registry, *world, socketID, entity, message);
            }

            case MetaGen::Shared::Cheat::CheatCommandEnum::CreatureInfo:
            {
                return HandleOnCheatCreatureInfo(registry, *world, socketID, entity, message);
            }

            case MetaGen::Shared::Cheat::CheatCommandEnum::Damage:
            {
                return HandleOnCheatDamage(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::Kill:
            {
                return HandleOnCheatKill(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::Resurrect:
            {
                return HandleOnCheatResurrect(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::UnitMorph:
            {
                return HandleOnCheatMorph(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::UnitDemorph:
            {
                return HandleOnCheatDemorph(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::UnitSetRace:
            {
                return HandleOnCheatUnitSetRace(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::UnitSetGender:
            {
                return HandleOnCheatUnitSetGender(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::UnitSetClass:
            {
                return HandleOnCheatUnitSetClass(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::UnitSetLevel:
            {
                return HandleOnCheatUnitSetLevel(registry, *world, socketID, entity, message);
            }

            case MetaGen::Shared::Cheat::CheatCommandEnum::ItemAdd:
            {
                return HandleOnCheatItemAdd(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::ItemRemove:
            {
                return HandleOnCheatItemRemove(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::ItemSetTemplate:
            {
                return HandleOnCheatItemSetTemplate(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::ItemSetStatTemplate:
            {
                return HandleOnCheatSetItemStatTemplate(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::ItemSetArmorTemplate:
            {
                return HandleOnCheatSetItemArmorTemplate(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::ItemSetWeaponTemplate:
            {
                return HandleOnCheatSetItemWeaponTemplate(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::ItemSetShieldTemplate:
            {
                return HandleOnCheatSetItemShieldTemplate(registry, *world, socketID, entity, message);
            }

            case MetaGen::Shared::Cheat::CheatCommandEnum::MapAdd:
            {
                return HandleOnCheatMapAdd(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::GotoAddHere:
            {
                return HandleOnCheatGotoAddHere(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::GotoRemove:
            {
                return HandleOnCheatGotoRemove(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::GotoMap:
            {
                return HandleOnCheatGotoMap(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::GotoLocation:
            {
                return HandleOnCheatGotoLocation(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::GotoXYZ:
            {
                return HandleOnCheatGotoXYZ(registry, *world, socketID, entity, message);
            }

            case MetaGen::Shared::Cheat::CheatCommandEnum::TriggerAdd:
            {
                return HandleOnCheatTriggerAdd(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::TriggerRemove:
            {
                return HandleOnCheatTriggerRemove(registry, *world, socketID, entity, message);
            }

            case MetaGen::Shared::Cheat::CheatCommandEnum::SpellSet:
            {
                return HandleOnCheatSpellSet(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::SpellEffectSet:
            {
                return HandleOnCheatSpellEffectSet(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::SpellProcDataSet:
            {
                return HandleOnCheatSpellProcDataSet(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::SpellProcLinkSet:
            {
                return HandleOnCheatSpellProcLinkSet(registry, *world, socketID, entity, message);
            }

            case MetaGen::Shared::Cheat::CheatCommandEnum::CreatureAddScript:
            {
                return HandleOnCheatCreatureAddScript(registry, *world, socketID, entity, message);
            }
            case MetaGen::Shared::Cheat::CheatCommandEnum::CreatureRemoveScript:
            {
                return HandleOnCheatCreatureRemoveScript(registry, *world, socketID, entity, message);
            }

            default: break;
        }

        return true;
    }

    bool HandleOnConnect(World* world, entt::entity entity, Network::SocketID socketID, MetaGen::Shared::Packet::ClientConnectPacket& packet)
    {
        if (!StringUtils::StringIsAlphaAndAtLeastLength(packet.accountName, 2))
            return false;

        entt::registry* registry = ServiceLocator::GetEnttRegistries()->gameRegistry;
        entt::registry::context& ctx = registry->ctx();

        auto& networkState = ctx.get<Singletons::NetworkState>();

        AccountLoginRequest accountLoginRequest =
        {
            .socketID = socketID,
            .name = packet.accountName
        };
        networkState.accountLoginRequest.enqueue(accountLoginRequest);

        return true;
    }
    bool HandleOnAuthChallenge(World* world, entt::entity entity, Network::SocketID socketID, MetaGen::Shared::Packet::ClientAuthChallengePacket& packet)
    {
        entt::registry* registry = ServiceLocator::GetEnttRegistries()->gameRegistry;
        auto& networkState = registry->ctx().get<Singletons::NetworkState>();

        auto& authInfo = registry->get<Components::AuthenticationInfo>(entity);
        if (authInfo.state != AuthenticationState::Step1)
        {
            authInfo.state = AuthenticationState::Failed;
            return false;
        }

        auto& accountInfo = registry->get<Components::AccountInfo>(entity);

        unsigned char response2[crypto_spake_RESPONSE2BYTES];
        i32 result = crypto_spake_step2(&authInfo.serverState, response2, accountInfo.name.c_str(), accountInfo.name.length(), "NovusEngine", 11, authInfo.blob, packet.challenge.data());
        if (result != 0)
        {
            authInfo.state = AuthenticationState::Failed;
            return false;
        }

        authInfo.state = AuthenticationState::Step2;

        MetaGen::Shared::Packet::ServerAuthProofPacket authProofPacket;
        std::memcpy(authProofPacket.proof.data(), response2, crypto_spake_RESPONSE2BYTES);

        Util::Network::SendPacket(networkState, socketID, authProofPacket);

        return true;
    }
    bool HandleOnAuthProof(World* world, entt::entity entity, Network::SocketID socketID, MetaGen::Shared::Packet::ClientAuthProofPacket& packet)
    {
        entt::registry* registry = ServiceLocator::GetEnttRegistries()->gameRegistry;
        auto& networkState = registry->ctx().get<Singletons::NetworkState>();

        auto& authInfo = registry->get<Components::AuthenticationInfo>(entity);
        if (authInfo.state != AuthenticationState::Step2)
        {
            authInfo.state = AuthenticationState::Failed;
            return false;
        }

        auto& sessionKeys = networkState.socketIDToSessionKeys[socketID];
        i32 result = crypto_spake_step4(&authInfo.serverState, &sessionKeys, packet.proof.data());
        if (result != 0)
        {
            authInfo.state = AuthenticationState::Failed;
            return false;
        }

        authInfo.state = AuthenticationState::Completed;

        CharacterListRequest characterListRequest =
        {
            .socketID = socketID,
            .socketEntity = entity
        };
        networkState.characterListRequest.enqueue(characterListRequest);

        return true;
    }
    bool HandleOnCharacterSelect(World* world, entt::entity entity, Network::SocketID socketID, MetaGen::Shared::Packet::ClientCharacterSelectPacket& packet)
    {
        if (world || entity == entt::null)
            return false;

        entt::registry* registry = ServiceLocator::GetEnttRegistries()->gameRegistry;
        auto& networkState = registry->ctx().get<Singletons::NetworkState>();

        auto& authInfo = registry->get<Components::AuthenticationInfo>(entity);
        if (authInfo.state != AuthenticationState::Completed)
        {
            authInfo.state = AuthenticationState::Failed;
            return false;
        }

        auto& characterListInfo = registry->get<Components::CharacterListInfo>(entity);
        
        u32 numCharacters = static_cast<u32>(characterListInfo.list.size());
        if (packet.characterIndex >= numCharacters)
            return false;

        auto& characterEntry = characterListInfo.list[packet.characterIndex];

        CharacterLoginRequest characterLoginRequest =
        {
            .socketID = socketID,
            .name = characterEntry.name
        };
        networkState.characterLoginRequest.enqueue(characterLoginRequest);

        return true;
    }
    bool HandleOnCharacterLogout(World* world, entt::entity entity, Network::SocketID socketID, MetaGen::Shared::Packet::ClientCharacterLogoutPacket& packet)
    {
        if (!world || entity == entt::null)
            return false;

        entt::registry* registry = ServiceLocator::GetEnttRegistries()->gameRegistry;
        auto& networkState = registry->ctx().get<Singletons::NetworkState>();

        if (world->ValidEntity(entity))
        {
            world->EmplaceOrReplace<Events::CharacterNeedsDeinitialization>(entity);
        }

        networkState.server->SetSocketIDLane(socketID, Network::DEFAULT_LANE_ID);
        ECS::Util::Network::SendPacket(networkState, socketID, MetaGen::Shared::Packet::ServerCharacterLogoutPacket{});
        return true;
    }
    bool HandleOnPing(World* world, entt::entity entity, Network::SocketID socketID, MetaGen::Shared::Packet::ClientPingPacket& packet)
    {
        if (!world || entity == entt::null)
            return true;

        entt::registry* registry = ServiceLocator::GetEnttRegistries()->gameRegistry;
        auto& ctx = registry->ctx();

        auto& networkState = ctx.get<Singletons::NetworkState>();
        auto& timeState = ctx.get<Singletons::TimeState>();

        auto& netInfo = world->Get<Components::NetInfo>(entity);
        auto currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

        u64 timeSinceLastPing = currentTime - netInfo.lastPingTime;
        u8 serverDiff = static_cast<u8>(timeState.deltaTime * 1000.0f);

        if (timeSinceLastPing < Components::NetInfo::PING_INTERVAL - 1000)
        {
            netInfo.numEarlyPings++;
        }
        else if (timeSinceLastPing > Components::NetInfo::PING_INTERVAL + 1000)
        {
            netInfo.numLatePings++;
        }

        if (netInfo.numEarlyPings > Components::NetInfo::MAX_EARLY_PINGS ||
            netInfo.numLatePings > Components::NetInfo::MAX_LATE_PINGS)
        {
            networkState.server->CloseSocketID(socketID);
            return true;
        }

        netInfo.ping = packet.ping;
        netInfo.numEarlyPings = 0;
        netInfo.numLatePings = 0;
        netInfo.lastPingTime = currentTime;

        Util::Network::SendPacket(networkState, socketID, MetaGen::Shared::Packet::ServerUpdateStatsPacket{
            .serverTickTime = serverDiff
        });

        return true;
    }

    bool HandleOnClientUnitTargetUpdate(World* world, entt::entity entity, Network::SocketID socketID, MetaGen::Shared::Packet::ClientUnitTargetUpdatePacket& packet)
    {
        entt::registry* registry = ServiceLocator::GetEnttRegistries()->gameRegistry;
        auto& networkState = registry->ctx().get<Singletons::NetworkState>();

        auto& objectInfo = world->Get<Components::ObjectInfo>(entity);
        auto& visibilityInfo = world->Get<Components::VisibilityInfo>(entity);
        auto& targetInfo = world->Get<Components::TargetInfo>(entity);

        entt::entity targetEntity = world->GetEntity(packet.targetGUID);
        targetInfo.target = world->ValidEntity(targetEntity) ? targetEntity : entt::null;

        ECS::Util::Network::SendToNearby(networkState, *world, entity, visibilityInfo, false, MetaGen::Shared::Packet::ServerUnitTargetUpdatePacket{
            .guid = objectInfo.guid,
            .targetGUID = packet.targetGUID
        });

        return true;
    }
    bool HandleOnClientUnitMove(World* world, entt::entity entity, Network::SocketID socketID, MetaGen::Shared::Packet::ClientUnitMovePacket& packet)
    {
        entt::registry* registry = ServiceLocator::GetEnttRegistries()->gameRegistry;
        auto& ctx = registry->ctx();

        auto& networkState = ctx.get<Singletons::NetworkState>();

        auto& objectInfo = world->Get<Components::ObjectInfo>(entity);
        auto& transform = world->Get<Components::Transform>(entity);
        transform.position = packet.position;
        transform.pitchYaw = packet.pitchYaw;

        world->playerVisData.Update(objectInfo.guid, packet.position.x, packet.position.z);

        auto& visibilityInfo = world->Get<Components::VisibilityInfo>(entity);
        ECS::Util::Network::SendToNearby(networkState, *world, entity, visibilityInfo, false, MetaGen::Shared::Packet::ServerUnitMovePacket{
            .guid = objectInfo.guid,
            .movementFlags = packet.movementFlags,
            .position = packet.position,
            .pitchYaw = packet.pitchYaw,
            .verticalVelocity = packet.verticalVelocity
        });

        return true;
    }
    bool HandleOnClientContainerSwapSlots(World* world, entt::entity entity, Network::SocketID socketID, MetaGen::Shared::Packet::SharedContainerSwapSlotsPacket& packet)
    {
        if (packet.srcContainer == packet.dstContainer && packet.srcSlot == packet.dstSlot)
            return false;

        entt::registry* registry = ServiceLocator::GetEnttRegistries()->gameRegistry;
        auto& ctx = registry->ctx();

        auto& gameCache = ctx.get<Singletons::GameCache>();
        auto& networkState = ctx.get<Singletons::NetworkState>();

        auto& playerContainers = world->Get<Components::PlayerContainers>(entity);

        Database::Container* srcContainer = nullptr;
        u64 srcContainerID = 0;

        Database::Container* dstContainer = nullptr;
        u64 dstContainerID = 0;

        bool isSameContainer = packet.srcContainer == packet.dstContainer;
        if (isSameContainer)
        {
            if (packet.srcContainer == 0)
            {
                srcContainer = &playerContainers.equipment;
                srcContainerID = 0;
            }
            else
            {
                u32 bagIndex = PLAYER_BAG_INDEX_START + packet.srcContainer - 1;
                const Database::ContainerItem& bagContainerItem = playerContainers.equipment.GetItem(bagIndex);

                if (bagContainerItem.IsEmpty())
                    return false;

                srcContainer = &playerContainers.bags[bagIndex];
                srcContainerID = bagContainerItem.objectGUID.GetCounter();
            }

            u32 numContainerSlots = srcContainer->GetTotalSlots();
            if (packet.srcSlot >= numContainerSlots || packet.dstSlot >= numContainerSlots)
                return false;

            auto& srcItem = srcContainer->GetItem(packet.srcSlot);
            if (srcItem.IsEmpty())
                return false;

            dstContainer = srcContainer;
            dstContainerID = srcContainerID;
        }
        else
        {
            if (packet.srcContainer == 0)
            {
                srcContainer = &playerContainers.equipment;
            }
            else
            {
                u32 bagIndex = PLAYER_BAG_INDEX_START + packet.srcContainer - 1;
                const Database::ContainerItem& bagContainerItem = playerContainers.equipment.GetItem(bagIndex);

                if (bagContainerItem.IsEmpty())
                    return false;

                srcContainer = &playerContainers.bags[bagIndex];
                srcContainerID = bagContainerItem.objectGUID.GetCounter();
            }

            if (packet.dstContainer == 0)
            {
                dstContainer = &playerContainers.equipment;
            }
            else
            {
                u32 bagIndex = PLAYER_BAG_INDEX_START + packet.dstContainer - 1;
                const Database::ContainerItem& bagContainerItem = playerContainers.equipment.GetItem(bagIndex);

                if (bagContainerItem.IsEmpty())
                    return false;

                dstContainer = &playerContainers.bags[bagIndex];
                dstContainerID = bagContainerItem.objectGUID.GetCounter();
            }

            if (packet.srcSlot >= srcContainer->GetTotalSlots() || packet.dstSlot >= dstContainer->GetTotalSlots())
                return false;

            auto& srcItem = srcContainer->GetItem(packet.srcSlot);
            if (srcItem.IsEmpty())
                return false;
        }

        auto& objectInfo = world->Get<Components::ObjectInfo>(entity);
        auto& visibilityInfo = world->Get<Components::VisibilityInfo>(entity);
        auto& visualItems = world->Get<Components::UnitVisualItems>(entity);
        u64 characterID = objectInfo.guid.GetCounter();

        if (Util::Persistence::Character::ItemSwap(gameCache, characterID, *srcContainer, srcContainerID, packet.srcSlot, *dstContainer, dstContainerID, packet.dstSlot) == ECS::Result::Success)
        {
            Util::Network::SendPacket(networkState, socketID, MetaGen::Shared::Packet::SharedContainerSwapSlotsPacket{
                .srcContainer = packet.srcContainer,
                .dstContainer = packet.dstContainer,
                .srcSlot = packet.srcSlot,
                .dstSlot = packet.dstSlot
            });

            if (srcContainerID == 0)
            {
                auto equippedSlot = static_cast<MetaGen::Shared::Unit::ItemEquipSlotEnum>(packet.srcSlot);
                if (equippedSlot >= MetaGen::Shared::Unit::ItemEquipSlotEnum::EquipmentStart && equippedSlot <= MetaGen::Shared::Unit::ItemEquipSlotEnum::EquipmentEnd)
                {
                    const Database::ContainerItem& containerItem = srcContainer->GetItem(packet.srcSlot);
                    bool isContainerItemEmpty = containerItem.IsEmpty();
                    u32 itemID = 0;

                    if (!isContainerItemEmpty)
                    {
                        Database::ItemInstance* itemInstance = nullptr;
                        if (Util::Cache::GetItemInstanceByID(gameCache, containerItem.objectGUID.GetCounter(), itemInstance))
                            itemID = itemInstance->itemID;
                    }

                    ECS::Util::Network::SendToNearby(networkState, *world, entity, visibilityInfo, true, MetaGen::Shared::Packet::ServerUnitEquippedItemUpdatePacket{
                        .guid = objectInfo.guid,
                        .slot = static_cast<u8>(packet.srcSlot),
                        .itemID = itemID
                    });

                    visualItems.equippedItemIDs[packet.srcSlot] = itemID;
                    visualItems.dirtyItemIDs.insert(packet.srcSlot);
                    world->EmplaceOrReplace<Events::CharacterNeedsVisualItemUpdate>(entity);
                    world->EmplaceOrReplace<Events::CharacterNeedsRecalculateStatsUpdate>(entity);
                }
            }

            if (dstContainerID == 0)
            {
                auto equippedSlot = static_cast<MetaGen::Shared::Unit::ItemEquipSlotEnum>(packet.dstSlot);
                if (equippedSlot >= MetaGen::Shared::Unit::ItemEquipSlotEnum::EquipmentStart && equippedSlot <= MetaGen::Shared::Unit::ItemEquipSlotEnum::EquipmentEnd)
                {
                    const Database::ContainerItem& containerItem = dstContainer->GetItem(packet.dstSlot);
                    bool isContainerItemEmpty = containerItem.IsEmpty();
                    u32 itemID = 0;

                    if (!isContainerItemEmpty)
                    {
                        Database::ItemInstance* itemInstance = nullptr;
                        if (Util::Cache::GetItemInstanceByID(gameCache, containerItem.objectGUID.GetCounter(), itemInstance))
                            itemID = itemInstance->itemID;
                    }

                    ECS::Util::Network::SendToNearby(networkState, *world, entity, visibilityInfo, true, MetaGen::Shared::Packet::ServerUnitEquippedItemUpdatePacket{
                        .guid = objectInfo.guid,
                        .slot = static_cast<u8>(packet.dstSlot),
                        .itemID = itemID
                    });

                    visualItems.equippedItemIDs[packet.dstSlot] = itemID;
                    visualItems.dirtyItemIDs.insert(packet.dstSlot);
                    world->EmplaceOrReplace<Events::CharacterNeedsVisualItemUpdate>(entity);
                    world->EmplaceOrReplace<Events::CharacterNeedsRecalculateStatsUpdate>(entity);
                }
            }
        }

        return true;
    }

    bool HandleOnClientTriggerEnter(World* world, entt::entity entity, Network::SocketID socketID, MetaGen::Shared::Packet::ClientTriggerEnterPacket& packet)
    {
        entt::registry* registry = ServiceLocator::GetEnttRegistries()->gameRegistry;
        auto& ctx = registry->ctx();
        auto& networkState = ctx.get<Singletons::NetworkState>();

        auto& proximityTriggers = world->GetSingleton<Singletons::ProximityTriggers>();

        entt::entity triggerEntity;
        if (!Util::ProximityTrigger::ProximityTriggerGetByID(proximityTriggers, packet.triggerID, triggerEntity))
            return true;

        auto& proximityTrigger = world->Get<Components::ProximityTrigger>(triggerEntity);

        // Only process if the server is authoritative for this trigger
        bool isServerAuthoritative = (proximityTrigger.flags & MetaGen::Shared::ProximityTrigger::ProximityTriggerFlagEnum::IsServerAuthorative) != MetaGen::Shared::ProximityTrigger::ProximityTriggerFlagEnum::None;
        if (!isServerAuthoritative)
            return false;

        auto& playerTransform = world->Get<Components::Transform>(entity);
        auto& playerAABB = world->Get<Components::AABB>(entity);
        auto& triggerTransform = world->Get<Components::Transform>(triggerEntity);
        auto& triggerAABB = world->Get<Components::AABB>(triggerEntity);

        if (!Util::Collision::Overlaps(playerTransform, playerAABB, triggerTransform, triggerAABB))
            return true;

        Util::ProximityTrigger::AddPlayerToTriggerEntered(*world, proximityTrigger, triggerEntity, entity);

        return true;
    }
    bool HandleOnClientSendChatMessage(World* world, entt::entity entity, Network::SocketID socketID, MetaGen::Shared::Packet::ClientSendChatMessagePacket& packet)
    {
        entt::registry* registry = ServiceLocator::GetEnttRegistries()->gameRegistry;
        auto& ctx = registry->ctx();

        auto& networkState = ctx.get<Singletons::NetworkState>();

        auto& objectInfo = world->Get<Components::ObjectInfo>(entity);
        auto& visibilityInfo = world->Get<Components::VisibilityInfo>(entity);
        ECS::Util::Network::SendToNearby(networkState, *world, entity, visibilityInfo, true, MetaGen::Shared::Packet::ServerSendChatMessagePacket{
            .guid = objectInfo.guid,
            .message = packet.message
        });

        return true;
    }
    bool HandleOnClientSpellCast(World* world, entt::entity entity, Network::SocketID socketID, MetaGen::Shared::Packet::ClientSpellCastPacket& packet)
    {
        entt::registry* registry = ServiceLocator::GetEnttRegistries()->gameRegistry;
        auto& gameCache = registry->ctx().get<Singletons::GameCache>();
        auto& networkState = registry->ctx().get<Singletons::NetworkState>();

        u8 result = 0;

        GameDefine::Database::Spell* spell = nullptr;
        auto& casterSpellCooldownHistory = world->Get<Components::UnitSpellCooldownHistory>(entity);

        if (!Util::Cache::GetSpellByID(gameCache, packet.spellID, spell))
        {
            result = 0x1;
        }
        else
        {
            f32 cooldown = Util::Unit::GetSpellCooldownRemaining(casterSpellCooldownHistory, packet.spellID);
            if (cooldown > 0.0f)
                result = 0x2;
        }

        if (result != 0x0)
        {
            Util::Network::SendPacket(networkState, socketID, MetaGen::Shared::Packet::ServerSpellCastResultPacket{
                .result = result,
            });

            return true;
        }

        auto& targetInfo = world->Get<Components::TargetInfo>(entity);
        auto& characterSpellCastInfo = world->Get<Components::CharacterSpellCastInfo>(entity);

        entt::entity spellEntity = entt::null;
        if (characterSpellCastInfo.activeSpellEntity == entt::null)
        {
            spellEntity = world->CreateEntity();
            characterSpellCastInfo.activeSpellEntity = spellEntity;

            world->Emplace<Tags::IsUnpreparedSpell>(spellEntity);
        }
        else
        {
            if (characterSpellCastInfo.queuedSpellEntity == entt::null)
            {
                spellEntity = world->CreateEntity();
                characterSpellCastInfo.queuedSpellEntity = spellEntity;
            }
            else
            {
                spellEntity = characterSpellCastInfo.queuedSpellEntity;
            }
        }

        auto& casterObjectInfo = world->Get<Components::ObjectInfo>(entity);

        ObjectGUID targetGUID = ObjectGUID::Empty;
        if (world->ValidEntity(targetInfo.target))
        {
            auto& targetObjectInfo = world->Get<Components::ObjectInfo>(targetInfo.target);
            targetGUID = targetObjectInfo.guid;
        }

        auto& spellInfo = world->EmplaceOrReplace<Components::SpellInfo>(spellEntity);
        auto& spellEffectInfo = world->EmplaceOrReplace<Components::SpellEffectInfo>(spellEntity);

        spellInfo.spellID = packet.spellID;
        spellInfo.caster = casterObjectInfo.guid;
        spellInfo.target = targetGUID;
        spellInfo.castTime = spell->castTime;
        spellInfo.timeToCast = spellInfo.castTime;

        Util::Network::SendPacket(networkState, socketID, MetaGen::Shared::Packet::ServerSpellCastResultPacket{
            .result = result,
        });

        return true;
    }
    
    bool HandleOnClientPathGenerate(World* world, entt::entity entity, Network::SocketID socketID, MetaGen::Shared::Packet::ClientPathGeneratePacket& packet)
    {
        entt::registry* registry = ServiceLocator::GetEnttRegistries()->gameRegistry;
        auto& networkState = registry->ctx().get<Singletons::NetworkState>();

        dtQueryFilter filter;
        filter.setIncludeFlags(0xFFFF);
        filter.setExcludeFlags(0);

        // AV : Ally Start -> Horde Base
        //vec3 startPos = vec3(490.59f, 97.50f, -764.04f);
        //vec3 endPos = vec3(289.57f, 90.45f, 1319.79f);

        vec3 startPos = vec3(packet.start.x, packet.start.y, -packet.start.z);
        vec3 endPos = vec3(packet.end.x, packet.end.y, -packet.end.z);
        vec3 polyPickExt = vec3(50.0f, 100.0f, 50.0f);

        dtPolyRef startRef, endRef;

        auto* navMesh = world->navmeshData.GetNavMesh();
        auto* navQuery = world->navmeshData.GetQuery();

        navQuery->findNearestPoly(&startPos.x, &polyPickExt.x, &filter, &startRef, nullptr);
        navQuery->findNearestPoly(&endPos.x, &polyPickExt.x, &filter, &endRef, nullptr);
        
        if (startRef == 0 || endRef == 0)
            return true;

        navQuery->closestPointOnPoly(startRef, &startPos.x, &startPos.x, nullptr);
        navQuery->closestPointOnPoly(endRef, &endPos.x, &endPos.x, nullptr);

        dtPolyRef path[1024];
        i32 pathCount = 0;
        navQuery->findPath(startRef, endRef, &startPos.x, &endPos.x, &filter, path, &pathCount, 1024);

        if (pathCount == 0)
            return true;

        vec3 lastPointPos = endPos;
        dtPolyRef lastRef = path[pathCount - 1];
        if (lastRef != endRef)
            navQuery->closestPointOnPoly(lastRef, &endPos.x, &lastPointPos.x, nullptr);

        dtPolyRef straightPathPolyRefs[1024];
        f32 straightPath[1024 * 3];
        i32 straightPathCount = 0;

        navQuery->findStraightPath(&startPos.x, &lastPointPos.x, path, pathCount, straightPath, nullptr, straightPathPolyRefs, &straightPathCount, 1024);

        for (i32 i = 0; i < straightPathCount; i++)
        {
            straightPath[i * 3 + 2] *= -1.0f;
        }

        if (straightPathCount > 0)
        {
            std::shared_ptr<Bytebuffer> buffer = Bytebuffer::Borrow<8192>();
            ECS::Util::MessageBuilder::CreatePacket(buffer, MetaGen::Shared::Packet::ServerPathVisualizationPacket::PACKET_ID, [&, straightPathCount]()
            {
                u32 numPathCount = static_cast<u32>(straightPathCount);
                buffer->PutU32(numPathCount);
                buffer->PutBytes(straightPath, numPathCount * sizeof(vec3));
            });

            Util::Network::SendPacket(networkState, socketID, buffer);
        }

        return true;
    }

    void NetworkConnection::Init(entt::registry& registry)
    {
        entt::registry::context& ctx = registry.ctx();

        auto& networkState = ctx.emplace<Singletons::NetworkState>();

        // Setup NetServer
        {
            u16 port = 4000;
            networkState.server = std::make_unique<Network::Server>(port);
            networkState.messageRouter = std::make_unique<Network::MessageRouter>();

            networkState.messageRouter->SetMessageHandler(MetaGen::Shared::Packet::ClientSendCheatCommandPacket::PACKET_ID, Network::MessageHandler(Network::ConnectionStatus::Connected, 0u, -1, &HandleOnSendCheatCommand));

            networkState.messageRouter->RegisterPacketHandler(Network::ConnectionStatus::Connected, HandleOnConnect);
            networkState.messageRouter->RegisterPacketHandler(Network::ConnectionStatus::Connected, HandleOnAuthChallenge);
            networkState.messageRouter->RegisterPacketHandler(Network::ConnectionStatus::Connected, HandleOnAuthProof);
            networkState.messageRouter->RegisterPacketHandler(Network::ConnectionStatus::Connected, HandleOnCharacterSelect);
            networkState.messageRouter->RegisterPacketHandler(Network::ConnectionStatus::Connected, HandleOnCharacterLogout);
            networkState.messageRouter->RegisterPacketHandler(Network::ConnectionStatus::Connected, HandleOnPing);

            networkState.messageRouter->RegisterPacketHandler(Network::ConnectionStatus::Connected, HandleOnClientUnitTargetUpdate);
            networkState.messageRouter->RegisterPacketHandler(Network::ConnectionStatus::Connected, HandleOnClientUnitMove);
            networkState.messageRouter->RegisterPacketHandler(Network::ConnectionStatus::Connected, HandleOnClientContainerSwapSlots);
            networkState.messageRouter->RegisterPacketHandler(Network::ConnectionStatus::Connected, HandleOnClientTriggerEnter);
            networkState.messageRouter->RegisterPacketHandler(Network::ConnectionStatus::Connected, HandleOnClientSendChatMessage);
            networkState.messageRouter->RegisterPacketHandler(Network::ConnectionStatus::Connected, HandleOnClientSpellCast);
            networkState.messageRouter->RegisterPacketHandler(Network::ConnectionStatus::Connected, HandleOnClientPathGenerate);

            // Bind to IP/Port
            std::string ipAddress = "0.0.0.0";
            
            if (networkState.server->Start())
            {
                NC_LOG_INFO("Network : Listening on ({0}, {1})", ipAddress, port);
            }
        }
    }

    void NetworkConnection::Update(entt::registry& registry, f32 deltaTime)
    {
        entt::registry::context& ctx = registry.ctx();

        auto& gameCache = ctx.get<Singletons::GameCache>();
        auto& networkState = ctx.get<Singletons::NetworkState>();
        auto& worldState = ctx.get<Singletons::WorldState>();

        // Handle 'SocketConnectedEvent'
        {
            moodycamel::ConcurrentQueue<Network::SocketConnectedEvent>& connectedEvents = networkState.server->GetConnectedEvents();

            Network::SocketConnectedEvent connectedEvent;
            while (connectedEvents.try_dequeue(connectedEvent))
            {
                const Network::ConnectionInfo& connectionInfo = connectedEvent.connectionInfo;
                NC_LOG_INFO("Network : Client connected from (SocketID : {0}, \"{1}:{2}\")", static_cast<u32>(connectedEvent.socketID), connectionInfo.ipAddr, connectionInfo.port);

                Util::Network::ActivateSocket(networkState, connectedEvent.socketID);
            }
        }

        // Handle 'SocketDisconnectedEvent'
        {
            moodycamel::ConcurrentQueue<Network::SocketDisconnectedEvent>& disconnectedEvents = networkState.server->GetDisconnectedEvents();

            Network::SocketDisconnectedEvent disconnectedEvent;
            while (disconnectedEvents.try_dequeue(disconnectedEvent))
            {
                Network::SocketID socketID = disconnectedEvent.socketID;
                NC_LOG_INFO("Network : Client disconnected (SocketID : {0})", static_cast<u32>(socketID));

                Util::Network::DeactivateSocket(networkState, socketID);
                networkState.socketIDToSessionKeys.erase(socketID);

                entt::entity characterEntity;
                if (Util::Network::GetCharacterEntity(networkState, socketID, characterEntity))
                {
                    u16 mapID;
                    if (worldState.GetMapIDFromSocket(socketID, mapID))
                    {
                        worldState.ClearMapIDForSocket(socketID);
                        World& world = worldState.GetWorld(mapID);

                        if (world.ValidEntity(characterEntity))
                        {
                            world.EmplaceOrReplace<Events::CharacterNeedsDeinitialization>(characterEntity);
                        }
                    }
                    else
                    {
                        registry.destroy(characterEntity);
                    }
                }

                entt::entity accountEntity;
                if (Util::Network::GetAccountEntity(networkState, socketID, accountEntity))
                {
                    auto& accountInfo = registry.get<Components::AccountInfo>(accountEntity);

                    Util::Network::UnlinkSocketFromAccount(networkState, socketID, accountInfo.id);
                    registry.destroy(accountEntity);
                }
            }
        }

        // Handle 'SocketMessageEvent'
        {
            moodycamel::ConcurrentQueue<Network::SocketMessageEvent>& messageEvents = networkState.server->GetMessageEvents(Network::DEFAULT_LANE_ID);
            Scripting::LuaManager* luaManager = ServiceLocator::GetLuaManager();

            auto key = Scripting::ZenithInfoKey::MakeGlobal(0, 0);
            Scripting::Zenith* zenith = luaManager->GetZenithStateManager().Get(key);

            Network::SocketMessageEvent messageEvent;
            while (messageEvents.try_dequeue(messageEvent))
            {
                if (!Util::Network::IsSocketActive(networkState, messageEvent.socketID))
                    continue;

                Network::MessageHeader messageHeader;
                if (networkState.messageRouter->GetMessageHeader(messageEvent.message, messageHeader))
                {
                    entt::entity accountEntity;
                    Util::Network::GetAccountEntity(networkState, messageEvent.socketID, accountEntity);

                    bool hasLuaHandlerForOpcode = zenith && Scripting::Util::Network::HasPacketHandler(zenith, messageHeader.opcode);
                    if (hasLuaHandlerForOpcode)
                    {
                        u32 messageReadOffset = static_cast<u32>(messageEvent.message.buffer->readData);
                        if (Scripting::Util::Network::CallPacketHandler(zenith, messageHeader, messageEvent.message))
                        {
                            if (!networkState.messageRouter->HasValidHandlerForHeader(messageHeader))
                                continue;
                        
                            if (networkState.messageRouter->CallHandler(nullptr, accountEntity, messageEvent.socketID, messageHeader, messageEvent.message))
                                continue;
                        }
                    }
                    else
                    {
                        if (networkState.messageRouter->HasValidHandlerForHeader(messageHeader))
                        {
                            if (networkState.messageRouter->CallHandler(nullptr, accountEntity, messageEvent.socketID, messageHeader, messageEvent.message))
                                continue;
                        }
                    }
                }

                // Failed to Call Handler, Close Socket
                {
                    Util::Network::RequestSocketToClose(networkState, messageEvent.socketID);
                }
            }
        }

        networkState.server->Update();
    }
}