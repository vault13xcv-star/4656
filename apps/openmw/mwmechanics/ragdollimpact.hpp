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
        bool mAwaitingMagic = false;
        double mContactAge = 0.;

        bool isRecentContact() const { return std::isfinite(mContactAge) && mContactAge >= 0. && mContactAge < 1.; }
        bool isPending() const { return mAwaitingWeapon || mAwaitingMagic; }

        void applyWeapon()
        {
            mApplied.mWeaponDirection = mWeapon.mWeaponDirection;
            mApplied.mWeaponDistance = mWeapon.mWeaponDistance;
            mApplied.mWeaponMultiplier = mWeapon.mWeaponMultiplier;
            mApplied.mWeaponImpulseScale = mWeapon.mWeaponImpulseScale;
            mApplied.mContactMomentum = mWeapon.mContactMomentum;
            mApplied.mContactSpeed = mWeapon.mContactSpeed;
            mApplied.mWeaponPosition = mWeapon.mWeaponPosition;
            mApplied.mHasWeaponContact = mWeapon.mHasWeaponContact;
            mApplied.mCrossbow = mWeapon.mCrossbow;
            mApplied.mWoundWeapon = mWeapon.mWoundWeapon;
            mApplied.mTwoHandedCutting = mWeapon.mTwoHandedCutting;
            mApplied.mTwoHandedHammer = mWeapon.mTwoHandedHammer;
            mApplied.mAttackType = mWeapon.mAttackType;
            mApplied.mWeaponHitBody = mWeapon.mWeaponHitBody;
            mApplied.mPosition = mWeapon.mPosition;
            mApplied.mValid = true;
        }

        void beginMagicTick(float duration = 0.f)
        {
            // Native zero-time priming marks effects Applied without doing any
            // duration damage. It must neither age nor erase the shared contact.
            if (!std::isfinite(duration) || duration <= 0.f)
                return;
            mContactAge += static_cast<double>(duration);
            // Keep actual weapon damage and short-lived enchantment contributions
            // together for the first second, regardless of Lua/effect ordering.
            // After that only the current DOT tick can select death presentation;
            // the original physical blow and wave are no longer eligible.
            if (!isRecentContact())
            {
                mAwaitingWeapon = mAwaitingMagic = false;
                mApplied = {};
                mMagic.clear();
            }
        }

        void applyMagic(std::uint64_t id, const Ragdoll::Impact& impact)
        {
            // A duration effect replaces its own contribution; ticks never multiply
            // the impulse. Only effects that really removed health enter this map.
            const bool contributes = impact.mValid && std::isfinite(impact.mDominantMagicDamage)
                && impact.mDominantMagicDamage > 0.f;
            if (contributes)
            {
                mAwaitingMagic = false;
                mMagic[id] = impact;
            }
            else
                mMagic.erase(id);
            mApplied.mMagicDistance = 0.f;
            mApplied.mMagicDirection = {};
            mApplied.mMagicOrigin = {};
            mApplied.mMagicTarget = {};
            mApplied.mMagicAttraction = false;
            mApplied.mDeathElement = Ragdoll::DeathElement::None;
            mApplied.mDominantMagicDamage = 0.f;
            mApplied.mDirectMagic = false;
            for (const auto& [key, contribution] : mMagic)
            {
                // One effect supplies both motion and presentation. A weak,
                // wide-area effect must not lend its radius to a stronger one.
                // Strict comparison makes equal damage deterministic: the first
                // effect in native effect order wins. Ordinary health damage also
                // participates and can suppress a weaker elemental presentation.
                if (contribution.mDominantMagicDamage > mApplied.mDominantMagicDamage)
                {
                    mApplied.mDominantMagicDamage = contribution.mDominantMagicDamage;
                    mApplied.mDeathElement = contribution.mDeathElement;
                    mApplied.mMagicDistance = contribution.mMagicDistance;
                    mApplied.mMagicImpulseScale = contribution.mMagicImpulseScale;
                    mApplied.mMagicDirection = contribution.mMagicDirection;
                    mApplied.mMagicOrigin = contribution.mMagicOrigin;
                    mApplied.mMagicTarget = contribution.mMagicTarget;
                    mApplied.mMagicAttraction = contribution.mMagicAttraction;
                    mApplied.mDirectMagic = contribution.mDirectMagic;
                }
            }
            mApplied.mShock = mApplied.mDeathElement == Ragdoll::DeathElement::Shock;
            if (contributes)
            {
                mApplied.mPosition = impact.mPosition;
                mApplied.mValid = true;
            }
        }

    };

    struct RagdollMagicImpact
    {
        std::shared_ptr<RagdollHit> mHit;
        std::uint64_t mEffect = 0;
        osg::Vec3f mOrigin;
        osg::Vec3f mDirection;
        osg::Vec3f mTargetPosition;
        bool mHasTargetPosition = false;
        float mRadius = 0.f; // metres; ESM effect area is stored in feet
        bool mExploded = false;
        bool mHasImpulse = true; // False when only the effect survives a save/load.
    };
    inline Ragdoll::Impact makeRagdollMagicImpact(const RagdollMagicImpact& context, const osg::Vec3f& targetPosition,
        float scale, Ragdoll::DeathElement element, float effectiveDamage)
    {
        Ragdoll::Impact impact;
        impact.mDeathElement = element;
        impact.mDominantMagicDamage = effectiveDamage;
        impact.mShock = element == Ragdoll::DeathElement::Shock;
        impact.mValid = true;
        // Death before one second is an immediate kill, including native
        // zero-time priming followed by real duration damage on the next frame.
        // Later DOT retains the element, but cannot replay the old impact.
        if (!context.mHasImpulse || (context.mHit && !context.mHit->isRecentContact()))
            return impact;
        impact.mDirectMagic = !context.mExploded && context.mHit;
        impact.mMagicImpulseScale = scale;
        impact.mPosition = context.mOrigin;
        impact.mMagicOrigin = context.mOrigin;
        impact.mMagicTarget = context.mHasTargetPosition ? context.mTargetPosition : targetPosition;
        // Direct elemental hits without an explosion have a one-metre baseline.
        // Indirect splash requires an actual explosion radius.
        const float radius = context.mRadius > 0.f ? context.mRadius : impact.mDirectMagic ? 1.f : 0.f;
        if (element == Ragdoll::DeathElement::None || !std::isfinite(context.mRadius) || context.mRadius < 0.f || radius <= 0.f
            || !std::isfinite(scale) || scale <= 0.f)
            return impact;
        impact.mMagicDirection = context.mDirection;
        float distance = 0.f;
        if (context.mExploded || element == Ragdoll::DeathElement::Poison)
        {
            impact.mMagicDirection = impact.mMagicTarget - context.mOrigin;
            distance = impact.mMagicDirection.length() / Constants::UnitsPerMeter;
        }
        if (impact.mMagicDirection.normalize() == 0.f)
        {
            impact.mMagicDirection = context.mDirection;
            if (impact.mMagicDirection.normalize() == 0.f)
                impact.mMagicDirection.set(0, 0, 1);
        }
        if (element == Ragdoll::DeathElement::Poison)
        {
            impact.mMagicAttraction = true;
            impact.mMagicDirection = -impact.mMagicDirection;
            impact.mMagicDistance
                = distance <= radius ? std::min(distance, radius - 0.5f * distance) * scale : 0.f;
        }
        else
            impact.mMagicDistance = std::max(0.f, radius - distance) * scale;
        return impact;
    }
}

#endif
