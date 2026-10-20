#include <BulletCollision/CollisionDispatch/btCollisionObject.h>

#include "collisiontype.hpp"
#include "projectile.hpp"
#include "projectileconvexcallback.hpp"

namespace MWPhysics
{
    btScalar ProjectileConvexCallback::addSingleResult(
        btCollisionWorld::LocalConvexResult& result, bool normalInWorldSpace)
    {
        const auto* hitObject = result.m_hitCollisionObject;
        // don't hit the caster
        if (hitObject == mCaster)
            return 1.f;

        // don't hit the projectile
        if (hitObject == mMe)
            return 1.f;

        if (result.m_hitFraction > m_closestHitFraction)
            return m_closestHitFraction;
        switch (hitObject->getBroadphaseHandle()->m_collisionFilterGroup)
        {
            case CollisionType_Actor:
            {
                if (!mProjectile.isValidTarget(hitObject))
                    return 1.f;
                break;
            }
            case CollisionType_Projectile:
            {
                auto* target = static_cast<Projectile*>(hitObject->getUserPointer());
                if (!mProjectile.isValidTarget(target->getCasterCollisionObject()))
                    return 1.f;
                break;
            }
        }
        return btCollisionWorld::ClosestConvexResultCallback::addSingleResult(result, normalInWorldSpace);
    }

    void ProjectileConvexCallback::commitHit()
    {
        // Bullet enumerates broadphase candidates in arbitrary order. Publishing
        // during addSingleResult permanently latched the first candidate, even
        // if a bottle in front of that wall was reported later and was closer.
        if (!hasHit())
            return;
        const int group = m_hitCollisionObject->getBroadphaseHandle()->m_collisionFilterGroup;
        if (group == CollisionType_Projectile)
            static_cast<Projectile*>(m_hitCollisionObject->getUserPointer())
                ->hit(mMe, m_hitPointWorld, m_hitNormalWorld);
        else if (group == CollisionType_Water)
            mProjectile.setHitWater();
        mProjectile.hit(m_hitCollisionObject, m_hitPointWorld, m_hitNormalWorld);
    }
}
