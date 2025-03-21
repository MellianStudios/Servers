#pragma once
#include <Base/Types.h>
#include <Base/Util/DebugHandler.h>

#include <Gameplay/GameDefine.h>

#include <robinhood/robinhood.h>

namespace Database
{
    static constexpr u32 CHARACTER_BASE_CONTAINER_SIZE = 24;

    struct CharacterDefinition
    {
    public:
        u64 id = 0;
        u32 accountid = 0;
        std::string name = "";
        u32 totalTime = 0;
        u32 levelTime = 0;
        u32 logoutTime = 0;
        u32 flags = 0;
        u16 raceGenderClass = 0;
        u16 level = 0;
        u64 experiencePoints = 0;
        u16 mapID = 0;
        vec3 position = vec3(0.0f);
        f32 orientation = 0.0f;

    public:
        GameDefine::UnitRace GetRace() const { return static_cast<GameDefine::UnitRace>(raceGenderClass & 0x7F); }
        void SetRace(GameDefine::UnitRace race) { raceGenderClass = (raceGenderClass & 0xFF80) | static_cast<u16>(race); }

        GameDefine::Gender GetGender() const { return static_cast<GameDefine::Gender>((raceGenderClass >> 7) & 0x3); }
        void SetGender(GameDefine::Gender gender) { raceGenderClass = (raceGenderClass & 0xFE7F) | (static_cast<u16>(gender) << 7); }

        GameDefine::UnitClass GetGameClass() const { return static_cast<GameDefine::UnitClass>((raceGenderClass >> 9) & 0x7f); }
        void SetGameClass(GameDefine::UnitClass gameClass) { raceGenderClass = (raceGenderClass & 0x1FF) | (static_cast<u16>(gameClass) << 9); }
    };
    struct CharacterCurrency
    {
    public:
        u16 currencyID = 0;
        u64 value = 0;
    };

    struct Permission
    {
    public:
        u16 id = 0;
        std::string name;
    };
    struct PermissionGroup
    {
    public:
        u16 id = 0;
        std::string name;
    };
    struct PermissionGroupData
    {
    public:
        u16 groupID = 0;
        u16 permissionID = 0;
    };

    struct Currency
    {
    public:
        u16 id = 0;
        std::string name;
    };

    struct ContainerItem
    {
    public:
        bool IsEmpty() const { return !objectGuid.IsValid(); }
        void Clear()
        {
            objectGuid = GameDefine::ObjectGuid::Empty;
        }

    public:
        GameDefine::ObjectGuid objectGuid = GameDefine::ObjectGuid::Empty;
    };

    struct Container
    {
    public:
        Container() = default;
        Container(u8 numSlots) : _slots(numSlots), _freeSlots(numSlots)
        {
            _items.resize(numSlots);
        }

        /**
        * Finds the next available free slot in the container
        * @return Index of next free slot, or INVALID_SLOT if container is full
        */
        u16 GetNextFreeSlot() const
        {
            if (IsFull()) return INVALID_SLOT;

            for (u8 i = 0; i < _slots; ++i)
            {
                if (_items[i].IsEmpty())
                    return i;
            }

            return INVALID_SLOT;
        }

        /**
        * Checks if a specific slot is empty
        * @param slotIndex The slot to check
        * @return true if the slot is empty
        */
        bool IsSlotEmpty(u8 slotIndex) const
        {
#if defined(NC_DEBUG)
            NC_ASSERT(slotIndex < _slots, "Container::IsSlotEmpty - Called with slotIndex ({0}) when the container can only hold {1} items", slotIndex, _slots);
#endif
            if (slotIndex >= _slots)
                return true;

            return _items[slotIndex].IsEmpty();
        }

        /**
        * Attempts to add an item to the next available slot
        * @param itemGuid The item to add
        * @return Slot where item was added, or INVALID_SLOT if container is full
        */
        u16 AddItem(GameDefine::ObjectGuid itemGuid)
        {
            u16 slot = GetNextFreeSlot();
            if (slot == INVALID_SLOT)
                return INVALID_SLOT;

            return AddItemToSlot(itemGuid, static_cast<u8>(slot));
        }

        /**
        * Attempts to add an item to a specific slot
        * @param itemGuid The item to add
        * @param slotIndex The desired slot
        * @return The slot where item was added, or INVALID_SLOT if slot was occupied/invalid
        */
        u16 AddItemToSlot(GameDefine::ObjectGuid itemGuid, u8 slotIndex)
        {
#if defined(NC_DEBUG)
            NC_ASSERT(slotIndex < _slots, "Container::AddItemToSlot - Called with slotIndex ({0}) when the container can only hold {1} items", slotIndex, _slots);
#endif
            if (slotIndex >= _slots || !_items[slotIndex].IsEmpty())
                return INVALID_SLOT;

            _items[slotIndex] = { itemGuid };
            --_freeSlots;
            return slotIndex;
        }

        /**
        * Removes an item from a specific slot
        * @param slotIndex The slot to remove from
        * @return The GUID of the removed item, or empty GUID if slot was already empty
        */
        bool RemoveItem(u8 slotIndex)
        {
#if defined(NC_DEBUG)
            NC_ASSERT(slotIndex < _slots, "Container::RemoveItem - Called with slotIndex ({0}) when the container can only hold {1} items", slotIndex, _slots);
#endif
            if (slotIndex >= _slots || _items[slotIndex].IsEmpty())
                return false;

            _items[slotIndex].Clear();
            ++_freeSlots;
            return true;
        }

