#ifndef OPENMW_COMPONENTS_RAGDOLL_BLOODFLOW_HPP
#define OPENMW_COMPONENTS_RAGDOLL_BLOODFLOW_HPP

#include <osg/Vec3f>
#include <algorithm>
#include <cmath>
#include <functional>

namespace Ragdoll
{
    // A small surface-bound volume. Gravity is projected onto the contacted
    // plane; a probe follows its actual geometry and releases it at an edge.
    struct BloodFlow
    {
        osg::Vec3f mPosition, mNormal, mVelocity;
        float mAge = 0;
    };
    enum class BloodFlowResult { Attached, Detached, Settled };
    using BloodFlowRay = std::function<bool(const osg::Vec3f&, const osg::Vec3f&, osg::Vec3f&, osg::Vec3f&)>;

    inline BloodFlowResult advanceBloodFlow(BloodFlow& flow, float elapsed, const BloodFlowRay& ray)
    {
        if (!ray || !std::isfinite(elapsed) || elapsed <= 0 || !std::isfinite(flow.mPosition.length2())
            || !std::isfinite(flow.mNormal.length2()) || !std::isfinite(flow.mVelocity.length2()))
            return BloodFlowResult::Settled;
        const float dt = std::min(elapsed, .1f);
        flow.mAge += dt;
        osg::Vec3f normal = flow.mNormal;
        if (normal.normalize() < .001f) return BloodFlowResult::Settled;
        const osg::Vec3f gravity(0, 0, -627.2f);
        // Drops on ceilings detach under gravity instead of sliding upwards.
        if (normal.z() < -.7f)
            return flow.mAge >= .15f ? BloodFlowResult::Detached : BloodFlowResult::Attached;
        const auto acceleration = gravity - normal * (gravity * normal);
        flow.mVelocity = (flow.mVelocity + acceleration * dt) * std::exp(-5.f * dt);
        const float speed = flow.mVelocity.length();
        if (speed > 65.f) flow.mVelocity *= 65.f / speed;
        if (acceleration.length2() < 4.f && speed < 1.f) return BloodFlowResult::Settled;
        const osg::Vec3f target = flow.mPosition + flow.mVelocity * dt;
        osg::Vec3f point, nextNormal;
        const auto probe = [&](const osg::Vec3f& where, osg::Vec3f& hit, osg::Vec3f& hitNormal) {
            return ray(where + normal * 2.f, where - normal * 2.f, hit, hitNormal)
                && hitNormal.normalize() >= .001f && hitNormal * normal >= .5f;
        };
        if (!probe(target,point,nextNormal))
        {
            // Approximate the edge before releasing the drop. Keep the work
            // bounded: two extra probes rather than an unbounded search.
            float low=0, high=1;
            const auto origin=flow.mPosition;
            for (int i=0;i<2;++i)
            {
                const float middle=(low+high)*.5f;
                osg::Vec3f edge,edgeNormal;
                if (probe(origin+(target-origin)*middle,edge,edgeNormal))
                { low=middle;flow.mPosition=edge;flow.mNormal=edgeNormal; }
                else high=middle;
            }
            return BloodFlowResult::Detached;
        }
        flow.mPosition = point;
        flow.mNormal = nextNormal;
        flow.mVelocity -= nextNormal * (flow.mVelocity * nextNormal);
        return BloodFlowResult::Attached;
    }
}
#endif
