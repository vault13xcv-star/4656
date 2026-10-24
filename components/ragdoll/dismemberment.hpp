#ifndef OPENMW_RAGDOLL_DISMEMBERMENT_HPP
#define OPENMW_RAGDOLL_DISMEMBERMENT_HPP

#include "impact.hpp"
#include <cstdint>

namespace Ragdoll
{
    // Bits identify the child of a removed joint in the standard humanoid rig.
    // Hands/feet remain articulated with their severed arm/leg.
    struct Wounds
    {
        std::uint32_t mCuts = 0;
        bool mBurstHead = false;
        bool mLongitudinal = false;
        bool mCrushed = false;
        bool any() const { return mCuts || mLongitudinal; }
        bool cuts(std::size_t body) const { return body < 15 && (mCuts & (1u << body)); }
    };

    inline Wounds selectWounds(const Impact& hit, std::size_t part, float roll, float chance)
    {
        if (!hit.mValid || !std::isfinite(roll) || !std::isfinite(chance)
            || roll < 0 || roll > 1 || chance <= 0 || (chance < 1 && roll >= chance))
            return {};
        constexpr auto bit = [](unsigned body) { return std::uint32_t(1) << body; };
        if (hit.mDirectMagic)
        {
            if (hit.mDeathElement == DeathElement::Fire || hit.mDeathElement == DeathElement::Frost)
                return { bit(2) | bit(3) | bit(5) | bit(7) | bit(9), false };
            if (hit.mDeathElement == DeathElement::Shock)
                return { bit(2), true };
        }
        if (!hit.mHasWeaponContact)
            return {};
        if (part >= 15)
            return {};
        const bool swing = hit.mAttackType == AttackType::Chop || hit.mAttackType == AttackType::Slash;
        if (hit.mTwoHandedHammer && swing && part <= 1)
            return { bit(1) | bit(2) | bit(3) | bit(5) | bit(7) | bit(9), false, false, true };
        // Only a descending chop through the centre line can split lengthwise.
        // The native hit key describes the swing; the direction rejects upward contacts.
        if (hit.mTwoHandedCutting && hit.mAttackType == AttackType::Chop && (part <= 2)
            && std::isfinite(hit.mWeaponDirection.length2()) && hit.mWeaponDirection.z() < -.01f)
            return { 0, false, true, false };
        if ((hit.mWoundWeapon == WoundWeapon::Blunt && part == 2
                && (hit.mAttackType == AttackType::Chop || hit.mAttackType == AttackType::Slash))
            || (hit.mWoundWeapon == WoundWeapon::Bolt && part == 2))
            return { bit(2), true };
        if (hit.mWoundWeapon != WoundWeapon::Blade && hit.mWoundWeapon != WoundWeapon::HeavyBlade)
            return {};
        // A thrust punctures; it cannot sever or bisect. Unknown animation keys
        // are equally unsuitable for inventing a cutting plane.
        if (hit.mAttackType != AttackType::Chop && hit.mAttackType != AttackType::Slash)
            return {};
        const bool arm = part == 3 || part == 4 || part == 11 || part == 5 || part == 6 || part == 12;
        const bool horizontal = hit.mAttackType == AttackType::Slash
            && std::isfinite(hit.mWeaponDirection.length2()) && hit.mWeaponDirection.length2() > 0.f
            && std::abs(hit.mWeaponDirection.z()) <= hit.mWeaponDirection.length() * .5f;
        // A heavy lateral sweep may continue through an arm into the torso.
        // Head/leg contacts never turn into a remotely located waist cut.
        if (hit.mWoundWeapon == WoundWeapon::HeavyBlade && horizontal
            && (part <= 1 || (arm && roll < std::min(chance, 1.f) * .5f)))
            return { bit(1), false };
        unsigned cut = 0;
        if (part == 2) cut = 2;
        else if (part == 3 || part == 4 || part == 11) cut = 3;
        else if (part == 5 || part == 6 || part == 12) cut = 5;
        else if (part == 7 || part == 8 || part == 13) cut = 7;
        else if (part == 9 || part == 10 || part == 14) cut = 9;
        return { cut ? bit(cut) : 0, false };
    }
}
#endif
