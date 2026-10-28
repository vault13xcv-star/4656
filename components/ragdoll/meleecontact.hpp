#ifndef OPENMW_COMPONENTS_RAGDOLL_MELEECONTACT_HPP
#define OPENMW_COMPONENTS_RAGDOLL_MELEECONTACT_HPP

#include <cmath>
#include <osg/ComputeBoundsVisitor>
#include <osg/Transform>

namespace Ragdoll::Combat
{
    // Use the actual attached weapon, including its scale and all parent bones.
    // First-person models with a separate coordinate system use the view ray.
    inline bool weaponSegment(osg::Node& node, const osg::Vec3f& actor, osg::Vec3f& base, osg::Vec3f& tip)
    {
        osg::ComputeBoundsVisitor visitor;
        node.accept(visitor);
        const auto box = visitor.getBoundingBox();
        const auto paths = node.getParentalNodePaths();
        if (!box.valid() || paths.empty())
            return false;
        auto path = paths.front();
        path.pop_back(); // ComputeBoundsVisitor already includes the weapon's own transform.
        const auto matrix = osg::computeLocalToWorld(path);
        const auto size = box._max - box._min;
        int axis = size.y() > size.x() ? 1 : 0;
        if (size.z() > size[axis])
            axis = 2;
        base = tip = box.center();
        base[axis] = box._min[axis];
        tip[axis] = box._max[axis];
        base = base * matrix;
        tip = tip * matrix;
        return std::isfinite(base.length2()) && std::isfinite(tip.length2())
            && (base - actor).length2() < 400.f * 400.f;
    }

    // Bounded shaft/tip sweeps prevent a fast blade tunnelling between frames.
    // Stop at the first accepted contact; there is no proximity/aim cone.
    template <class Ray>
    bool sweepWeapon(const osg::Vec3f& base, const osg::Vec3f& tip,
        const osg::Vec3f& previousBase, const osg::Vec3f& previousTip, bool previous, Ray&& ray)
    {
        if (ray(base, tip))
            return true;
        if (previous)
            for (int i = 0; i <= 4; ++i)
            {
                const float t = i / 4.f;
                if (ray(previousBase * (1 - t) + previousTip * t, base * (1 - t) + tip * t))
                    return true;
            }
        return false;
    }
}
#endif