        bool SwapItems(u8 srcSlotIndex, u8 destSlotIndex)
        {
#if defined(NC_DEBUG)
            NC_ASSERT(srcSlotIndex < _slots, "Container::SwapItems - Called with srcSlotIndex ({0}) when the container can only hold {1} items", srcSlotIndex, _slots);
            NC_ASSERT(destSlotIndex < _slots, "Container::SwapItems - Called with destSlotIndex ({0}) when the container can only hold {1} items", destSlotIndex, _slots);
#endif
            if (srcSlotIndex >= _slots || destSlotIndex >= _slots)
                return false;

            std::swap(_items[srcSlotIndex], _items[destSlotIndex]);
            return true;
        }

        bool SwapItems(Container& destContainer, u8 srcSlotIndex, u8 destSlotIndex)
        {
            u8 numSrcSlots = _slots;
            u8 numDestSlots = destContainer._slots;
#if defined(NC_DEBUG)
            NC_ASSERT(srcSlotIndex < numSrcSlots, "Container::SwapItems - Called with srcSlotIndex ({0}) when the container can only hold {1} items", srcSlotIndex, numSrcSlots);
            NC_ASSERT(destSlotIndex < numDestSlots, "Container::SwapItems - Called with destSlotIndex ({0}) when the container can only hold {1} items", destSlotIndex, numDestSlots);
#endif
            if (srcSlotIndex >= numSrcSlots || destSlotIndex >= numDestSlots)
                return false;
            
            bool srcSlotEmpty = _items[srcSlotIndex].IsEmpty();
            bool destSlotEmpty = destContainer._items[destSlotIndex].IsEmpty();
            std::swap(_items[srcSlotIndex], destContainer._items[destSlotIndex]);

            if (srcSlotEmpty && !destSlotEmpty)
            {
                --_freeSlots;
                ++destContainer._freeSlots;
            }
            else if (!srcSlotEmpty && destSlotEmpty)
            {
                ++_freeSlots;
                --destContainer._freeSlots;
            }

            return true;
        }

        /**
        * Gets the item GUID at the specified slot
        * @param slotIndex The slot to check
        * @return The GUID of the item in the slot
        */
        ContainerItem& GetItem(u8 slotIndex)
        {
#if defined(NC_DEBUG)
            NC_ASSERT(slotIndex < _slots, "Container::GetItemGuid - Called with slotIndex ({0}) when the container can only hold {1} items", slotIndex, _slots);
#endif
            return _items[slotIndex];
        }

        /**
        * Gets the backing vector for all items
        * @return const std::vector& with all ContainerItem
        */
        const std::vector<ContainerItem>& GetItems() const { return _items; }

        /**
        * Checks if the container is full
        * @return true if no slots are available
        */
        bool IsFull() const { return _freeSlots == 0; }

        /**
        * Checks if the container is empty
        * @return true if all slots are available
        */
        bool IsEmpty() const { return _freeSlots == _slots; }

        /**
         * Gets the total number of slots in the container
         * @return Total slot count
         */
        u8 GetTotalSlots() const { return _slots; }

        /**
        * Gets the number of free slots in the container
        * @return Number of available slots
        */
        u8 GetFreeSlots() const { return _freeSlots; }

    public:
        static constexpr u16 INVALID_SLOT = 0xFFFF;

    private:
        u8 _slots = 0;
        u8 _freeSlots = 0;

        std::vector<ContainerItem> _items;
    };

    struct ItemInstance
    {
    public:
        u64 id = 0;
        u64 ownerID = 0;
        u32 itemID = 0;
        u16 count = 0;
        u16 durability = 0;
    };

    struct CharacterItem
    {
    public:
        u64 charid = 0;
        u64 itemInstanceId = 0;
        u16 containerID = 0;
        u16 slot = 0;
    };

    struct PermissionTables
    {
    public:
        robin_hood::unordered_map<u16, Database::Permission> idToDefinition;
        robin_hood::unordered_map<u16, Database::PermissionGroup> groupIDToDefinition;
        robin_hood::unordered_map<u16, std::vector<Database::PermissionGroupData>> groupIDToData;
    };

    struct CurrencyTables
    {
    public:
        robin_hood::unordered_map<u16, Database::Currency> idToDefinition;
    };

    struct ItemTables
    {
    public:
        robin_hood::unordered_map<u32, GameDefine::Database::ItemTemplate> templateIDToTemplateDefinition;
        robin_hood::unordered_map<u32, GameDefine::Database::ItemStatTemplate> statTemplateIDToTemplateDefinition;
        robin_hood::unordered_map<u32, GameDefine::Database::ItemArmorTemplate> armorTemplateIDToTemplateDefinition;
        robin_hood::unordered_map<u32, GameDefine::Database::ItemWeaponTemplate> weaponTemplateIDToTemplateDefinition;
        robin_hood::unordered_map<u32, GameDefine::Database::ItemShieldTemplate> shieldTemplateIDToTemplateDefinition;
        robin_hood::unordered_map<u64, ItemInstance> itemInstanceIDToDefinition;
        robin_hood::unordered_map<u64, Container> itemInstanceIDToContainer;
    };

    struct CharacterTables
    {
    public:
        robin_hood::unordered_map<u64, Database::CharacterDefinition> charIDToDefinition;
        robin_hood::unordered_map<u32, u64> charNameHashToCharID;
        robin_hood::unordered_map<u64, std::vector<u16>> charIDToPermissions;
        robin_hood::unordered_map<u64, std::vector<u16>> charIDToPermissionGroups;
        robin_hood::unordered_map<u64, std::vector<CharacterCurrency>> charIDToCurrency;
        robin_hood::unordered_map<u64, Container> charIDToBaseContainer;
    };
}