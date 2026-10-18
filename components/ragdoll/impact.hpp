#ifndef OPENMW_COMPONENTS_RAGDOLL_IMPACT_HPP
#define OPENMW_COMPONENTS_RAGDOLL_IMPACT_HPP

#include <osg/Vec3f>

namespace Ragdoll
{
    // Presentation selected from effective magical health damage, independently
    // of explosion radius and weapon impulse. None includes ordinary health damage.
    enum class DeathElement
    {
        None,
        Poison,
        Fire,
        Frost,
        Shock
    };

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
        DeathElement mDeathElement = DeathElement::None;
        float mDominantMagicDamage = 0.f;
        bool mShock = false; // Compatibility flag; true only for dominant Shock.
        bool mValid = false;
    };
}

#endif
