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
        bool cuts(std::size_t body) const { return body < 15 && (mCuts & (1u << body)); }
    };

    inline Wounds selectWounds(const Impact& hit, std::size_t part, float roll, float chance)
    {
        if (!hit.mValid || !std::isfinite(roll) || !std::isfinite(chance)
            || roll < 0 || roll >= std::clamp(chance, 0.f, 1.f))
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
        if (hit.mWoundWeapon == WoundWeapon::Blunt || (hit.mWoundWeapon == WoundWeapon::Bolt && part == 2))
            return { bit(2), true };
        if (hit.mWoundWeapon != WoundWeapon::Blade && hit.mWoundWeapon != WoundWeapon::HeavyBlade)
            return {};
        unsigned cut = 0;
        if (part == 2) cut = 2;
        else if (part == 3 || part == 4 || part == 11) cut = 3;
        else if (part == 5 || part == 6 || part == 12) cut = 5;
        else if (part == 7 || part == 8 || part == 13) cut = 7;
        else if (part == 9 || part == 10 || part == 14) cut = 9;
        else if (hit.mWoundWeapon == WoundWeapon::HeavyBlade && part <= 1) cut = 1;
        return { cut ? bit(cut) : 0, false };
    }
}
#endif
