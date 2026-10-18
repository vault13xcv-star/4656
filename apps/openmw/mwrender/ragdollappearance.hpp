#ifndef OPENMW_MWRENDER_RAGDOLLAPPEARANCE_H
#define OPENMW_MWRENDER_RAGDOLLAPPEARANCE_H

#include <components/ragdoll/impact.hpp>

#include <osg/ref_ptr>
#include <osg/Vec3f>

#include <memory>
#include <span>
#include <vector>

namespace osg
{
    class Node;
    class Group;
}

namespace MWRender
{
    // Only explicitly supplied, bare skin body-part roots are traversed. Clothing,
    // armour and other actors may share the original assets without being changed.
    class RagdollAppearance
    {
    public:
        using SkinParts = std::vector<osg::ref_ptr<osg::Node>>;
        struct Settings
        {
            float mPoisonTransitionTime = 1.5f;
            float mFireTransitionTime = 2.f;
            float mFrostTransitionTime = 1.5f;
            float mFireEffectDuration = 5.f;
        };

        RagdollAppearance(osg::Group& root, Ragdoll::DeathElement element, const Settings& settings);
        ~RagdollAppearance();
        void update(float elapsed, const SkinParts& skinParts, std::span<const osg::Vec3f> bodyPositions);
        float getBlend() const;
        bool hasFire() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
