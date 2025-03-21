#pragma once
#include <Base/Types.h>

#include <Gameplay/GameDefine.h>

namespace ECS::Components
{
    struct DisplayInfo
    {
    public:
        u32 displayID;

        GameDefine::UnitRace race = GameDefine::UnitRace::None;
        GameDefine::Gender gender = GameDefine::Gender::None;
    };
}