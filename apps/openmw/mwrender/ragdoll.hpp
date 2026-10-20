#ifndef OPENMW_MWRENDER_RAGDOLL_H
#define OPENMW_MWRENDER_RAGDOLL_H

#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/ragdoll/impact.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace osg
{
    class Group;
    class Node;
}

namespace SceneUtil
{
    class Skeleton;
}

namespace ESM
{
    struct RagdollState;
}

namespace MWRender
{
    /// Owns the corpse simulation and pose; RefData stores its portable saved snapshot.
    class RagdollController
    {
    public:
        using CollisionSnapshot = std::function<std::vector<osg::Vec3f>(const osg::Vec3f&, float)>;

        struct Settings
        {
            int mMaxActive = 100;
            osg::Vec3f mObserverPosition;
            float mCollisionRadius = 768.f;
            float mCollisionRefreshInterval = 0.5f;
            float mMaxSimulationTime = 10.f;
            float mBaseMass = 70.f;
            float mReferenceHeight = 122.4847f;
            float mImpulseReferenceMass = 70.f;
            bool mAllowGenericSkeleton = false;
            float mArmorMass = 0.f;
            float mDeathAge = 0.f;
            float mImpulseMultiplier = 1.f;
            float mMaxLaunchSpeed = 1400.f;
            float mShockDuration = 10.f;
            float mShockStrength = 1.f;
            bool mPinEnabled = true;
            float mPinMaxDistance = 2.f;
            float mPinContactDistance = 12.f;
            float mPoisonTransitionTime = 1.5f;
            float mFireTransitionTime = 2.f;
            float mFrostTransitionTime = 1.5f;
            float mFireEffectDuration = 15.f;
            float mPoisonEffectDuration = 10.f;
            osg::Vec3f mInitialVelocity;
            std::optional<float> mWaterLevel;
        };

        /// Returns nullptr for unsupported skeletons, missing collision geometry or an exhausted budget.
        static std::unique_ptr<RagdollController> create(osg::Group& root, SceneUtil::Skeleton& skeleton,
            CollisionSnapshot snapshot, const Settings& settings, const Ragdoll::Impact& impact = {});
        static std::unique_ptr<RagdollController> restore(osg::Group& root, SceneUtil::Skeleton& skeleton,
            CollisionSnapshot snapshot, const Settings& settings, const ESM::RagdollState& state);
        void writeState(ESM::RagdollState& state) const;
        void setWaterLevel(std::optional<float> level);
        void setArmorMass(float mass);
        void setRuntimeSettings(int maxActive, float maxSimulationTime, float pinDistance, float fireDuration,
            float poisonDuration, float shockDuration);

        ~RagdollController();

        void update(float duration);
        void setSkinParts(std::vector<osg::ref_ptr<osg::Node>> skinParts);
        float getAppearanceBlend() const;
        bool hasFireEffect() const;
        /// Native animation controllers are rebuilt when a death animation starts/ends.
        /// Keep the final pose override last, without suppressing their event bookkeeping.
        void refreshCallbacks();
        osg::Vec3f getPosition() const;
        float getMass() const;
        bool isSimulating() const;
        bool isPinned() const;

    private:
        struct Impl;
        explicit RagdollController(std::shared_ptr<Impl> impl);
        std::shared_ptr<Impl> mImpl;
    };
}

#endif
