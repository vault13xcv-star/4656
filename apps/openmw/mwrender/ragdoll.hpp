#ifndef OPENMW_MWRENDER_RAGDOLL_H
#define OPENMW_MWRENDER_RAGDOLL_H

#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/ragdoll/impact.hpp>
#include <components/ragdoll/dismemberment.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <map>
#include <osg/Matrixf>

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
            bool mNpc = false;
            float mWoundRoll = 1.f;
            float mWoundChance = .3f;
            float mBleedDuration = 10.f;
            std::function<void(const osg::Vec3f&)> mBloodEffect;
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
            float poisonDuration, float shockDuration, float bleedDuration = 10.f);

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
        Ragdoll::Wounds getWounds() const;
        // Substitute only influences across a severed joint. Intact joints in
        // the owning part continue to animate; whole robes follow the torso.
        struct PartPose
        {
            std::string mAnchor;
            std::map<std::string, osg::Matrixf> mOffsets;
        };
        const PartPose& getPartPose(std::size_t body, bool whole = false) const;

    private:
        struct Impl;
        explicit RagdollController(std::shared_ptr<Impl> impl);
        std::shared_ptr<Impl> mImpl;
    };
}

#endif
