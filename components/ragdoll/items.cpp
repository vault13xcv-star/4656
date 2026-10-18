#include "items.hpp"

#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionShapes/btShapeHull.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

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
        };
        std::map<Id, Item> mItems;
        std::vector<StaticCollider> mColliders;
        std::vector<std::unique_ptr<btCollisionObject>> mStatics;

        Impl()
        {
            mWorld.setGravity(btVector3(0, 0, -627.2));
            mWorld.getSolverInfo().m_numIterations = 20;
        }
        ~Impl()
        {
            for (const auto& [id, item] : mItems)
                mWorld.removeRigidBody(item.mBody.get());
            for (const auto& object : mStatics)
                mWorld.removeCollisionObject(object.get());
        }
    };

    ItemSimulation::ItemSimulation() : mImpl(std::make_unique<Impl>()) {}
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
        mImpl->mWorld.addRigidBody(item.mBody.get(), 2, 1 | 2);
        // Preserve authored shelf arrangements until an impact, drop, or contact.
        item.mBody->setActivationState(ISLAND_SLEEPING);
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
        for (const auto& [otherId, item] : mImpl->mItems)
            if (wakeNeighbours && otherId != id && item.mBody->getCenterOfMassPosition().distance(center) < radius)
                item.mBody->activate(true);
        mImpl->mWorld.removeRigidBody(it->second.mBody.get());
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
        if (auto it = mImpl->mItems.find(id); it != mImpl->mItems.end()) it->second.mBody->activate(true);
    }
    void ItemSimulation::impulse(Id id, const btVector3& momentum, const btVector3& point)
    {
        const auto it = mImpl->mItems.find(id);
        if (it == mImpl->mItems.end() || !finite(momentum) || !finite(point)) return;
        auto& body = *it->second.mBody;
        body.activate(true);
        body.applyImpulse(momentum, point - body.getCenterOfMassPosition());
        if (body.getLinearVelocity().length2() > 1200 * 1200)
            body.setLinearVelocity(body.getLinearVelocity().normalized() * 1200);
        if (body.getAngularVelocity().length2() > 25 * 25)
            body.setAngularVelocity(body.getAngularVelocity().normalized() * 25);
    }
    bool ItemSimulation::hasActiveBodies() const
    {
        return std::any_of(mImpl->mItems.begin(), mImpl->mItems.end(),
            [](const auto& entry) { return entry.second.mBody->isActive(); });
    }
    std::vector<ItemSimulation::Pose> ItemSimulation::activePoses() const
    {
        std::vector<Pose> result;
        for (const auto& [id, item] : mImpl->mItems)
            if (item.mBody->isActive()) result.push_back({ id, item.mBody->getWorldTransform() * item.mOffset.inverse() });
        return result;
    }
    std::vector<ItemSimulation::Pose> ItemSimulation::advance(btScalar duration)
    {
        std::vector<Pose> result;
        if (!std::isfinite(duration) || duration <= 0) return result;
        mImpl->mWorld.stepSimulation(std::min(duration, btScalar(0.1)), 12, 1.0 / 120.0);
        for (auto& [id, item] : mImpl->mItems)
        {
            const auto pose = item.mBody->getWorldTransform() * item.mOffset.inverse();
            if (!finite(pose.getOrigin()) || !finite(item.mBody->getLinearVelocity()))
            {
                move(id, item.mLastPose);
                item.mBody->setActivationState(ISLAND_SLEEPING);
                continue;
            }
            if (pose == item.mLastPose) continue;
            item.mLastPose = pose;
            result.push_back({ id, pose });
        }
        return result;
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
    btVector3 ItemSimulation::velocity(Id id) const { return mImpl->mItems.at(id).mBody->getLinearVelocity(); }
    btScalar ItemSimulation::mass(Id id) const { return mImpl->mItems.at(id).mMass; }
}
