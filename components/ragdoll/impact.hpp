#ifndef OPENMW_COMPONENTS_RAGDOLL_IMPACT_HPP
#define OPENMW_COMPONENTS_RAGDOLL_IMPACT_HPP

#include <osg/Vec3f>

namespace Ragdoll
{
    // Transient, world-space provenance of the lethal contact. Distances are metres
    // for a reference-mass body; the renderer converts these to momentum.
    struct Impact
    {
        osg::Vec3f mWeaponDirection;
        osg::Vec3f mMagicDirection;
        osg::Vec3f mPosition;
        float mWeaponDistance = 0.f;
        float mMagicDistance = 0.f;
        bool mCrossbow = false;
        bool mShock = false;
        bool mValid = false;
    };
}

#endif
