#ifndef OPENMW_COMPONENTS_RAGDOLL_SIMULATION_H
#define OPENMW_COMPONENTS_RAGDOLL_SIMULATION_H

#include <LinearMath/btTransform.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

class btCollisionShape;

namespace Ragdoll
{
    enum class BodyMode { Articulated, Soft, Rigid };

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
        enum class Type { Ball, Hinge };

        std::size_t mParent = 0;
        std::size_t mChild = 0;
        // Both joint frames coincide here in the initial pose. Angular bounds
        // are in radians relative to that pose; translation is always locked.
        btTransform mFrameWorld = btTransform::getIdentity();
        // Anatomical frames need not share an orientation at creation: their
        // relative rotation describes the already bent limb in the captured pose.
        bool mUseLocalFrames = false;
        btTransform mFrameInParent = btTransform::getIdentity();
        btTransform mFrameInChild = btTransform::getIdentity();
        Type mType = Type::Ball;
        // A hinge uses only Z bounds and rotates around its frame's Z axis.
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
        BodyMode mBodyMode = BodyMode::Articulated;
        btVector3 mGravity{ 0, 0, -627.2 };
        btScalar mFixedTimeStep = 1.0 / 120.0;
        // A long stall must not cause an unbounded physics catch-up.
        btScalar mMaxFrameTime = 0.1;
        btScalar mLinearDamping = 0.04;
        btScalar mAngularDamping = 0.4;
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
        // All segments receive the same change in velocity; total momentum is
        // exactly the supplied impulse, independent of the mass distribution.
        void applyWholeBodyImpulse(const btVector3& impulse);
        // A finite-duration, mass-dependent spring/drag field. Forces are
        // distributed by segment mass, keeping the skeleton connected.
        void attract(
            const btVector3& target, btScalar referenceDistance, btScalar referenceMass, btScalar duration = 2);
        struct AttractionState
        {
            btVector3 mTarget{ 0, 0, 0 };
            btScalar mDistance = 0, mReferenceMass = 70, mDuration = 2, mTimeLeft = 0;
        };
        AttractionState attractionState() const;
        void restoreAttraction(const AttractionState& state);
        void setBodyVelocity(std::size_t part, const btVector3& linear, const btVector3& angular);
        void setWaterLevel(std::optional<btScalar> level);
        bool isBelowWaterSurface() const;
        void setTotalMass(btScalar mass);
        // Equal and opposite torque impulses, without a net linear launch.
        void applyJointTorqueImpulse(std::size_t joint, const btVector3& impulse);
        // Detach a joint while retaining all bodies and their remaining joints.
        void detachJoint(std::size_t joint);
        bool raycastStatic(const btVector3& from, const btVector3& to,
            btVector3& point, btVector3& normal) const;
        bool sweepPartStatic(
            std::size_t part, const btVector3& displacement, btVector3& point, btVector3& normal) const;
        // Anchors the current body point to the world without teleportation.
        // One pin per corpse allows the remaining articulated limbs to hang.
        bool pinBody(std::size_t body, const btVector3& worldPoint);
        bool isPinned() const;

        std::size_t getNumBodies() const;
        const btTransform& getBodyTransform(std::size_t body) const;
        btVector3 getBodyLinearVelocity(std::size_t body) const;
        const btVector3& getBodyAngularVelocity(std::size_t body) const;
        btScalar getBodyMass(std::size_t body) const;
        btScalar getTotalMass() const;
        // Only meaningful for Type::Hinge; other joint types return zero.
        btScalar getJointAngle(std::size_t joint) const;
        bool isSleeping() const;
        bool isValid() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
