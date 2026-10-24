#ifndef OPENMW_COMPONENTS_RAGDOLL_IMPACT_HPP
#define OPENMW_COMPONENTS_RAGDOLL_IMPACT_HPP

#include <algorithm>
#include <cmath>
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

    enum class WoundWeapon { None, Blade, HeavyBlade, Blunt, Bolt };
    // Native animation hit key, independent of the direction of the death impulse.
    enum class AttackType { Unknown, Chop, Slash, Thrust, Projectile };

    // Transient, world-space provenance of the lethal contact. Distances are metres
    // for a reference-mass body; the renderer converts these to momentum.
    struct Impact
    {
        osg::Vec3f mWeaponDirection;
        osg::Vec3f mMagicDirection;
        osg::Vec3f mPosition;
        osg::Vec3f mWeaponPosition;
        bool mHasWeaponContact = false;
        osg::Vec3f mMagicOrigin;
        osg::Vec3f mMagicTarget;
        bool mMagicAttraction = false;
        float mWeaponDistance = 0.f;
        float mWeaponMultiplier = 1.f;
        float mWeaponImpulseScale = 1.f;
        float mMagicDistance = 0.f;
        float mMagicImpulseScale = 1.f;
        bool mCrossbow = false;
        WoundWeapon mWoundWeapon = WoundWeapon::None;
        bool mTwoHandedCutting = false;
        bool mTwoHandedHammer = false;
        AttackType mAttackType = AttackType::Unknown;
        // Captured on the living skeleton before damage/enchantments. An unknown
        // contact must not be guessed again from the later death pose.
        int mWeaponHitBody = -1;
        bool mDirectMagic = false;
        DeathElement mDeathElement = DeathElement::None;
        float mDominantMagicDamage = 0.f;
        bool mShock = false; // Compatibility flag; true only for dominant Shock.
        bool mValid = false;
    };

    inline float meleeMultiplier(float weight, float minimum, float maximum, float strength, bool useWeight = true)
    {
        const float weaponBonus = useWeight && std::isfinite(weight) && std::isfinite(minimum) && std::isfinite(maximum)
                && maximum > minimum
            ? std::clamp((weight - minimum) / (maximum - minimum), 0.f, 1.f)
            : 0.f;
        const float strengthBonus = std::isfinite(strength) ? std::clamp((strength - 40.f) / 60.f, 0.f, 1.f) : 0.f;
        return std::min(3.f, 1.f + weaponBonus + strengthBonus);
    }
}

#endif
