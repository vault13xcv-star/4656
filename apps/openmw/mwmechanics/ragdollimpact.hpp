#ifndef OPENMW_MWMECHANICS_RAGDOLLIMPACT_HPP
#define OPENMW_MWMECHANICS_RAGDOLLIMPACT_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>

#include <components/ragdoll/impact.hpp>
#include <components/misc/constants.hpp>

namespace MWMechanics
{
    // One native contact, shared by its weapon damage and enchantment effects.
    // No actor pointers and no persistent/save-game state are retained here.
    struct RagdollHit
    {
        Ragdoll::Impact mApplied;
        Ragdoll::Impact mWeapon;
        std::map<std::uint64_t, Ragdoll::Impact> mMagic;
        std::uint64_t mNextEffect = 0;
        bool mAwaitingWeapon = false;
        float mContactAge = 0.f;

        void applyWeapon()
        {
            mApplied.mWeaponDirection = mWeapon.mWeaponDirection;
            mApplied.mWeaponDistance = mWeapon.mWeaponDistance;
            mApplied.mCrossbow = mWeapon.mCrossbow;
            mApplied.mPosition = mWeapon.mPosition;
            mApplied.mValid = true;
        }

        void beginMagicTick(float duration = 0.f)
        {
            mContactAge += std::max(0.f, duration);
            if (mContactAge > 0.1f)
                mAwaitingWeapon = false;
            // Enchantment and Lua weapon damage must meet in the same contact.
            // Later duration ticks have their own current magical contributors;
            // expired shock effects and the old weapon blow cannot linger.
            if (!mAwaitingWeapon)
            {
                mApplied = {};
                mMagic.clear();
            }
        }

        void applyMagic(std::uint64_t id, const Ragdoll::Impact& impact)
        {
            // A duration effect replaces its own contribution; ticks never multiply
            // the impulse. Only effects that really removed health enter this map.
            mMagic[id] = impact;
            mApplied.mMagicDistance = 0.f;
            mApplied.mMagicDirection = {};
            mApplied.mShock = false;
            for (const auto& [key, contribution] : mMagic)
            {
                // A multi-effect explosion has one spatial radius, not one
                // independent full-strength explosion per elemental component.
                if (contribution.mMagicDistance > mApplied.mMagicDistance)
                {
                    mApplied.mMagicDistance = contribution.mMagicDistance;
                    mApplied.mMagicDirection = contribution.mMagicDirection;
                }
                mApplied.mShock = mApplied.mShock || contribution.mShock;
            }
            mApplied.mPosition = impact.mPosition;
            mApplied.mValid = true;
        }
    };

    struct RagdollMagicImpact
    {
        std::shared_ptr<RagdollHit> mHit;
        std::uint64_t mEffect = 0;
        osg::Vec3f mOrigin;
        osg::Vec3f mDirection;
        float mRadius = 0.f; // metres; ESM effect area is stored in feet
        bool mExploded = false;
    };
    inline Ragdoll::Impact makeRagdollMagicImpact(const RagdollMagicImpact& context,
        const osg::Vec3f& targetPosition, float minimumDistance, float scale, bool shock)
    {
        Ragdoll::Impact impact;
        impact.mPosition = context.mOrigin;
        impact.mMagicDirection = context.mDirection;
        float distance = 0.f;
        if (context.mExploded)
        {
            impact.mMagicDirection = targetPosition - context.mOrigin;
            distance = impact.mMagicDirection.length() / Constants::UnitsPerMeter;
        }
        if (impact.mMagicDirection.normalize() == 0.f)
        {
            impact.mMagicDirection = context.mDirection;
            if (impact.mMagicDirection.normalize() == 0.f)
                impact.mMagicDirection.set(0, 1, 0);
        }
        impact.mMagicDistance = std::max(minimumDistance, context.mRadius - distance) * scale;
        impact.mShock = shock;
        impact.mValid = true;
        return impact;
    }
}

#endif
