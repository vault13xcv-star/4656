#ifndef OPENMW_COMPONENTS_RAGDOLL_ITEMS_HPP
#define OPENMW_COMPONENTS_RAGDOLL_ITEMS_HPP

#include "simulation.hpp"
#include <cstdint>

class btConvexShape;

namespace Ragdoll
{
    // Separate, main-thread dynamics world. Item IDs are owned by the caller.
    // Sleeping items stay in the world and can be woken by another moving item.
    class ItemSimulation
    {
    public:
        using Id = std::uintptr_t;
        struct Pose { Id mId; btTransform mTransform; };
        struct Settings
        {
            std::size_t mMaxActive = 100;
            btScalar mMaxSimulationTime = 10;
        };
        struct State
        {
            btTransform mTransform = btTransform::getIdentity();
            btVector3 mLinearVelocity{ 0, 0, 0 };
            btVector3 mAngularVelocity{ 0, 0, 0 };
            btScalar mTimeLeft = 0;
            bool mActive = false;
        };
        ItemSimulation();
        explicit ItemSimulation(const Settings& settings);
        ~ItemSimulation();
        void add(Id id, const btCollisionShape& shape, const btTransform& transform, btScalar mass);
        void remove(Id id, bool wakeNeighbours = true);
        bool contains(Id id) const;
        bool empty() const;
        void move(Id id, const btTransform& transform);
        void impulse(Id id, const btVector3& momentum, const btVector3& point);
        void wake(Id id);
        void setObserverPosition(const btVector3& position);
        std::size_t activeCount() const;
        State state(Id id) const;
        std::vector<Id> takeStateChanges();
        void restore(Id id, const State& state);
        void pushKinematic(const btConvexShape& shape, const btTransform& from, const btTransform& to,
            btScalar duration, Id source = 0);
        void forgetKinematic(Id source);
        bool hasActiveBodies() const;
        std::vector<Pose> activePoses() const;
        std::vector<Pose> advance(btScalar duration);
        void elapseWithoutPhysics(btScalar duration);
        void replaceStaticColliders(std::vector<StaticCollider> colliders);
        btVector3 velocity(Id id) const;
        btScalar mass(Id id) const;
    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
#endif
