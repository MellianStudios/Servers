#include "UpdateCharacter.h"
#include "Server-Game/ECS/Components/AABB.h"
#include "Server-Game/ECS/Components/CharacterInfo.h"
#include "Server-Game/ECS/Components/CharacterSpellCastInfo.h"
#include "Server-Game/ECS/Components/DisplayInfo.h"
#include "Server-Game/ECS/Components/NetInfo.h"
#include "Server-Game/ECS/Components/ObjectInfo.h"
#include "Server-Game/ECS/Components/PlayerContainers.h"
#include "Server-Game/ECS/Components/ProximityTrigger.h"
#include "Server-Game/ECS/Components/Tags.h"
#include "Server-Game/ECS/Components/TargetInfo.h"
#include "Server-Game/ECS/Components/Transform.h"
#include "Server-Game/ECS/Components/UnitAuraInfo.h"
#include "Server-Game/ECS/Components/UnitCombatInfo.h"
#include "Server-Game/ECS/Components/UnitPowersComponent.h"
#include "Server-Game/ECS/Components/UnitResistancesComponent.h"
#include "Server-Game/ECS/Components/UnitSpellCooldownHistory.h"
#include "Server-Game/ECS/Components/UnitStatsComponent.h"
#include "Server-Game/ECS/Components/UnitVisualItems.h"
#include "Server-Game/ECS/Components/VisibilityInfo.h"
#include "Server-Game/ECS/Components/VisibilityUpdateInfo.h"
#include "Server-Game/ECS/Singletons/GameCache.h"
#include "Server-Game/ECS/Singletons/NetworkState.h"
#include "Server-Game/ECS/Singletons/WorldState.h"
#include "Server-Game/ECS/Util/MessageBuilderUtil.h"
#include "Server-Game/ECS/Util/NetFieldsUtil.h"
#include "Server-Game/ECS/Util/UnitUtil.h"
#include "Server-Game/ECS/Util/Cache/CacheUtil.h"
#include "Server-Game/ECS/Util/Network/NetworkUtil.h"
#include "Server-Game/ECS/Util/Persistence/CharacterUtil.h"

#include <Server-Common/Database/DBController.h>
#include <Server-Common/Database/Util/CharacterUtils.h>

#include <Gameplay/ECS/Components/Events.h>
#include <Gameplay/ECS/Components/ObjectFields.h>
#include <Gameplay/ECS/Components/UnitFields.h>

#include <MetaGen/EnumTraits.h>
#include <MetaGen/Server/Lua/Lua.h>
#include <MetaGen/Shared/NetField/NetField.h>
#include <MetaGen/Shared/Packet/Packet.h>
#include <MetaGen/Shared/Unit/Unit.h>

#include <Scripting/Zenith.h>

