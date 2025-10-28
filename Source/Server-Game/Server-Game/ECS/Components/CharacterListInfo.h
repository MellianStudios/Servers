#pragma once
#include <Base/Types.h>

namespace ECS
{
    struct CharacterListEntry
    {
    public:
        std::string name;
        u8 race;
        u8 gender;
        u8 unitClass;
        u16 level;
        u16 mapID;
    };

    namespace Components
    {
        struct CharacterListInfo
        {
        public:
            std::vector<CharacterListEntry> list;
        };
    }
}