#include "../mwmechanics/creaturestats.hpp"
#include "../mwworld/class.hpp"
#include "actor.hpp"
#include "mtphysics.hpp"
#include "object.hpp"
#include "physicssystem.hpp"
#include <BulletCollision/CollisionShapes/btBvhTriangleMeshShape.h>
#include <BulletCollision/CollisionShapes/btTriangleMesh.h>
#include <BulletCollision/CollisionShapes/btBoxShape.h>
#include <BulletCollision/CollisionShapes/btCompoundShape.h>
#include <components/debug/debuglog.hpp>
#include <components/misc/convert.hpp>
#include <components/ragdoll/items.hpp>
#include <components/resource/bulletshape.hpp>
#include <components/settings/values.hpp>
#include <set>

namespace MWPhysics
{
    void PhysicsSystem::addProjectileDebris(std::uintptr_t key, const osg::Vec3f& center,
        const osg::Vec3f& halfExtents, const osg::Vec3f& position, const osg::Quat& rotation,
        float mass, const osg::Vec3f& velocity)
    {
        if (!mItems)
            mItems=std::make_unique<Ragdoll::ItemSimulation>(Ragdoll::ItemSimulation::Settings{
                static_cast<std::size_t>(Settings::ragdoll().mMaxActiveItems.get()),Settings::ragdoll().mItemSimulationTime.get()});
        btBoxShape box(Misc::Convert::toBullet(halfExtents));
        btCompoundShape shape;
        auto offset=btTransform::getIdentity();offset.setOrigin(Misc::Convert::toBullet(center));
        shape.addChildShape(offset,&box);
        btTransform transform(Misc::Convert::toBullet(rotation),Misc::Convert::toBullet(position));
        mItems->add(key,shape,transform,std::max(.05f,mass));
        if (Settings::ragdoll().mItemPhysics)
        {
            mItems->wake(key);
            auto state=mItems->state(key);
            state.mLinearVelocity=Misc::Convert::toBullet(velocity);
            state.mAngularVelocity=btVector3(2,1,3);
            mItems->restore(key,state);
        }
        mItemSnapshotAge=1.f;
    }
    void PhysicsSystem::removeProjectileDebris(std::uintptr_t key)
    {
        if(mItems)mItems->remove(key,false);
    }
    bool PhysicsSystem::getProjectileDebrisPose(std::uintptr_t key, osg::Vec3f& position, osg::Quat& rotation) const
    {
        if(!mItems || !mItems->contains(key))return false;
        const auto transform=mItems->state(key).mTransform;
        position=Misc::Convert::toOsg(transform.getOrigin());
        rotation=Misc::Convert::toOsg(transform.getRotation());return true;
    }
    namespace
    {
        Ragdoll::ItemSimulation::Id id(const MWWorld::Ptr& ptr)
        {
            return reinterpret_cast<Ragdoll::ItemSimulation::Id>(ptr.mRef);
        }
        struct Mesh
        {
            btTriangleMesh mMesh;
            std::unique_ptr<btBvhTriangleMeshShape> mShape;
        };
        void storeItemState(const MWWorld::Ptr& ptr, const Ragdoll::ItemSimulation::State& state)
        {
            ESM::ItemPhysicsState saved;
            saved.mPresent = true;
            saved.mTimeLeft = state.mActive ? static_cast<float>(state.mTimeLeft) : 0.f;
            for (int axis = 0; axis < 3; ++axis)
            {
                saved.mLinearVelocity[axis] = state.mActive ? state.mLinearVelocity[axis] : 0.f;
                saved.mAngularVelocity[axis] = state.mActive ? state.mAngularVelocity[axis] : 0.f;
            }
            ptr.getRefData().setItemPhysicsState(saved);
        }
    }
    void PhysicsSystem::registerItem(const MWWorld::Ptr& ptr, const Object& object)
    {
        if (!Settings::ragdoll().mItemPhysics || !ptr.getClass().isItem(ptr))
            return;
        if (!mItems)
            mItems = std::make_unique<Ragdoll::ItemSimulation>(
                Ragdoll::ItemSimulation::Settings{ static_cast<std::size_t>(Settings::ragdoll().mMaxActiveItems.get()),
                    Settings::ragdoll().mItemSimulationTime.get() });
        try
        {
            const float scale = ptr.getCellRef().getScale();
            const float mass = std::max(0.05f, ptr.getClass().getWeight(ptr)) * std::max(1, ptr.getCellRef().getCount())
                * scale * scale * scale * Settings::ragdoll().mItemMassScale;
            const bool existing = mItems->contains(id(ptr));
            auto state = existing ? mItems->state(id(ptr)) : Ragdoll::ItemSimulation::State{};
            const auto& saved = ptr.getRefData().getItemPhysicsState();
            if (!existing && saved.mPresent)
            {
                state.mActive = saved.mTimeLeft > 0;
                state.mTimeLeft = saved.mTimeLeft;
                state.mLinearVelocity
                    = btVector3(saved.mLinearVelocity[0], saved.mLinearVelocity[1], saved.mLinearVelocity[2]);
                state.mAngularVelocity
                    = btVector3(saved.mAngularVelocity[0], saved.mAngularVelocity[1], saved.mAngularVelocity[2]);
            }
            state.mTransform = object.getTransform();
            mItems->add(id(ptr), *object.getShapeInstance()->mCollisionShape, object.getTransform(), mass);
            if ((existing && state.mActive) || saved.mPresent)
                mItems->restore(id(ptr), state);
        }
        catch (const std::exception& error)
        {
            Log(Debug::Warning) << "Loose item physics unavailable for " << ptr.getCellRef().getRefId() << ": "
                                << error.what();
        }
    }
    void PhysicsSystem::applyItemImpulse(const MWWorld::Ptr& ptr, const osg::Vec3f& impulse, const osg::Vec3f& point,
        float strengthScale)
    {
        if (!mItems || !Settings::ragdoll().mItemPhysics || ptr.isEmpty())
            return;
        // Native drop paths request gravity alone with an explicit zero impulse.
        // A disabled impulse multiplier must not turn other hits into drops.
        if (impulse == osg::Vec3f())
            mItems->wake(id(ptr));
        else
            mItems->impulse(id(ptr), Misc::Convert::toBullet(impulse * Settings::ragdoll().mItemImpulseScale.get()),
                Misc::Convert::toBullet(point), strengthScale);
        syncItemStates();
        mItemSnapshotAge = 1.f;
    }
    void PhysicsSystem::applyItemExplosion(const osg::Vec3f& origin, float radius, float strength, float strengthScale)
    {
        if (!mItems || !Settings::ragdoll().mItemPhysics || radius <= 0 || strength <= 0)
            return;
        for (const auto& [key, object] : mObjects)
        {
            const auto ptr = object->getPtr();
            if (!mItems->contains(id(ptr)))
                continue;
            const auto box = getBoundingBox(ptr);
            const osg::Vec3f center = box.valid() ? box.center() : ptr.getRefData().getPosition().asVec3();
            osg::Vec3f direction = center - origin;
            const float distance = direction.normalize();
            if (distance >= radius)
                continue;
            if (distance < 0.01f)
                direction.set(0, 0, 1);
            const auto occlusion = castRay(origin + direction * 2.f, center, { ptr }, {},
                CollisionType_World | CollisionType_Door | CollisionType_HeightMap);
            if (occlusion.mHit && (occlusion.mHitPos - center).length() > 2.f)
                continue;
            direction.z() += 0.2f;
            direction.normalize();
            applyItemImpulse(ptr, direction * (strength * (1.f - distance / radius)), center, strengthScale);
        }
    }
    void PhysicsSystem::updateItems(float duration,
        const std::function<void(const MWWorld::Ptr&, const osg::Vec3f&, const osg::Quat&)>& updateObject,
        const osg::Vec3f& observer)
    {
        if (!mItems || !Settings::ragdoll().mItemPhysics || duration <= 0)
            return;
        mItems->setSettings({ static_cast<std::size_t>(Settings::ragdoll().mMaxActiveItems.get()),
            Settings::ragdoll().mItemSimulationTime.get() });
        mItems->setObserverPosition(Misc::Convert::toBullet(observer));
        std::map<const MWWorld::LiveCellRefBase*, osg::Vec3f> positions;
        for (const auto& [key, actor] : mActors)
        {
            if (actor->getPtr().getClass().getCreatureStats(actor->getPtr()).isDead())
                continue;
            const auto position = actor->getCollisionObjectPosition();
            positions.emplace(key, position);
            if (const auto previous = mItemActorPositions.find(key); previous != mItemActorPositions.end())
            {
                auto from = btTransform::getIdentity(), to = from;
                const auto rotation = Misc::Convert::toBullet(
                    Misc::Convert::makeOsgQuat(actor->getPtr().getRefData().getPosition().rot));
                from.setRotation(rotation);
                to.setRotation(rotation);
                from.setOrigin(Misc::Convert::toBullet(previous->second));
                to.setOrigin(Misc::Convert::toBullet(position));
                mItems->pushKinematic(
                    *actor->getConvexShape(), from, to, duration, reinterpret_cast<Ragdoll::ItemSimulation::Id>(key));
            }
        }
        mItemActorPositions = std::move(positions);
        if (!mItems->hasActiveBodies())
            return;
        std::set<std::array<int, 3>> tiles;
        for (const auto& pose : mItems->activePoses())
        {
            const auto p = pose.mTransform.getOrigin();
            tiles.insert(
                { int(std::floor(p.x() / 1024)), int(std::floor(p.y() / 1024)), int(std::floor(p.z() / 1024)) });
        }
        std::vector<osg::Vec3f> regions;
        for (const auto& tile : tiles)
            regions.emplace_back(tile[0] * 1024.f + 512, tile[1] * 1024.f + 512, tile[2] * 1024.f + 512);
        mItemSnapshotAge += duration;
        if (regions != mItemRegions || mItemSnapshotAge >= 0.25f)
        {
            // Copy static geometry under the scheduler lock; never run a second
            // solver on OpenMW's concurrently accessed collision-only world.
            std::set<std::array<float, 9>> triangles;
            for (const auto& center : regions)
            {
                const auto vertices = mTaskScheduler->getRagdollCollisionTriangles(center, 1024, true);
                if (vertices.empty())
                {
                    // Hold the last safe pose, but do not restart/extend timers
                    // when geometry is unavailable in a loaded cell.
                    mItems->elapseWithoutPhysics(duration);
                    syncItemStates();
                    return;
                }
                for (std::size_t i = 0; i + 2 < vertices.size(); i += 3)
                    triangles.insert(
                        { vertices[i].x(), vertices[i].y(), vertices[i].z(), vertices[i + 1].x(), vertices[i + 1].y(),
                            vertices[i + 1].z(), vertices[i + 2].x(), vertices[i + 2].y(), vertices[i + 2].z() });
            }
            std::vector<osg::Vec3f> vertices;
            for (const auto& t : triangles)
                for (int i = 0; i < 9; i += 3)
                    vertices.emplace_back(t[i], t[i + 1], t[i + 2]);
            if (vertices != mItemCollisionVertices)
            {
                auto mesh = std::make_shared<Mesh>();
                for (std::size_t i = 0; i + 2 < vertices.size(); i += 3)
                    mesh->mMesh.addTriangle(Misc::Convert::toBullet(vertices[i]),
                        Misc::Convert::toBullet(vertices[i + 1]), Misc::Convert::toBullet(vertices[i + 2]));
                mesh->mShape = std::make_unique<btBvhTriangleMeshShape>(&mesh->mMesh, true);
                Ragdoll::StaticCollider collider;
                collider.mShape = std::shared_ptr<btCollisionShape>(mesh, mesh->mShape.get());
                mItems->replaceStaticColliders({ collider });
                mItemCollisionVertices = std::move(vertices);
            }
            mItemRegions = std::move(regions);
            mItemSnapshotAge = 0;
        }
        const auto poses = mItems->advance(duration);
        // Persist the remaining interval even if the final transform did not
        // change (sleep/timeout). Unloaded cells then resume without a new timer.
        syncItemStates();
        // World operations preserve reference identity, cell transfers, scripts,
        // picking, and native saved position/rotation. They may erase mObjects.
        mSynchronizingItems = true;
        try
        {
            for (const auto& pose : poses)
            {
                const auto it = mObjects.find(reinterpret_cast<const MWWorld::LiveCellRefBase*>(pose.mId));
                if (it == mObjects.end())
                    continue;
                const auto ptr = it->second->getPtr();
                updateObject(ptr, Misc::Convert::toOsg(pose.mTransform.getOrigin()),
                    Misc::Convert::toOsg(pose.mTransform.getRotation()));
            }
        }
        catch (...)
        {
            mSynchronizingItems = false;
            throw;
        }
        mSynchronizingItems = false;
    }
    void PhysicsSystem::syncItemStates()
    {
        if (!mItems)
            return;
        for (const auto stateId : mItems->takeStateChanges())
        {
            const auto it = mObjects.find(reinterpret_cast<const MWWorld::LiveCellRefBase*>(stateId));
            if (it != mObjects.end() && mItems->contains(stateId))
                storeItemState(it->second->getPtr(), mItems->state(stateId));
        }
    }
}