namespace ECS::Systems
{
    void UpdateCharacter::HandleDeinitialization(Singletons::WorldState& worldState, World& world, Scripting::Zenith* zenith, Singletons::GameCache& gameCache, Singletons::NetworkState& networkState)
    {
        auto view = world.View<Components::ObjectInfo, Components::NetInfo, Events::CharacterNeedsDeinitialization>();
        view.each([&](entt::entity entity, Components::ObjectInfo& objectInfo, Components::NetInfo& netInfo)
        {
            u64 characterID = objectInfo.guid.GetCounter();

            bool hasCharacterInfo = world.AllOf<Components::CharacterInfo>(entity);
            bool hasWorldTransfer = world.AllOf<Events::CharacterWorldTransfer>(entity);

            if (!hasCharacterInfo || !hasWorldTransfer)
            {
                zenith->CallEvent(MetaGen::Server::Lua::CharacterEvent::OnLogout, MetaGen::Server::Lua::CharacterEventDataOnLogout{
                    .characterEntity = entt::to_integral(entity)
                });
            }

            if (auto* playerContainers = world.TryGet<Components::PlayerContainers>(entity))
            {
                for (const auto& [bagIndex, bagContainer] : playerContainers->bags)
                {
                    u16 numContainerSlots = bagContainer.GetTotalSlots();
                    for (u16 containerSlot = 0; containerSlot < numContainerSlots; containerSlot++)
                    {
                        const Database::ContainerItem& item = bagContainer.GetItem(containerSlot);
                        if (item.IsEmpty())
                            continue;

                        u64 itemInstanceID = item.objectGUID.GetCounter();
                        Util::Cache::ItemInstanceDelete(gameCache, itemInstanceID);
                    }
                }

                u16 numEquipmentSlots = playerContainers->equipment.GetTotalSlots();
                for (u16 equipmentSlot = 0; equipmentSlot < numEquipmentSlots; equipmentSlot++)
                {
                    const Database::ContainerItem& item = playerContainers->equipment.GetItem(equipmentSlot);
                    if (item.IsEmpty())
                        continue;

                    u64 itemInstanceID = item.objectGUID.GetCounter();
                    Util::Cache::ItemInstanceDelete(gameCache, itemInstanceID);
                }
            }

            // Network Cleanup
            {
                Util::Network::UnlinkSocketFromCharacter(networkState, netInfo.socketID, characterID);
            }

            // Grid Cleanup
            if (auto* transform = world.TryGet<Components::Transform>(entity))
            {
                f32 orientation = transform->pitchYaw.y;
                Util::Persistence::Character::CharacterSetMapIDPositionOrientation(gameCache, characterID, transform->mapID, transform->position, orientation);

                world.RemoveEntity(objectInfo.guid);
            }

            if (hasCharacterInfo)
            {
                auto& characterInfo = world.Get<Components::CharacterInfo>(entity);
                if (hasWorldTransfer)
                {
                    auto& worldTransfter = world.Get<Events::CharacterWorldTransfer>(entity);

                    worldState.AddWorldTransferRequest(WorldTransferRequest{
                        .socketID = netInfo.socketID,

                        .characterName = characterInfo.name,
                        .characterID = characterID,

                        .targetMapID = worldTransfter.targetMapID,
                        .targetPosition = worldTransfter.targetPosition,
                        .targetOrientation = worldTransfter.targetOrientation,
                        .useTargetPosition = true
                    });
                }

                Util::Cache::CharacterDelete(gameCache, characterID, characterInfo);
            }

            world.DestroyEntity(entity);
        });

        world.Clear<Events::CharacterNeedsDeinitialization>();
    }
    void UpdateCharacter::HandleInitialization(World& world, Scripting::Zenith* zenith, Singletons::GameCache& gameCache, Singletons::NetworkState& networkState)
    {
        auto databaseConn = gameCache.dbController->GetConnection(::Database::DBType::Character);

        std::shared_ptr<Bytebuffer> buffer = Bytebuffer::Borrow<8192>();

        auto view = world.View<Components::ObjectInfo, Components::NetInfo, Events::CharacterNeedsInitialization>();
        view.each([&](entt::entity entity, Components::ObjectInfo& objectInfo, Components::NetInfo& netInfo)
        {
            u64 characterID = objectInfo.guid.GetCounter();
            pqxx::result databaseResult;

            if (!Database::Util::Character::CharacterGetInfoByID(databaseConn, characterID, databaseResult))
            {
                Util::Network::RequestSocketToClose(networkState, netInfo.socketID);
                return;
            }

            world.Emplace<Tags::IsPlayer>(entity);
            auto& characterInfo = world.Emplace<Components::CharacterInfo>(entity);
            characterInfo.name = databaseResult[0][2].as<std::string>();
            characterInfo.accountID = databaseResult[0][1].as<u32>();
            characterInfo.totalTime = databaseResult[0][3].as<u32>();
            characterInfo.levelTime = databaseResult[0][4].as<u32>();
            characterInfo.logoutTime = databaseResult[0][5].as<u32>();
            characterInfo.flags = databaseResult[0][6].as<u32>();
            characterInfo.level = databaseResult[0][8].as<u16>();
            characterInfo.experiencePoints = databaseResult[0][9].as<u64>();

            auto& transform = world.Emplace<Components::Transform>(entity);
            transform.mapID = databaseResult[0][10].as<u16>();
            transform.position = vec3(databaseResult[0][11].as<f32>(), databaseResult[0][12].as<f32>(), databaseResult[0][13].as<f32>());

            f32 orientation = databaseResult[0][14].as<f32>();
            transform.pitchYaw = vec2(0.0f, orientation);

            auto& aabb = world.Emplace<Components::AABB>(entity);
            aabb.extents = vec3(3.0f, 5.0f, 2.0f); // TODO: Create proper data for this

            world.Emplace<Components::VisibilityInfo>(entity);
            auto& visibilityUpdateInfo = world.Emplace<Components::VisibilityUpdateInfo>(entity);
            visibilityUpdateInfo.updateInterval = 0.25f;
            visibilityUpdateInfo.timeSinceLastUpdate = visibilityUpdateInfo.updateInterval;

            auto& displayInfo = world.Emplace<Components::DisplayInfo>(entity);
            auto& visualItems = world.Emplace<Components::UnitVisualItems>(entity);

            u16 raceGenderClass = databaseResult[0][7].as<u16>();
            auto unitRace = static_cast<GameDefine::UnitRace>(raceGenderClass & 0x7F);
            auto unitGender = static_cast<GameDefine::UnitGender>((raceGenderClass >> 7) & 0x3);
            auto unitClass = static_cast<GameDefine::UnitClass>((raceGenderClass >> 9) & 0x7F);

            characterInfo.unitClass = unitClass;

            Util::Unit::AddPowersComponent(world, entity, unitClass);
            auto& unitResistancesComponent = Util::Unit::AddResistancesComponent(world, entity);
            auto& unitStatsComponent = Util::Unit::AddStatsComponent(world, entity);

            world.Emplace<Tags::IsAlive>(entity);
            world.Emplace<Components::UnitSpellCooldownHistory>(entity);
            world.Emplace<Components::UnitCombatInfo>(entity);
            world.Emplace<Components::UnitAuraInfo>(entity);
            auto& targetInfo = world.Emplace<Components::TargetInfo>(entity);
            targetInfo.target = entt::null;

            auto& characterSpellCastInfo = world.Emplace<Components::CharacterSpellCastInfo>(entity);
            characterSpellCastInfo.activeSpellEntity = entt::null;
            characterSpellCastInfo.queuedSpellEntity = entt::null;

            auto& playerContainers = world.Emplace<Components::PlayerContainers>(entity);

            auto& objectFields = world.Emplace<Components::ObjectFields>(entity);
            objectFields.fields.SetField(MetaGen::Shared::NetField::ObjectNetFieldEnum::ObjectGUIDLow, objectInfo.guid);
            objectFields.fields.SetField(MetaGen::Shared::NetField::ObjectNetFieldEnum::Scale, 1.0f);

            auto& unitFields = world.Emplace<Components::UnitFields>(entity);

            constexpr u8 unitClassBytesOffset = (u8)MetaGen::Shared::NetField::UnitLevelRaceGenderClassPackedInfoEnum::ClassByteOffset;
            constexpr u8 unitClassBitOffset = (u8)MetaGen::Shared::NetField::UnitLevelRaceGenderClassPackedInfoEnum::ClassBitOffset;
            constexpr u8 unitClassBitSize = (u8)MetaGen::Shared::NetField::UnitLevelRaceGenderClassPackedInfoEnum::ClassBitSize;
            unitFields.fields.SetField(MetaGen::Shared::NetField::UnitNetFieldEnum::LevelRaceGenderClassPacked, characterInfo.level);
            unitFields.fields.SetField(MetaGen::Shared::NetField::UnitNetFieldEnum::LevelRaceGenderClassPacked, characterInfo.unitClass, unitClassBytesOffset, unitClassBitOffset, unitClassBitSize);
            Util::Unit::UpdateDisplayRaceGender(*world.registry, entity, unitFields, unitRace, unitGender);

            buffer->Reset();
            bool failed = false;

            failed |= !Util::MessageBuilder::Unit::BuildUnitAdd(buffer, objectInfo.guid, characterInfo.name, unitClass, transform.position, transform.scale, transform.pitchYaw);
            failed |= !Util::Network::AppendPacketToBuffer(buffer, MetaGen::Shared::Packet::ServerUnitSetMoverPacket{
                .guid = objectInfo.guid
            });
            failed |= !Util::MessageBuilder::Unit::BuildUnitBaseInfo(buffer, *world.registry, entity, objectInfo.guid);
            
            for (auto& pair : unitResistancesComponent.resistanceTypeToValue)
            {
                failed |= !Util::Network::AppendPacketToBuffer(buffer, MetaGen::Shared::Packet::ServerUnitResistanceUpdatePacket{
                    .kind = static_cast<u8>(pair.first),
                    .base = pair.second.base,
                    .current = pair.second.current,
                    .max = pair.second.max
                });
            }
            world.EmplaceOrReplace<Events::CharacterNeedsRecalculateStatsUpdate>(entity);

            if (Database::Util::Character::CharacterGetItems(databaseConn, characterID, databaseResult))
            {
                databaseResult.for_each([&](u64 itemInstanceID, u32 itemID, u64 ownerID, u16 count, u16 durability)
                {
                    Database::ItemInstance itemInstance =
                    {
                        .id = itemInstanceID,
                        .ownerID = ownerID,
                        .itemID = itemID,
                        .count = count,
                        .durability = durability
                    };

                    Util::Cache::ItemInstanceCreate(gameCache, itemInstanceID, itemInstance);
                });

                if (Database::Util::Character::CharacterGetItemsInContainer(databaseConn, characterID, PLAYER_BASE_CONTAINER_ID, databaseResult))
                {
                    databaseResult.for_each([&](u64 characterID, u64 containerID, u16 slotIndex, u64 itemInstanceID)
                    {
                        Database::ItemInstance* itemInstance = nullptr;
                        if (!Util::Cache::GetItemInstanceByID(gameCache, itemInstanceID, itemInstance))
                            return;

                        ObjectGUID itemGUID = ObjectGUID::CreateItem(itemInstanceID);
                        playerContainers.equipment.AddItemToSlot(itemGUID, slotIndex);

                        if (slotIndex >= PLAYER_BAG_INDEX_START)
                            return;

                        failed |= !Util::Network::AppendPacketToBuffer(buffer, MetaGen::Shared::Packet::ServerItemAddPacket{
                            .guid = itemGUID,
                            .itemID = itemInstance->itemID,
                            .count = itemInstance->count,
                            .durability = itemInstance->durability
                        });
                    });

                    for (ItemEquipSlot_t bagIndex = PLAYER_BAG_INDEX_START; bagIndex <= PLAYER_BAG_INDEX_END; bagIndex++)
                    {
                        if (playerContainers.equipment.IsSlotEmpty(bagIndex))
                            continue;

                        ObjectGUID containerGUID = playerContainers.equipment.GetItem(bagIndex).objectGUID;
                        u64 containerID = containerGUID.GetCounter();

                        Database::ItemInstance* containerItemInstance = nullptr;
                        if (!Util::Cache::GetItemInstanceByID(gameCache, containerID, containerItemInstance))
                            continue;

                        playerContainers.bags.emplace(bagIndex, Database::Container(containerItemInstance->durability));
                        Database::Container& container = playerContainers.bags[bagIndex];

                        if (Database::Util::Character::CharacterGetItemsInContainer(databaseConn, characterID, containerID, databaseResult))
                        {
                            databaseResult.for_each([&](u64 characterID, u64 containerID, u16 slotIndex, u64 itemInstanceID)
                            {
                                Database::ItemInstance* itemInstance = nullptr;
                                if (!Util::Cache::GetItemInstanceByID(gameCache, itemInstanceID, itemInstance))
                                    return;

                                ObjectGUID itemGUID = ObjectGUID::CreateItem(itemInstanceID);
                                container.AddItemToSlot(itemGUID, slotIndex);

                                failed |= !Util::Network::AppendPacketToBuffer(buffer, MetaGen::Shared::Packet::ServerItemAddPacket{
                                    .guid = itemGUID,
                                    .itemID = itemInstance->itemID,
                                    .count = itemInstance->count,
                                    .durability = itemInstance->durability
                                });
                            });
                        }
                    }
                }

                bool hasDirtyVisualItems = false;
                for (ItemEquipSlot_t i = static_cast<ItemEquipSlot_t>(MetaGen::Shared::Unit::ItemEquipSlotEnum::EquipmentStart); i <= static_cast<ItemEquipSlot_t>(MetaGen::Shared::Unit::ItemEquipSlotEnum::EquipmentEnd); i++)
                {
                    const auto& item = playerContainers.equipment.GetItem(static_cast<ItemEquipSlot_t>(i));
                    if (item.IsEmpty())
                        continue;

                    ObjectGUID itemGUID = item.objectGUID;

                    Database::ItemInstance* itemInstance = nullptr;
                    if (!Util::Cache::GetItemInstanceByID(gameCache, itemGUID.GetCounter(), itemInstance))
                        continue;

                    visualItems.equippedItemIDs[i] = itemInstance->itemID;
                    visualItems.dirtyItemIDs.insert(i);

                    hasDirtyVisualItems = true;
                }

                if (hasDirtyVisualItems)
                    world.EmplaceOrReplace<Events::CharacterNeedsVisualItemUpdate>(entity);
            }

            world.EmplaceOrReplace<Events::CharacterNeedsContainerUpdate>(entity);

            // Send Proximity Triggers
            {
                auto view = world.View<Components::ProximityTrigger, Components::Transform, Components::AABB, Tags::ProximityTriggerIsClientSide>();
                view.each([&](entt::entity entity, Components::ProximityTrigger& proximityTrigger, Components::Transform& transform, Components::AABB& aabb)
                {
                    failed |= !Util::Network::AppendPacketToBuffer(buffer, MetaGen::Shared::Packet::ServerTriggerAddPacket{
                        .triggerID = proximityTrigger.triggerID,

                        .name = proximityTrigger.name,
                        .flags = static_cast<u8>(proximityTrigger.flags),

                        .mapID = transform.mapID,
                        .position = transform.position,
                        .extents = aabb.extents
                    });
                });
            }

            if (failed)
            {
                NC_LOG_ERROR("Failed to build character initialization message for character: {0}", characterID);
                networkState.server->CloseSocketID(netInfo.socketID);
                return;
            }

            world.AddEntity(objectInfo.guid, entity, vec2(transform.position.x, transform.position.z));
            Util::Network::SendPacket(networkState, netInfo.socketID, buffer);

            if (world.AllOf<Tags::CharacterWasWorldTransferred>(entity))
            {
                zenith->CallEvent(MetaGen::Server::Lua::CharacterEvent::OnWorldChanged, MetaGen::Server::Lua::CharacterEventDataOnWorldChanged{
                    .characterEntity = entt::to_integral(entity)
                });

                world.Remove<Tags::CharacterWasWorldTransferred>(entity);
            }
            else
            {
                zenith->CallEvent(MetaGen::Server::Lua::CharacterEvent::OnLogin, MetaGen::Server::Lua::CharacterEventDataOnLogin{
                    .characterEntity = entt::to_integral(entity)
                });
            }
        });
    }

