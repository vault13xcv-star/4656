#ifndef OPENMW_RAGDOLL_DISMEMBERMENT_HPP
#define OPENMW_RAGDOLL_DISMEMBERMENT_HPP

#include "impact.hpp"
#include <array>
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
        {
            // Reuse the conditional uniform roll after the normal chance gate:
            // equal odds of 3/4/5 extremities, then equal odds of each subset.
            // The torso/waist stays intact; selected arms/legs keep their joints.
            const float selection = std::clamp(roll / std::min(chance, 1.f), 0.f, 1.f) * 3.f;
            const unsigned group = std::min(2u, static_cast<unsigned>(selection));
            const unsigned count = 3 + group;
            const unsigned combinations = count == 3 ? 10 : count == 4 ? 5 : 1;
            unsigned choice = std::min(combinations - 1,
                static_cast<unsigned>((selection - group) * combinations));
            constexpr std::array<unsigned, 5> extremities{ 2, 3, 5, 7, 9 };
            for (unsigned mask = 0; mask < 32; ++mask)
            {
                unsigned setBits = 0;
                for (unsigned bits = mask; bits; bits >>= 1) setBits += bits & 1;
                if (setBits != count || choice-- != 0) continue;
                std::uint32_t cuts = 0;
                for (unsigned i = 0; i < extremities.size(); ++i)
                    if (mask & (1u << i)) cuts |= bit(extremities[i]);
                return { cuts, false, false, true };
            }
        }
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
