#ifndef OPENMW_COMPONENTS_RAGDOLL_SIMULATION_H
#define OPENMW_COMPONENTS_RAGDOLL_SIMULATION_H

#include <LinearMath/btTransform.h>

#include <cstddef>
#include <memory>
#include <vector>

class btCollisionShape;

namespace Ragdoll
{
    // Coordinates and velocities use OpenMW world units, with Z pointing up.
    // A capsule's cylindrical section extends along its local Y axis.
    struct BodyDefinition
    {
        btTransform mTransform = btTransform::getIdentity();
        btScalar mRadius = 1;
        btScalar mLength = 1;
        btScalar mMass = 1;
    };

    struct JointDefinition
    {
        std::size_t mParent = 0;
        std::size_t mChild = 0;
        // Both joint frames coincide here in the initial pose. Angular bounds
        // are in radians relative to that pose; translation is always locked.
        btTransform mFrameWorld = btTransform::getIdentity();
        btVector3 mAngularLower{ -0.5, -0.5, -0.5 };
        btVector3 mAngularUpper{ 0.5, 0.5, 0.5 };
    };

    struct StaticCollider
    {
        // The source shape and any mesh data referenced by it must remain
        // immutable. An aliasing shared_ptr can retain their common owner.
        std::shared_ptr<btCollisionShape> mShape;
        btTransform mTransform = btTransform::getIdentity();
        btScalar mFriction = 0.8;
    };

    struct Settings
    {
        btVector3 mGravity{ 0, 0, -627.2 };
        btScalar mFixedTimeStep = 1.0 / 120.0;
        // A long stall must not cause an unbounded physics catch-up.
        btScalar mMaxFrameTime = 0.1;
        btScalar mLinearDamping = 0.15;
        btScalar mAngularDamping = 0.65;
        btScalar mMaxLinearSpeed = 2000;
        btScalar mMaxAngularSpeed = 25;
    };

    // Each corpse has a separate dynamics world. It never inserts rigid bodies
    // into OpenMW's concurrently accessed, collision-only physics world.
    class Simulation
    {
    public:
        Simulation(const std::vector<BodyDefinition>& bodies, const std::vector<JointDefinition>& joints,
            std::vector<StaticCollider> colliders, const Settings& settings = {});
        ~Simulation();

        Simulation(const Simulation&) = delete;
        Simulation& operator=(const Simulation&) = delete;

        // Invalid input definitions throw std::invalid_argument. Runtime
        // divergence freezes the last finite pose and makes advance return false.
        // Zero/negative dt is a no-op, allowing the game to pause normally.
        bool advance(btScalar dt);
        void replaceStaticColliders(std::vector<StaticCollider> colliders);
        void applyImpulse(std::size_t body, const btVector3& impulse, const btVector3& worldPoint);

        std::size_t getNumBodies() const;
        const btTransform& getBodyTransform(std::size_t body) const;
        bool isSleeping() const;
        bool isValid() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
