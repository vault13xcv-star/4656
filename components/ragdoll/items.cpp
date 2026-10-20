#include "items.hpp"

#include <BulletCollision/CollisionShapes/btShapeHull.h>
#include <algorithm>
#include <btBulletDynamicsCommon.h>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace Ragdoll
{
    namespace
    {
        bool finite(const btVector3& v)
        {
            return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
        }

        // A convex hull retains the silhouette of a bottle/axe/helmet instead
        // of substituting a world-aligned box. Concave item interiors are solid.
        void vertices(const btCollisionShape& shape, const btTransform& transform, std::vector<btVector3>& out)
        {
            if (out.size() > 16384)
                return;
            if (shape.isCompound())
            {
                const auto& compound = static_cast<const btCompoundShape&>(shape);
                for (int i = 0; i < compound.getNumChildShapes(); ++i)
                    vertices(*compound.getChildShape(i), transform * compound.getChildTransform(i), out);
            }
            else if (shape.isConvex())
            {
                btShapeHull hull(static_cast<const btConvexShape*>(&shape));
                hull.buildHull(shape.getMargin());
                for (int i = 0; i < hull.numVertices(); ++i)
                    out.push_back(transform * hull.getVertexPointer()[i]);
            }
            else if (shape.isConcave())
            {
                struct Callback : btTriangleCallback
                {
                    const btTransform& mTransform;
                    std::vector<btVector3>& mVertices;
                    Callback(const btTransform& t, std::vector<btVector3>& v) : mTransform(t), mVertices(v) {}
                    void processTriangle(btVector3* triangle, int, int) override
                    {
                        if (mVertices.size() <= 16384)
                            for (int i = 0; i < 3; ++i)
                                mVertices.push_back(mTransform * triangle[i]);
                    }
                } callback(transform, out);
                static_cast<const btConcaveShape&>(shape).processAllTriangles(
                    &callback, btVector3(-1e8, -1e8, -1e8), btVector3(1e8, 1e8, 1e8));
            }
        }
    }

    struct ItemSimulation::Impl
    {
        btDefaultCollisionConfiguration mConfiguration;
        btCollisionDispatcher mDispatcher{ &mConfiguration };
        btDbvtBroadphase mBroadphase;
        btSequentialImpulseConstraintSolver mSolver;
        btDiscreteDynamicsWorld mWorld{ &mDispatcher, &mBroadphase, &mSolver, &mConfiguration };
        struct Item
        {
            std::unique_ptr<btConvexHullShape> mShape;
            std::unique_ptr<btRigidBody> mBody;
            btTransform mOffset;
            btTransform mLastPose;
            btScalar mMass;
            btVector3 mInertia;
            btScalar mDeadline = 0;
            std::uint64_t mActivation = 0;
            bool mActive = false;
        };
        std::map<Id, Item> mItems;
        Settings mSettings;
        btVector3 mObserver{ 0, 0, 0 };
        btScalar mClock = 0;
        std::uint64_t mNextActivation = 0;
        std::map<const btCollisionObject*, Id> mBodyIds;
        std::set<std::pair<Id, Id>> mContacts;
        std::set<Id> mActiveIds, mDirtyIds;
        std::map<Id, std::set<Id>> mKinematicContacts;
        std::vector<StaticCollider> mColliders;
        std::vector<std::unique_ptr<btCollisionObject>> mStatics;

        explicit Impl(const Settings& settings)
            : mSettings(settings)
        {
            if (settings.mMaxActive < 1 || settings.mMaxActive > 100 || !std::isfinite(settings.mMaxSimulationTime)
                || settings.mMaxSimulationTime <= 0)
                throw std::invalid_argument("Invalid item physics limits");
            mWorld.setGravity(btVector3(0, 0, -627.2));
            mWorld.getSolverInfo().m_numIterations = 20;
        }
        std::size_t activeCount() const { return mActiveIds.size(); }

        void freeze(Item& item)
        {
            // A dormant item is a genuine fixed collision body, not a dynamic
            // body that Bullet may wake through gravity or a persistent contact.
            mWorld.removeRigidBody(item.mBody.get());
            item.mBody->setLinearVelocity(btVector3(0, 0, 0));
            item.mBody->setAngularVelocity(btVector3(0, 0, 0));
            item.mBody->clearForces();
            item.mBody->setMassProps(0, btVector3(0, 0, 0));
            item.mBody->updateInertiaTensor();
            item.mBody->forceActivationState(ISLAND_SLEEPING);
            mWorld.addRigidBody(item.mBody.get(), 2, 1 | 2);
            item.mActive = false;
            item.mDeadline = 0;
            const auto id = mBodyIds.at(item.mBody.get());
            mActiveIds.erase(id);
            mDirtyIds.insert(id);
        }

        void activate(Item& item)
        {
            if (item.mActive)
            {
                item.mBody->activate(true);
                return; // A hit inside the current window never extends it.
            }
            while (activeCount() >= mSettings.mMaxActive)
            {
                auto victim = mItems.end();
                const auto priority = [&](const Item& value) {
                    return std::tuple(!value.mBody->isActive(),
                        value.mBody->getCenterOfMassPosition().distance2(mObserver),
                        std::numeric_limits<std::uint64_t>::max() - value.mActivation);
                };
                for (const auto activeId : mActiveIds)
                {
                    auto it = mItems.find(activeId);
                    if (victim == mItems.end() || priority(it->second) > priority(victim->second))
                        victim = it;
                }
                freeze(victim->second);
            }
            mWorld.removeRigidBody(item.mBody.get());
            item.mBody->setMassProps(item.mMass, item.mInertia);
            item.mBody->updateInertiaTensor();
            item.mBody->forceActivationState(ACTIVE_TAG);
            item.mBody->setDeactivationTime(0);
            mWorld.addRigidBody(item.mBody.get(), 2, 1 | 2);
            item.mActive = true;
            item.mDeadline = mClock + mSettings.mMaxSimulationTime;
            item.mActivation = ++mNextActivation;
            const auto id = mBodyIds.at(item.mBody.get());
            mActiveIds.insert(id);
            mDirtyIds.insert(id);
        }

        void wakeContacts()
        {
            // Collect before changing broadphase proxies: activation invalidates
            // manifolds. Static shelves/floors never enter this item-pair set.
            mWorld.performDiscreteCollisionDetection();
            std::set<std::pair<Id, Id>> contacts;
            std::set<Id> wake;
            for (int i = 0; i < mDispatcher.getNumManifolds(); ++i)
            {
                const auto* manifold = mDispatcher.getManifoldByIndexInternal(i);
                const auto left = mBodyIds.find(manifold->getBody0());
                const auto right = mBodyIds.find(manifold->getBody1());
                if (left == mBodyIds.end() || right == mBodyIds.end())
                    continue;
                bool touching = false;
                for (int j = 0; j < manifold->getNumContacts(); ++j)
                    touching |= manifold->getContactPoint(j).getDistance() <= 0.05;
                if (!touching)
                    continue;
                const auto key = std::minmax(left->second, right->second);
                contacts.emplace(key.first, key.second);
                if (mContacts.contains({ key.first, key.second }))
                    continue;
                auto& a = mItems.at(left->second);
                auto& b = mItems.at(right->second);
                const auto moving = [](const Item& value) {
                    return value.mActive
                        && (value.mBody->getLinearVelocity().length2() > 1
                            || value.mBody->getAngularVelocity().length2() > 0.01);
                };
                if (!a.mActive && moving(b))
                    wake.insert(left->second);
                if (!b.mActive && moving(a))
                    wake.insert(right->second);
            }
            mContacts = std::move(contacts);
            for (Id id : wake)
                activate(mItems.at(id));
        }

        ~Impl()
        {
            for (const auto& [id, item] : mItems)
                mWorld.removeRigidBody(item.mBody.get());
            for (const auto& object : mStatics)
                mWorld.removeCollisionObject(object.get());
        }
    };

    ItemSimulation::ItemSimulation()
        : ItemSimulation(Settings{})
    {
    }
    ItemSimulation::ItemSimulation(const Settings& settings)
        : mImpl(std::make_unique<Impl>(settings))
    {
    }
    ItemSimulation::~ItemSimulation() = default;

    void ItemSimulation::add(Id id, const btCollisionShape& source, const btTransform& transform, btScalar mass)
    {
        if (!std::isfinite(mass) || mass <= 0 || !finite(transform.getOrigin()))
            throw std::invalid_argument("Invalid loose item mass/transform");
        remove(id, false);
        std::vector<btVector3> points;
        vertices(source, btTransform::getIdentity(), points);
        btVector3 min, max;
        source.getAabb(btTransform::getIdentity(), min, max);
        if (!finite(min) || !finite(max))
            throw std::invalid_argument("Invalid loose item bounds");
        if (points.empty() || points.size() > 16384)
        {
            points.clear();
            for (int i = 0; i < 8; ++i)
                points.emplace_back(i & 1 ? max.x() : min.x(), i & 2 ? max.y() : min.y(), i & 4 ? max.z() : min.z());
        }
        Impl::Item item;
        item.mMass = std::clamp(mass, btScalar(0.05), btScalar(5000));
        item.mOffset = btTransform::getIdentity();
        item.mOffset.setOrigin((min + max) * 0.5);
        item.mShape = std::make_unique<btConvexHullShape>();
        item.mShape->setMargin(0.05);
        for (const auto& point : points)
            item.mShape->addPoint(point - item.mOffset.getOrigin(), false);
        item.mShape->recalcLocalAabb();
        item.mShape->optimizeConvexHull();
        btVector3 inertia;
        item.mShape->calculateLocalInertia(item.mMass, inertia);
        item.mInertia = inertia;
        btRigidBody::btRigidBodyConstructionInfo info(item.mMass, nullptr, item.mShape.get(), inertia);
        info.m_startWorldTransform = transform * item.mOffset;
        info.m_friction = 0.5;
        info.m_restitution = 0.08;
        info.m_linearDamping = 0.025;
        info.m_angularDamping = 0.12;
        item.mBody = std::make_unique<btRigidBody>(info);
        item.mBody->setSleepingThresholds(1, 0.1);
        const btVector3 extent = (max - min) * 0.5;
        const btScalar radius = std::max(btScalar(0.1), std::min({ extent.x(), extent.y(), extent.z() }) * 0.8);
        item.mBody->setCcdMotionThreshold(radius * 0.5);
        item.mBody->setCcdSweptSphereRadius(radius);
        item.mLastPose = transform;
        // Authored placement is fixed until a real external impact or drop.
        // The same hull remains queryable and supports neighbouring objects.
        item.mBody->setMassProps(0, btVector3(0, 0, 0));
        item.mBody->updateInertiaTensor();
        item.mBody->setActivationState(ISLAND_SLEEPING);
        mImpl->mWorld.addRigidBody(item.mBody.get(), 2, 1 | 2);
        mImpl->mBodyIds.emplace(item.mBody.get(), id);
        mImpl->mItems.emplace(id, std::move(item));
    }

    void ItemSimulation::remove(Id id, bool wakeNeighbours)
    {
        const auto it = mImpl->mItems.find(id);
        if (it == mImpl->mItems.end()) return;
        btVector3 min, max;
        it->second.mBody->getAabb(min, max);
        const btVector3 center = (min + max) * 0.5;
        const btScalar radius = (max - min).length() * 0.5 + 16;
        for (auto& [otherId, item] : mImpl->mItems)
            if (wakeNeighbours && otherId != id && item.mBody->getCenterOfMassPosition().distance(center) < radius)
                mImpl->activate(item);
        mImpl->mWorld.removeRigidBody(it->second.mBody.get());
        mImpl->mBodyIds.erase(it->second.mBody.get());
        mImpl->mActiveIds.erase(id);
        mImpl->mDirtyIds.erase(id);
        std::erase_if(mImpl->mContacts, [id](const auto& pair) { return pair.first == id || pair.second == id; });
        for (auto& [source, contacts] : mImpl->mKinematicContacts)
            contacts.erase(id);
        mImpl->mItems.erase(it);
    }
    bool ItemSimulation::contains(Id id) const { return mImpl->mItems.contains(id); }
    void ItemSimulation::move(Id id, const btTransform& transform)
    {
        const auto it = mImpl->mItems.find(id);
        if (it == mImpl->mItems.end()) return;
        auto& item = it->second;
        item.mBody->setWorldTransform(transform * item.mOffset);
        item.mBody->setLinearVelocity(btVector3(0, 0, 0));
        item.mBody->setAngularVelocity(btVector3(0, 0, 0));
        item.mLastPose = transform;
        mImpl->mWorld.updateSingleAabb(item.mBody.get());
    }
    void ItemSimulation::wake(Id id)
    {
        if (auto it = mImpl->mItems.find(id); it != mImpl->mItems.end())
            mImpl->activate(it->second);
    }
    void ItemSimulation::impulse(Id id, const btVector3& momentum, const btVector3& point)
    {
        const auto it = mImpl->mItems.find(id);
        if (it == mImpl->mItems.end() || !finite(momentum) || !finite(point) || momentum.fuzzyZero())
            return;
        mImpl->activate(it->second);
        auto& body = *it->second.mBody;
        body.applyImpulse(momentum, point - body.getCenterOfMassPosition());
        if (body.getLinearVelocity().length2() > 1200 * 1200)
            body.setLinearVelocity(body.getLinearVelocity().normalized() * 1200);
        if (body.getAngularVelocity().length2() > 25 * 25)
            body.setAngularVelocity(body.getAngularVelocity().normalized() * 25);
        mImpl->mDirtyIds.insert(id);
    }
    bool ItemSimulation::hasActiveBodies() const
    {
        return mImpl->activeCount() != 0;
    }
    void ItemSimulation::pushKinematic(
        const btConvexShape& shape, const btTransform& from, const btTransform& to, btScalar duration, Id source)
    {
        if (!std::isfinite(duration) || duration <= 0 || !finite(from.getOrigin()) || !finite(to.getOrigin()))
            return;
        const auto velocity = (to.getOrigin() - from.getOrigin()) / duration;
        if (velocity.length2() > 1500 * 1500)
        {
            mImpl->mKinematicContacts.erase(source);
            return;
        }
        if (velocity.length2() < 1)
            return;
        struct Hit
        {
            Id mId;
            btVector3 mPoint;
            btVector3 mNormal;
        };
        struct Sweep : btCollisionWorld::ConvexResultCallback
        {
            const std::map<const btCollisionObject*, Id>& mIds;
            std::vector<Hit> mHits;
            explicit Sweep(const std::map<const btCollisionObject*, Id>& ids)
                : mIds(ids)
            {
                m_collisionFilterGroup = 4;
                m_collisionFilterMask = 2;
            }
            bool needsCollision(btBroadphaseProxy* proxy) const override
            {
                return mIds.contains(static_cast<const btCollisionObject*>(proxy->m_clientObject));
            }
            btScalar addSingleResult(btCollisionWorld::LocalConvexResult& result, bool worldNormal) override
            {
                const auto normal = worldNormal
                    ? result.m_hitNormalLocal
                    : result.m_hitCollisionObject->getWorldTransform().getBasis() * result.m_hitNormalLocal;
                mHits.push_back({ mIds.at(result.m_hitCollisionObject), result.m_hitPointLocal, normal });
                return 1; // Collect every real item contact along the resolved movement.
            }
        } sweep(mImpl->mBodyIds);
        mImpl->mWorld.convexSweepTest(&shape, from, to, sweep);
        std::set<Id> pushed;
        const auto previous = mImpl->mKinematicContacts[source];
        for (const auto& hit : sweep.mHits)
        {
            if (!pushed.insert(hit.mId).second)
                continue;
            const auto& item = mImpl->mItems.at(hit.mId);
            if (!item.mActive && previous.contains(hit.mId))
                continue;
            const auto relative
                = velocity - item.mBody->getVelocityInLocalPoint(hit.mPoint - item.mBody->getCenterOfMassPosition());
            const auto closing = std::max(btScalar(0), -relative.dot(hit.mNormal));
            const auto tangent = relative - hit.mNormal * relative.dot(hit.mNormal);
            const auto momentum = (-hit.mNormal * closing + tangent * 0.15) * std::min(item.mMass, btScalar(70));
            impulse(hit.mId, momentum, hit.mPoint);
        }
        mImpl->mKinematicContacts[source] = std::move(pushed);
    }
    void ItemSimulation::forgetKinematic(Id source)
    {
        mImpl->mKinematicContacts.erase(source);
    }
    std::vector<ItemSimulation::Pose> ItemSimulation::activePoses() const
    {
        std::vector<Pose> result;
        for (const auto id : mImpl->mActiveIds)
        {
            const auto& item = mImpl->mItems.at(id);
            result.push_back({ id, item.mBody->getWorldTransform() * item.mOffset.inverse() });
        }
        return result;
    }
    std::vector<ItemSimulation::Pose> ItemSimulation::advance(btScalar duration)
    {
        std::vector<Pose> result;
        if (!std::isfinite(duration) || duration <= 0) return result;
        const btScalar dt = std::min(duration, btScalar(0.1));
        // Run wake detection each substep before contact resolution. Using
        // Bullet's variable step here avoids silently dropping small frame times.
        const int steps = std::max(1, static_cast<int>(std::ceil(dt * 120)));
        for (int step = 0; step < steps; ++step)
        {
            mImpl->wakeContacts();
            mImpl->mWorld.stepSimulation(dt / steps, 0);
        }
        mImpl->mClock += duration;
        auto affected = mImpl->mActiveIds;
        affected.insert(mImpl->mDirtyIds.begin(), mImpl->mDirtyIds.end());
        for (const auto id : affected)
        {
            auto& item = mImpl->mItems.at(id);
            mImpl->mDirtyIds.insert(id);
            const auto pose = item.mBody->getWorldTransform() * item.mOffset.inverse();
            if (!finite(pose.getOrigin()) || !finite(item.mBody->getLinearVelocity()))
            {
                move(id, item.mLastPose);
                mImpl->freeze(item);
                continue;
            }
            if (item.mActive && (mImpl->mClock + 1e-6 >= item.mDeadline || !item.mBody->isActive()))
                mImpl->freeze(item);
            if (pose == item.mLastPose) continue;
            item.mLastPose = pose;
            result.push_back({ id, pose });
        }
        return result;
    }
    void ItemSimulation::elapseWithoutPhysics(btScalar duration)
    {
        if (!std::isfinite(duration) || duration <= 0)
            return;
        mImpl->mClock += duration;
        const auto active = mImpl->mActiveIds;
        for (const auto id : active)
        {
            auto& item = mImpl->mItems.at(id);
            if (mImpl->mClock + 1e-6 >= item.mDeadline)
                mImpl->freeze(item);
            mImpl->mDirtyIds.insert(id);
        }
    }
    void ItemSimulation::replaceStaticColliders(std::vector<StaticCollider> colliders)
    {
        for (auto& object : mImpl->mStatics) mImpl->mWorld.removeCollisionObject(object.get());
        mImpl->mStatics.clear();
        mImpl->mColliders = std::move(colliders);
        for (const auto& collider : mImpl->mColliders)
        {
            auto object = std::make_unique<btCollisionObject>();
            object->setCollisionShape(collider.mShape.get());
            object->setWorldTransform(collider.mTransform);
            object->setFriction(collider.mFriction);
            mImpl->mWorld.addCollisionObject(object.get(), 1, 2);
            mImpl->mStatics.push_back(std::move(object));
        }
    }
    void ItemSimulation::setObserverPosition(const btVector3& position)
    {
        if (finite(position))
            mImpl->mObserver = position;
    }
    std::size_t ItemSimulation::activeCount() const
    {
        return mImpl->activeCount();
    }
    std::vector<ItemSimulation::Id> ItemSimulation::takeStateChanges()
    {
        std::vector<Id> result(mImpl->mDirtyIds.begin(), mImpl->mDirtyIds.end());
        mImpl->mDirtyIds.clear();
        return result;
    }
    ItemSimulation::State ItemSimulation::state(Id id) const
    {
        const auto& item = mImpl->mItems.at(id);
        return { item.mBody->getWorldTransform() * item.mOffset.inverse(), item.mBody->getLinearVelocity(),
            item.mBody->getAngularVelocity(),
            item.mActive ? std::max(btScalar(0), item.mDeadline - mImpl->mClock) : btScalar(0), item.mActive };
    }
    void ItemSimulation::restore(Id id, const State& value)
    {
        const auto it = mImpl->mItems.find(id);
        if (it == mImpl->mItems.end())
            return;
        if (!finite(value.mTransform.getOrigin()) || !finite(value.mLinearVelocity) || !finite(value.mAngularVelocity)
            || !std::isfinite(value.mTimeLeft) || value.mTimeLeft < 0)
            throw std::invalid_argument("Invalid saved item physics state");
        move(id, value.mTransform);
        auto& item = it->second;
        mImpl->freeze(item);
        if (value.mActive && value.mTimeLeft > 0)
        {
            mImpl->activate(item);
            item.mDeadline = mImpl->mClock + std::min(value.mTimeLeft, mImpl->mSettings.mMaxSimulationTime);
            item.mBody->setLinearVelocity(value.mLinearVelocity);
            item.mBody->setAngularVelocity(value.mAngularVelocity);
        }
    }
    btVector3 ItemSimulation::velocity(Id id) const { return mImpl->mItems.at(id).mBody->getLinearVelocity(); }
    btScalar ItemSimulation::mass(Id id) const { return mImpl->mItems.at(id).mMass; }
}
