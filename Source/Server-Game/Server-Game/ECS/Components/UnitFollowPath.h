#pragma once
#include <Base/Types.h>

#include <Detour/DetourNavMesh.h>
#include <entt/fwd.hpp>

namespace ECS::Components
{
    struct UnitFollowPath
    {
    public:
        entt::entity target;

        dtPolyRef polyRefStart = 0;
        dtPolyRef polyRefEnd = 0;
        dtPolyRef polyRefCurrent = 0;

        vec3 positions[32];
        u32 numPositions = 0;

        u32 currentPositionIndex = 0;
        f32 recalcTimer = 0.0f;
    };
}