#ifndef OPENMW_MWRENDER_RAGDOLLAPPEARANCE_H
#define OPENMW_MWRENDER_RAGDOLLAPPEARANCE_H

#include <components/ragdoll/impact.hpp>

#include <osg/ref_ptr>
#include <osg/Vec3f>
#include <osg/ShapeDrawable>

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
    // Opaque physical wound surfaces use the same depth/projection contract as
    // the world, including reversed Z and inherited NPC material overrides.
    osg::ref_ptr<osg::ShapeDrawable> createWoundDrawable(osg::Shape* shape);

    // Explicit per-actor worn surfaces include skin, hair, clothes and armour.
    // Original cached textures and the materials of other actors stay immutable.
    class RagdollAppearance
    {
    public:
        using SkinParts = std::vector<osg::ref_ptr<osg::Node>>;
        struct Settings
        {
            float mPoisonTransitionTime = 1.5f;
            float mFireTransitionTime = 2.f;
            float mFrostTransitionTime = 1.5f;
            float mFireEffectDuration = 15.f;
            float mPoisonEffectDuration = 10.f;
            float mShockEffectDuration = 10.f;
        };

        RagdollAppearance(osg::Group& root, Ragdoll::DeathElement element, const Settings& settings);
        ~RagdollAppearance();
        void update(float elapsed, const SkinParts& skinParts, std::span<const osg::Vec3f> bodyPositions);
        float getBlend() const;
        void setEffectDurations(float fire, float poison, float shock);
        bool hasFire() const;
        bool hasParticles() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
