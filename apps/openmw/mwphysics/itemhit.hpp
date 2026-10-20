#ifndef OPENMW_MWPHYSICS_ITEMHIT_H
#define OPENMW_MWPHYSICS_ITEMHIT_H

#include "../mwworld/class.hpp"
#include "raycasting.hpp"
#include <array>
#include <cmath>
#include <limits>

namespace MWPhysics
{
    inline RayCastingResult itemMeleeContact(const RayCastingInterface& physics, const osg::Vec3f& origin,
        osg::Vec3f direction, float reach, const MWWorld::ConstPtr& attacker = {})
    {
        RayCastingResult best{};
        if (!std::isfinite(reach) || reach <= 0 || !std::isfinite(direction.length2()) || direction.normalize() == 0)
            return best;
        osg::Vec3f right = direction ^ osg::Vec3f(0, 0, 1);
        if (right.normalize() < 0.001f)
            right.set(1, 0, 0);
        const osg::Vec3f up = right ^ direction;
        constexpr std::array<std::array<float, 2>, 9> offsets{ { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
            { .7071f, .7071f }, { -.7071f, .7071f }, { .7071f, -.7071f }, { -.7071f, -.7071f } } };
        float distance = std::numeric_limits<float>::max();
        for (const auto& offset : offsets)
        {
            osg::Vec3f delta = direction * reach + (right * offset[0] + up * offset[1]) * 6.f;
            delta.normalize();
            // Each sample stops at its first physical obstruction. No search
            // through furniture/walls and no hit-chance roll for inanimate things.
            const auto hit = physics.castRay(origin, origin + delta * reach, { attacker }, {},
                CollisionType_World | CollisionType_HeightMap | CollisionType_Door | CollisionType_VisualOnly
                    | CollisionType_Actor);
            if (!hit.mHit || hit.mHitObject.isEmpty() || !hit.mHitObject.getClass().isItem(hit.mHitObject))
                continue;
            const float d = (hit.mHitPos - origin).length2();
            if (d < distance)
            {
                distance = d;
                best = hit;
            }
        }
        return best;
    }
}
#endif
