#ifndef OPENMW_MWRENDER_RAGDOLL_H
#define OPENMW_MWRENDER_RAGDOLL_H

#include <osg/Vec3f>

#include <components/ragdoll/impact.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace osg
{
    class Group;
}

namespace SceneUtil
{
    class Skeleton;
}

namespace MWRender
{
    /// Owns a disposable, scene-local corpse pose. It never changes savegame or actor state.
    class RagdollController
    {
    public:
        using CollisionSnapshot = std::function<std::vector<osg::Vec3f>(const osg::Vec3f&, float)>;

        struct Settings
        {
            int mMaxActive = 10;
            float mCollisionRadius = 768.f;
            float mCollisionRefreshInterval = 0.5f;
            float mMaxSimulationTime = 10.f;
            float mBaseMass = 80.f;
            float mReferenceHeight = 128.f;
            float mArmorMass = 0.f;
            float mDeathAge = 0.f;
            float mImpulseMultiplier = 1.f;
            float mMaxLaunchSpeed = 1400.f;
            float mShockDuration = 5.f;
            float mShockStrength = 1.f;
            bool mPinEnabled = true;
            float mPinMaxDistance = 3.f;
            float mPinContactDistance = 12.f;
        };

        /// Returns nullptr for unsupported skeletons, missing collision geometry or an exhausted budget.
        static std::unique_ptr<RagdollController> create(osg::Group& root, SceneUtil::Skeleton& skeleton,
            CollisionSnapshot snapshot, const Settings& settings, const Ragdoll::Impact& impact = {});

        ~RagdollController();

        void update(float duration);
        /// Native animation controllers are rebuilt when a death animation starts/ends.
        /// Keep the final pose override last, without suppressing their event bookkeeping.
        void refreshCallbacks();
        osg::Vec3f getPosition() const;
        float getMass() const;
        bool isSimulating() const;
        bool isPinned() const;

    private:
        struct Impl;
        explicit RagdollController(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
