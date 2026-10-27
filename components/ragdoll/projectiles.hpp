#ifndef OPENMW_COMPONENTS_RAGDOLL_PROJECTILES_HPP
#define OPENMW_COMPONENTS_RAGDOLL_PROJECTILES_HPP

#include <osg/Vec3f>
#include <algorithm>
#include <cmath>

namespace Ragdoll
{
    struct KillSlowMotion
    {
        float mRemaining = 0;
        void trigger() { if (mRemaining <= 0) mRemaining = 2.f; }
        void advance(float realSeconds, bool paused)
        {
            if (!paused && std::isfinite(realSeconds) && realSeconds > 0)
                mRemaining = std::max(0.f, mRemaining-realSeconds);
        }
        float scale() const { return mRemaining > 0 ? 1.f/3.f : 1.f; }
    };

    inline osg::Vec3f projectileBounce(osg::Vec3f velocity, osg::Vec3f normal)
    {
        if (normal.normalize() < .001f) return {};
        const float approach=velocity*normal;
        if (approach >= 0) return velocity;
        // Lose energy at the impact; preserve tangential motion and reflect
        // only the normal component. The rigid body then falls and tumbles.
        return (velocity-normal*approach)*.55f-normal*approach*.25f;
    }
}
#endif
