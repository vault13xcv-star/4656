#ifndef OPENMW_COMPONENTS_RAGDOLL_ITEMS_HPP
#define OPENMW_COMPONENTS_RAGDOLL_ITEMS_HPP

#include "simulation.hpp"
#include <cstdint>

namespace Ragdoll
{
    // Separate, main-thread dynamics world. Item IDs are owned by the caller.
    // Sleeping items stay in the world and can be woken by another moving item.
    class ItemSimulation
    {
    public:
        using Id = std::uintptr_t;
        struct Pose { Id mId; btTransform mTransform; };
        ItemSimulation();
        ~ItemSimulation();
        void add(Id id, const btCollisionShape& shape, const btTransform& transform, btScalar mass);
        void remove(Id id, bool wakeNeighbours = true);
        bool contains(Id id) const;
        void move(Id id, const btTransform& transform);
        void impulse(Id id, const btVector3& momentum, const btVector3& point);
        void wake(Id id);
        bool hasActiveBodies() const;
        std::vector<Pose> activePoses() const;
        std::vector<Pose> advance(btScalar duration);
        void replaceStaticColliders(std::vector<StaticCollider> colliders);
        btVector3 velocity(Id id) const;
        btScalar mass(Id id) const;
    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
#endif
