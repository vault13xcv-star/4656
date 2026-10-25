#ifndef OPENMW_COMPONENTS_RAGDOLL_PHYSICALCOMBAT_HPP
#define OPENMW_COMPONENTS_RAGDOLL_PHYSICALCOMBAT_HPP

#include <algorithm>
#include <cmath>
#include <osg/Vec3f>

// Shared by live combat, animation and the standalone regression tests. Units
// for motion are OpenMW world units/second; inventory weights are converted to
// an effective driven weapon mass, rather than treated as kilograms.
namespace PhysicalCombat
{
    inline float finite(float value, float fallback) { return std::isfinite(value) ? value : fallback; }
    enum class Handling { LightBlade, Heavy, Mixed };
    struct Blade
    {
        osg::Vec3f mBase, mTip;
        float mGripReach = 0; // Furthest endpoint from the attachment, including model scale.
    };
    inline float approachRange(float gripReach, float halfHeight, float bodyRadius)
    {
        // AI distances are measured between collision bounds, while blade
        // contact needs to reach the visible body. Leave room for bent elbows
        // and the gap between the target's capsule and its skin.
        const float armReach = .6f * std::max(0.f, finite(halfHeight, 64));
        return std::max(4.f, .8f * (armReach + std::max(0.f, finite(gripReach, 0)))
            - std::max(0.f, finite(bodyRadius, 16)) - 8.f);
    }
    inline osg::Vec3f pointVelocity(const Blade& previous, const Blade& current, float along, float duration)
    {
        return duration > 0 ? ((current.mBase - previous.mBase) * (1 - along)
            + (current.mTip - previous.mTip) * along) / duration : osg::Vec3f();
    }
    // Check both the shaft and its swept surface, with bounded work per swing.
    template <class Trace> void sweep(const Blade& previous, const Blade& current, Trace&& trace)
    {
        const float length = std::max((previous.mTip - previous.mBase).length(), (current.mTip - current.mBase).length());
        if (!std::isfinite(length)) return;
        const int samples = std::clamp(int(std::ceil(std::min(length, 96.f) / 4)), 4, 24);
        for (int i = 0; i <= samples; ++i)
        {
            const float along = float(i) / samples;
            trace(previous.mBase * (1 - along) + previous.mTip * along,
                current.mBase * (1 - along) + current.mTip * along, 0.f, 1.f, along);
        }
        for (int step = 1; step <= 4; ++step)
        {
            const float t = step / 4.f;
            trace(previous.mBase * (1 - t) + current.mBase * t,
                previous.mTip * (1 - t) + current.mTip * t, t, t, .7f);
        }
    }
    struct Weapon
    {
        float mWeight = 12, mLength = 70, mRecordSpeed = 1;
        Handling mHandling = Handling::Mixed;
    };
    struct Actor
    {
        float mStrength = 40, mAgility = 40, mSkill = 30, mFatigue = 1;
    };
    inline float inertia(const Weapon& weapon)
    {
        return std::clamp((std::max(0.f, finite(weapon.mWeight, 12)) + 10) / 30
            * std::pow(std::clamp(finite(weapon.mLength, 70) / 70, .35f, 2.5f), 2.f), .4f, 8.f);
    }
    struct Timing
    {
        float mWindup, mStrike, mRecovery, mMinimumCycle;
    };
    inline Timing timing(const Weapon& weapon, const Actor& actor)
    {
        const float strength = std::clamp(finite(actor.mStrength, 40), 0.f, 150.f);
        const float agility = std::clamp(finite(actor.mAgility, 40), 0.f, 150.f);
        const float skill = std::clamp(finite(actor.mSkill, 30), 0.f, 150.f);
        const float attribute = weapon.mHandling == Handling::Heavy ? strength
            : weapon.mHandling == Handling::LightBlade ? .3f * strength + .7f * agility
                                                       : .5f * (strength + agility);
        // Attribute and skill bonuses add. They never multiply one another.
        const float bonus = .003f * (attribute - 40) + .0035f * (skill - 30);
        const float inertia = PhysicalCombat::inertia(weapon);
        const float base = std::clamp(finite(weapon.mRecordSpeed, 1), .65f, 1.3f);
        const float fatigue = actor.mFatigue > 0 ? 1.f : .5f;
        // Lifting/accelerating a heavy weapon costs time. The striking phase
        // stays brisk once momentum has built, including for a new character.
        return { std::clamp(base * (1 + bonus) * (.55f + .45f / std::sqrt(inertia)), .72f, 1.75f) * fatigue,
            std::clamp(base * (1.05f + bonus * .65f) / (.9f + .1f * std::sqrt(inertia)), .95f, 1.85f) * fatigue,
            std::clamp(base * (1 + bonus * .7f) * (.6f + .4f / std::sqrt(inertia)), .75f, 1.6f) * fatigue,
            std::clamp(.48f + .14f * std::sqrt(inertia) - .0012f * skill, .36f, .85f) / fatigue };
    }
    inline float skillDamage(float skill) { return .55f + .0075f * std::clamp(skill, 0.f, 100.f); }
    inline float speedDamage(float closingSpeed, float referenceSpeed)
    {
        // Smoothly bound kinetic energy. A hard cap in the normal expert swing
        // range would swallow the additional velocity of a charge or fall.
        const float ratio = std::clamp(finite(closingSpeed, 0) / std::max(120.f, finite(referenceSpeed, 500)), 0.f, 20.f);
        const float energy = ratio * ratio;
        return .35f + 2.65f * energy / (energy + 3.076923f);
    }
    inline float impulse(const Weapon& weapon, float closingSpeed)
    {
        return (2.f + .12f * std::clamp(finite(weapon.mWeight, 12), 0.f, 150.f))
            * std::sqrt(std::clamp(finite(weapon.mLength, 70) / 70, .5f, 2.f))
            * std::clamp(finite(closingSpeed, 0), 0.f, 1600.f);
    }
    inline float raiseTime(float skill, bool exhausted)
    {
        return (.28f - .0016f * std::clamp(skill, 0.f, 100.f)) * (exhausted ? 2.f : 1.f);
    }
    inline int shieldWear(float absorbed, float skill)
    {
        if (absorbed <= 0) return 0;
        return std::max(1, int(std::ceil(std::clamp(finite(absorbed, 0), 0.f, 65535.f)
            * (.9f - .006f * std::clamp(finite(skill, 0), 0.f, 100.f)))));
    }
    inline float shieldFatigue(float absorbed, float weight, float strength, float skill)
    {
        return (2.f + std::max(0.f, absorbed) * .15f + std::max(0.f, weight) * std::clamp(strength, 0.f, 1.f) * .08f)
            * (1.f - .006f * std::clamp(skill, 0.f, 100.f));
    }
    struct Guard
    {
        bool mRequested = false, mHeld = false;
        float mProgress = 0, mReadyAge = 0, mReleaseTime = 1;
        // Returns fatigue spent on a new raise, never once per render frame.
        float update(float duration, float skill, bool exhausted, bool available)
        {
            const float dt = std::clamp(duration, 0.f, .1f);
            float cost = 0;
            const bool held = mRequested && available && (mHeld || mReleaseTime >= .12f);
            if (held && !mHeld)
            {
                mProgress = mReadyAge = 0;
                cost = 7.f - .05f * std::clamp(skill, 0.f, 100.f);
            }
            mHeld = held;
            if (held)
            {
                const float remaining = (1 - mProgress) * raiseTime(skill, exhausted);
                mProgress = std::min(1.f, mProgress + dt / raiseTime(skill, exhausted));
                mReadyAge += std::max(0.f, dt - remaining);
                mReleaseTime = 0;
            }
            else
            {
                mProgress = std::max(0.f, mProgress - dt / .12f);
                mReleaseTime += dt;
            }
            return cost;
        }
        float damageMultiplier(bool exhausted) const
        {
            if (!mHeld || mProgress < .999f) return 1;
            const bool timely = mReadyAge <= .22f;
            return exhausted ? (timely ? .5f : .9f) : (timely ? 0.f : .5f);
        }
    };
    struct State
    {
        Guard mGuard;
        osg::Vec3f mVelocity, mLastPosition, mPush, mLean, mLeanVelocity;
        osg::Vec3f mContactVelocity, mContactPosition, mContactNormal;
        osg::Vec3f mWeaponLag, mWeaponLagVelocity, mLastBladeDirection;
        float mPoise = 0, mContactSpeed = 0, mContactAge = 10;
        float mRecoil = 0;
        float mWeaponLength = 0, mWeaponGripReach = 0;
        int mContactBody = -1;
        bool mHasPosition = false, mContactValid = false;
        bool mHasBladeDirection = false;
        bool mSwinging = false;
        void driveWeapon(const Blade& target, float duration, const Weapon& weapon, float strength)
        {
            const float dt = std::clamp(finite(duration, 0), 0.f, .1f);
            if (dt <= 0) return;
            auto direction = target.mTip - target.mBase;
            if (!std::isfinite(direction.length2()) || direction.normalize() == 0) return;
            if (!mHasBladeDirection) { mLastBladeDirection = direction; mHasBladeDirection = true; }
            auto axis = mLastBladeDirection ^ direction;
            const float angle = std::atan2(axis.length(), std::clamp(mLastBladeDirection * direction, -1.f, 1.f));
            if (axis.normalize() == 0) axis.set(0, 0, 1);
            const float leverage = std::sqrt(inertia(weapon));
            const float control = .8f + .005f * std::clamp(finite(strength, 40), 0.f, 150.f);
            osg::Vec3f wanted = -axis * (std::min(40.f, angle / dt) * (.003f + .004f * leverage) / control);
            if (wanted.length() > .16f) wanted *= .16f / wanted.length();
            const float spring = 700.f * control / (.5f + leverage);
            const float damping = 1.5f * std::sqrt(spring);
            const int steps = std::max(1, int(std::ceil(dt * 120)));
            const float step = dt / steps;
            for (int i = 0; i < steps; ++i)
            {
                mWeaponLagVelocity += ((wanted - mWeaponLag) * spring - mWeaponLagVelocity * damping) * step;
                mWeaponLag += mWeaponLagVelocity * step;
            }
            mLastBladeDirection = direction;
        }
        void advance(float duration)
        {
            const float dt = std::clamp(duration, 0.f, .1f);
            mContactAge += dt;
            mRecoil *= std::exp(-14.f * dt);
            mPoise = std::max(0.f, mPoise - .65f * dt);
            mPush *= std::exp(-5.f * dt);
            // Stable semi-implicit spring integration; no per-frame rigid bodies
            // or scene nodes are allocated for living hit reactions.
            const int steps = std::max(1, int(std::ceil(dt * 120)));
            const float step = dt / steps;
            for (int i = 0; i < steps; ++i)
            {
                mLeanVelocity += (-mLean * 95.f - mLeanVelocity * 16.f) * step;
                mLean += mLeanVelocity * step;
            }
        }
        bool receive(const osg::Vec3f& direction, float momentum, float mass, float agility, float fatigue)
        {
            mass = std::clamp(mass, 15.f, 500.f);
            const float speed = std::clamp(momentum / mass, 0.f, 220.f);
            mPush += direction * speed;
            if (mPush.length() > 220) mPush *= 220 / mPush.length();
            mLeanVelocity += osg::Vec3f(direction.y(), -direction.x(), 0) * (speed * .035f);
            const float resistance = mass * (65.f + .6f * std::clamp(agility, 0.f, 150.f))
                * (fatigue > 0 ? 1.f : .6f);
            mPoise += momentum / resistance;
            if (mPoise >= 1) { mPoise = 0; return true; }
            return false;
        }
    };
}
#endif
