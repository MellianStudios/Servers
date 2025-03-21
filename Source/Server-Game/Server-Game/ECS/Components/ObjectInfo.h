#pragma once
#include <Base/Types.h>

#include <Gameplay/GameDefine.h>
#include <Network/Define.h>

#include <entt/fwd.hpp>

namespace ECS::Components
{
    struct ObjectInfo
    {
    public:
        GameDefine::ObjectGuid guid = GameDefine::ObjectGuid::Empty;
    };
}