    void UpdateCharacter::HandleUpdate(Singletons::WorldState& worldState, World& world, Scripting::Zenith* zenith, Singletons::GameCache& gameCache, Singletons::NetworkState& networkState, f32 deltaTime)
    {
        HandleDeinitialization(worldState, world, zenith, gameCache, networkState);
        HandleInitialization(world, zenith, gameCache, networkState);

        auto spellCooldownView = world.View<Components::UnitSpellCooldownHistory, Tags::IsPlayer>();
        spellCooldownView.each([&](entt::entity entity, Components::UnitSpellCooldownHistory& cooldownHistory)
        {
            for (auto& [spellID, cooldown] : cooldownHistory.spellIDToCooldown)
            {
                cooldown = glm::max(0.0f, cooldown - deltaTime);
            }
        });

        auto recalculateStatsView = world.View<Components::CharacterInfo, Components::PlayerContainers, Components::UnitPowersComponent, Components::UnitStatsComponent, Events::CharacterNeedsRecalculateStatsUpdate>();
        recalculateStatsView.each([&](entt::entity entity, Components::CharacterInfo& characterInfo, Components::PlayerContainers& playerContainers, Components::UnitPowersComponent& unitPowersComponent, Components::UnitStatsComponent& unitStatsComponent)
        {
            std::shared_ptr<Bytebuffer> buffer = Bytebuffer::Borrow<512>();
            // Default stats back to base
            for (auto& pair : unitStatsComponent.statTypeToValue)
            {
                UnitStat& unitStat = pair.second;
                unitStat.current = unitStat.base;

                unitStatsComponent.dirtyStatTypes.insert(pair.first);
            }

            // Apply item stats
            for (ItemEquipSlot_t i = static_cast<ItemEquipSlot_t>(MetaGen::Shared::Unit::ItemEquipSlotEnum::EquipmentStart); i <= static_cast<ItemEquipSlot_t>(MetaGen::Shared::Unit::ItemEquipSlotEnum::EquipmentEnd); i++)
            {
                const auto& item = playerContainers.equipment.GetItem(static_cast<ItemEquipSlot_t>(i));
                if (item.IsEmpty())
                    continue;

                ObjectGUID itemGUID = item.objectGUID;

                Database::ItemInstance* itemInstance = nullptr;
                if (!Util::Cache::GetItemInstanceByID(gameCache, itemGUID.GetCounter(), itemInstance))
                    continue;

                GameDefine::Database::ItemTemplate* itemTemplate = nullptr;
                if (!Util::Cache::GetItemTemplateByID(gameCache, itemInstance->itemID, itemTemplate))
                    continue;

                UnitStat& armorStat = Util::Unit::GetStat(unitStatsComponent, MetaGen::Shared::Unit::StatTypeEnum::Armor);
                armorStat.current += itemTemplate->armor;

                GameDefine::Database::ItemStatTemplate* itemStatTemplate = nullptr;
                if (itemTemplate->statTemplateID > 0 && Util::Cache::GetItemStatTemplateByID(gameCache, itemTemplate->statTemplateID, itemStatTemplate))
                {
                    for (u8 statIndex = 0; statIndex < 8; statIndex++)
                    {
                        u8 statType = itemStatTemplate->statTypes[statIndex];
                        i32 statValue = itemStatTemplate->statValues[statIndex];

                        if (statType == 0 || statValue == 0)
                            continue;

                        auto statTypeEnum = static_cast<MetaGen::Shared::Unit::StatTypeEnum>(statType);
                        UnitStat& unitStat = Util::Unit::GetStat(unitStatsComponent, statTypeEnum);
                        unitStat.current += statValue;
                    }
                }

                GameDefine::Database::ItemArmorTemplate* itemArmorTemplate = nullptr;
                if (itemTemplate->armorTemplateID > 0 && Util::Cache::GetItemArmorTemplateByID(gameCache, itemTemplate->armorTemplateID, itemArmorTemplate))
                {
                    armorStat.current += itemArmorTemplate->bonusArmor;
                }

                GameDefine::Database::ItemShieldTemplate* itemShieldTemplate = nullptr;
                if (itemTemplate->shieldTemplateID > 0 && Util::Cache::GetItemShieldTemplateByID(gameCache, itemTemplate->shieldTemplateID, itemShieldTemplate))
                {
                    armorStat.current += itemShieldTemplate->bonusArmor;
                }
            }

            for (MetaGen::Shared::Unit::StatTypeEnum dirtyStatType : unitStatsComponent.dirtyStatTypes)
            {
                UnitStat& unitStat = Util::Unit::GetStat(unitStatsComponent, dirtyStatType);

                Util::Network::AppendPacketToBuffer(buffer, MetaGen::Shared::Packet::ServerUnitStatUpdatePacket{
                    .kind = static_cast<u8>(dirtyStatType),
                    .base = unitStat.base,
                    .current = unitStat.current
                });
            }

            // Calculate New Health
            {
                UnitStat& healthStat = Util::Unit::GetStat(unitStatsComponent, MetaGen::Shared::Unit::StatTypeEnum::Health);
                UnitStat& staminaStat = Util::Unit::GetStat(unitStatsComponent, MetaGen::Shared::Unit::StatTypeEnum::Stamina);

                UnitPower& healthPower = Util::Unit::GetPower(unitPowersComponent, MetaGen::Shared::Unit::PowerTypeEnum::Health);

                f64 baseHealth = healthStat.current;
                f64 maxHealth = healthPower.base + (staminaStat.current * 10.0);
                f64 currentHealth = glm::min(healthPower.current, maxHealth);

                if (world.AllOf<Events::CharacterNeedsInitialization>(entity))
                    currentHealth = maxHealth;

                Util::Unit::SetPower(world, entity, unitPowersComponent, MetaGen::Shared::Unit::PowerTypeEnum::Health, baseHealth, currentHealth, maxHealth);
            }

            switch (characterInfo.unitClass)
            {
                case GameDefine::UnitClass::Warrior:
                case GameDefine::UnitClass::Paladin:
                {
                    UnitStat& strengthStat = Util::Unit::GetStat(unitStatsComponent, MetaGen::Shared::Unit::StatTypeEnum::Strength);
                    UnitStat& attackPowerStat = Util::Unit::GetStat(unitStatsComponent, MetaGen::Shared::Unit::StatTypeEnum::AttackPower);

                    attackPowerStat.current += strengthStat.current * 2.0;
                    break;
                }

                case GameDefine::UnitClass::Hunter:
                case GameDefine::UnitClass::Rogue:
                {
                    UnitStat& agilityStat = Util::Unit::GetStat(unitStatsComponent, MetaGen::Shared::Unit::StatTypeEnum::Agility);
                    UnitStat& attackPowerStat = Util::Unit::GetStat(unitStatsComponent, MetaGen::Shared::Unit::StatTypeEnum::AttackPower);

                    attackPowerStat.current += agilityStat.current * 2.0;
                    break;
                }

                case GameDefine::UnitClass::Priest:
                case GameDefine::UnitClass::Shaman:
                case GameDefine::UnitClass::Mage:
                case GameDefine::UnitClass::Warlock:
                case GameDefine::UnitClass::Druid:
                {
                    UnitStat& intellectStat = Util::Unit::GetStat(unitStatsComponent, MetaGen::Shared::Unit::StatTypeEnum::Intellect);
                    UnitStat& spellPowerStat = Util::Unit::GetStat(unitStatsComponent, MetaGen::Shared::Unit::StatTypeEnum::SpellPower);

                    spellPowerStat.current += intellectStat.current * 2.0;
                    break;
                }
            }

            world.EmplaceOrReplace<Events::CharacterNeedsStatUpdate>(entity);
        });
        world.Clear<Events::CharacterNeedsRecalculateStatsUpdate>();

        world.Clear<Events::CharacterNeedsInitialization>();
    }
}