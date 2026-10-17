#include "../simulation.hpp"

#include <btBulletDynamicsCommon.h>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    void check(bool condition, const std::string& message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    template <class F>
    void checkInvalid(F&& operation, const std::string& message)
    {
        try
        {
            operation();
        }
        catch (const std::invalid_argument&)
        {
            return;
        }
        throw std::runtime_error(message);
    }

    Ragdoll::BodyDefinition capsule(const btVector3& center, btScalar radius = 2, btScalar length = 0)
    {
        Ragdoll::BodyDefinition result;
        result.mTransform.setOrigin(center);
        result.mRadius = radius;
        result.mLength = length;
        result.mMass = 1;
        return result;
    }

    Ragdoll::StaticCollider plane(btScalar height = 0)
    {
        Ragdoll::StaticCollider result;
        result.mShape = std::make_shared<btStaticPlaneShape>(btVector3(0, 0, 1), 0);
        result.mTransform.setOrigin(btVector3(0, 0, height));
        return result;
    }

    void runFor(Ragdoll::Simulation& simulation, btScalar seconds, btScalar frame = 1.0 / 60)
    {
        for (int i = 0; i < static_cast<int>(std::ceil(seconds / frame)); ++i)
            check(simulation.advance(frame), "Physics simulation became non-finite");
    }

    void testPlaneAndSleep()
    {
        Ragdoll::Simulation simulation({ capsule(btVector3(0, 0, 100)) }, {}, { plane() });
        runFor(simulation, 8);
        const btVector3 origin = simulation.getBodyTransform(0).getOrigin();
        check(btFabs(origin.z() - 2) < 0.15, "Capsule did not settle on the collision plane");
        check(simulation.isSleeping(), "A resting rigid body failed to sleep");
        simulation.applyImpulse(0, btVector3(10, 0, 30), origin);
        check(!simulation.isSleeping(), "Impulse did not reactivate the rigid body");
        runFor(simulation, 0.1);
        check(simulation.getBodyTransform(0).getOrigin().x() > origin.x(), "Impulse produced no physical motion");
    }

    void testStairs()
    {
        std::vector<Ragdoll::StaticCollider> colliders{ plane() };
        std::vector<Ragdoll::BodyDefinition> bodies;
        for (int i = 0; i < 3; ++i)
        {
            const btScalar x = (i - 1) * 30;
            const btScalar height = (i + 1) * 10;
            Ragdoll::StaticCollider step;
            step.mShape = std::make_shared<btBoxShape>(btVector3(14, 20, height / 2));
            step.mTransform.setOrigin(btVector3(x, 0, height / 2));
            colliders.push_back(std::move(step));
            bodies.push_back(capsule(btVector3(x, 0, 100)));
        }
        Ragdoll::Simulation simulation(bodies, {}, std::move(colliders));
        runFor(simulation, 8);
        for (std::size_t i = 0; i < bodies.size(); ++i)
        {
            const btScalar expected = (i + 1) * 10 + bodies[i].mRadius;
            check(btFabs(simulation.getBodyTransform(i).getOrigin().z() - expected) < 0.2,
                "A body fell through or floated above a stair step");
        }
    }

    struct TriangleOwner
    {
        btTriangleMesh mMesh;
        std::unique_ptr<btBvhTriangleMeshShape> mShape;

        TriangleOwner()
        {
            mMesh.addTriangle(btVector3(-100, -100, 0), btVector3(100, -100, 0), btVector3(100, 100, 0));
            mMesh.addTriangle(btVector3(-100, -100, 0), btVector3(100, 100, 0), btVector3(-100, 100, 0));
            mShape = std::make_unique<btBvhTriangleMeshShape>(&mMesh, true);
        }
    };

    void testMeshLifetimeAndSnapshotReplacement()
    {
        auto owner = std::make_shared<TriangleOwner>();
        std::weak_ptr<TriangleOwner> weakOwner = owner;
        Ragdoll::StaticCollider collider;
        collider.mShape = std::shared_ptr<btCollisionShape>(owner, owner->mShape.get());
        Ragdoll::Simulation simulation({ capsule(btVector3(0, 0, 50)) }, {}, { collider });
        collider.mShape.reset();
        owner.reset();
        check(!weakOwner.expired(), "Simulation did not retain its static triangle mesh owner");
        runFor(simulation, 8);
        check(btFabs(simulation.getBodyTransform(0).getOrigin().z() - 2) < 0.2,
            "Capsule did not collide with an immutable triangle mesh snapshot");
        simulation.replaceStaticColliders({ plane(-20) });
        check(weakOwner.expired(), "Replacing static geometry leaked the old mesh owner");
        check(!simulation.isSleeping(), "Changing static geometry did not wake the body");
        runFor(simulation, 8);
        check(btFabs(simulation.getBodyTransform(0).getOrigin().z() + 18) < 0.2,
            "Body did not fall onto the newly replaced collision geometry");

        auto invalid = plane();
        invalid.mShape.reset();
        checkInvalid([&] { simulation.replaceStaticColliders({ invalid }); }, "Accepted a null collision shape");
        simulation.applyImpulse(0, btVector3(0, 0, 15), simulation.getBodyTransform(0).getOrigin());
        runFor(simulation, 4);
        check(btFabs(simulation.getBodyTransform(0).getOrigin().z() + 18) < 0.2,
            "Rejected replacement destroyed the previous valid collision geometry");
    }

    void testHumanoidJoints()
    {
        // Eleven articulated segments: pelvis, torso, head, and paired upper /
        // lower arms and legs. All start from a standing humanoid pose.
        std::vector<Ragdoll::BodyDefinition> bodies;
        const btQuaternion vertical(btVector3(1, 0, 0), SIMD_HALF_PI);
        const btQuaternion horizontal(btVector3(0, 0, 1), SIMD_HALF_PI);
        auto add = [&](btVector3 center, btScalar radius, btScalar length, btScalar mass, btQuaternion rotation) {
            auto body = capsule(center, radius, length);
            body.mMass = mass;
            body.mTransform.setRotation(rotation);
            bodies.push_back(body);
        };
        add(btVector3(0, 0, 100), 8, 10, 12, vertical);
        add(btVector3(0, 0, 124), 10, 14, 25, vertical);
        add(btVector3(0, 0, 146), 6, 6, 5, vertical);
        add(btVector3(-25, 0, 125), 3, 20, 3, horizontal);
        add(btVector3(-45, 0, 125), 2.5, 18, 2, horizontal);
        add(btVector3(25, 0, 125), 3, 20, 3, horizontal);
        add(btVector3(45, 0, 125), 2.5, 18, 2, horizontal);
        add(btVector3(-8, 0, 76), 5, 28, 7, vertical);
        add(btVector3(-8, 0, 43), 4, 27, 4, vertical);
        add(btVector3(8, 0, 76), 5, 28, 7, vertical);
        add(btVector3(8, 0, 43), 4, 27, 4, vertical);
        std::vector<Ragdoll::JointDefinition> joints;
        auto join = [&](std::size_t parent, std::size_t child, const btVector3& anchor, btScalar angle) {
            Ragdoll::JointDefinition joint;
            joint.mParent = parent;
            joint.mChild = child;
            joint.mFrameWorld.setOrigin(anchor);
            joint.mAngularLower = btVector3(-angle, -angle, -angle * 0.5);
            joint.mAngularUpper = -joint.mAngularLower;
            joints.push_back(joint);
        };
        join(0, 1, btVector3(0, 0, 112), 0.35);
        join(1, 2, btVector3(0, 0, 138), 0.5);
        join(1, 3, btVector3(-13, 0, 131), 1.1);
        join(3, 4, btVector3(-35, 0, 125), 1.2);
        join(1, 5, btVector3(13, 0, 131), 1.1);
        join(5, 6, btVector3(35, 0, 125), 1.2);
        join(0, 7, btVector3(-8, 0, 91), 0.8);
        join(7, 8, btVector3(-8, 0, 61), 1.1);
        join(0, 9, btVector3(8, 0, 91), 0.8);
        join(9, 10, btVector3(8, 0, 61), 1.1);
        Ragdoll::Simulation simulation(bodies, joints, { plane() });
        simulation.applyImpulse(1, btVector3(0, 500, 0), bodies[1].mTransform.getOrigin());
        runFor(simulation, 12);
        check(simulation.getNumBodies() == 11, "Humanoid body count changed");
        check(simulation.getBodyTransform(1).getOrigin().z() < 45, "Humanoid did not physically topple onto the floor");
        for (std::size_t i = 0; i < bodies.size(); ++i)
        {
            const auto& transform = simulation.getBodyTransform(i);
            check(std::isfinite(transform.getOrigin().length2()), "Humanoid segment has a non-finite position");
            check(transform.getOrigin().z() > bodies[i].mRadius - 0.5, "Humanoid segment penetrated the floor");
        }
        for (const auto& joint : joints)
        {
            const btTransform parentFrame = bodies[joint.mParent].mTransform.inverse() * joint.mFrameWorld;
            const btTransform childFrame = bodies[joint.mChild].mTransform.inverse() * joint.mFrameWorld;
            const btTransform parent = simulation.getBodyTransform(joint.mParent) * parentFrame;
            const btTransform child = simulation.getBodyTransform(joint.mChild) * childFrame;
            check(parent.getOrigin().distance(child.getOrigin()) < 0.7, "A humanoid joint separated under gravity");
            // Bullet's Generic6Dof constraint represents these rotations using
            // XYZ Euler angles. Inspect the actual same decomposition here.
            const btMatrix3x3 relative = parent.getBasis().inverse() * child.getBasis();
            btScalar yaw, pitch, roll;
            relative.getEulerZYX(yaw, pitch, roll);
            check(std::isfinite(yaw) && std::isfinite(pitch) && std::isfinite(roll),
                "A humanoid joint rotation became non-finite");
        }
    }

    void testFrameRatePauseAndBadInput()
    {
        const auto body = capsule(btVector3(0, 0, 100));
        Ragdoll::Simulation sixty({ body }, {}, { plane() });
        Ragdoll::Simulation thirty({ body }, {}, { plane() });
        runFor(sixty, 0.5, 1.0 / 60);
        runFor(thirty, 0.5, 1.0 / 30);
        check(sixty.getBodyTransform(0).getOrigin().distance(thirty.getBodyTransform(0).getOrigin()) < 1e-8,
            "Fixed time step depends on rendering frame rate");
        const auto before = sixty.getBodyTransform(0).getOrigin();
        check(sixty.advance(0) && sixty.advance(-1), "Paused simulation rejected a nonpositive frame duration");
        check(sixty.getBodyTransform(0).getOrigin() == before, "Paused simulation moved a body");
        check(!sixty.advance(std::numeric_limits<btScalar>::quiet_NaN()), "Accepted a NaN frame duration");
        check(sixty.isValid(), "A rejected frame duration damaged valid physics state");
        check(sixty.advance(1e30), "A long frame caused unsafe unbounded catch-up");
        auto invalid = body;
        invalid.mRadius = -1;
        checkInvalid([&] { Ragdoll::Simulation bad({ invalid }, {}, {}); }, "Accepted a negative capsule radius");
        invalid = body;
        invalid.mTransform.getBasis().setValue(2, 0, 0, 0, 1, 0, 0, 0, 1);
        checkInvalid([&] { Ragdoll::Simulation bad({ invalid }, {}, {}); }, "Accepted scale in a rigid transform");
        invalid = body;
        invalid.mTransform.setOrigin(btVector3(0, std::numeric_limits<btScalar>::infinity(), 0));
        checkInvalid([&] { Ragdoll::Simulation bad({ invalid }, {}, {}); }, "Accepted infinite body coordinates");
        invalid.mTransform.setOrigin(btVector3(1e150, 0, 0));
        checkInvalid([&] { Ragdoll::Simulation bad({ invalid }, {}, {}); }, "Accepted overflowing finite coordinates");
        Ragdoll::Settings settings;
        settings.mFixedTimeStep = 1e-100;
        checkInvalid([&] { Ragdoll::Simulation bad({ body }, {}, {}, settings); },
            "Accepted a time step that would require unbounded substeps");
        Ragdoll::JointDefinition joint;
        joint.mChild = 5;
        checkInvalid([&] { Ragdoll::Simulation bad({ body }, { joint }, {}); }, "Accepted an out-of-range joint body");
        checkInvalid([&] { Ragdoll::Simulation bad({}, {}, {}); }, "Accepted an empty ragdoll");
        checkInvalid([&] {
            sixty.applyImpulse(0, btVector3(std::numeric_limits<btScalar>::infinity(), 0, 0), before);
        }, "Accepted a non-finite impulse");
    }

    void testConstrainedMotion()
    {
        // Gravity-free test isolates the actual joint limit solver from floor
        // contacts. A zero-angle joint must remain rigid under off-center impact.
        auto left = capsule(btVector3(-5, 0, 50), 2, 6);
        auto right = capsule(btVector3(5, 0, 50), 2, 6);
        Ragdoll::JointDefinition joint;
        joint.mParent = 0;
        joint.mChild = 1;
        joint.mFrameWorld.setOrigin(btVector3(0, 0, 50));
        joint.mAngularLower.setZero();
        joint.mAngularUpper.setZero();
        Ragdoll::Settings settings;
        settings.mGravity.setZero();
        Ragdoll::Simulation simulation({ left, right }, { joint }, {}, settings);
        simulation.applyImpulse(1, btVector3(0, 40, 0), btVector3(5, 0, 53));
        runFor(simulation, 5);
        const btTransform first = simulation.getBodyTransform(0);
        const btTransform second = simulation.getBodyTransform(1);
        check(btFabs(first.getOrigin().distance(second.getOrigin()) - 10) < 0.1,
            "Locked joint translation stretched under an impact");
        const btQuaternion relative = first.getRotation().inverse() * second.getRotation();
        check(relative.getAngleShortestPath() < 0.01, "Locked angular joint allowed independent rotation");
    }
}

int main()
{
    try
    {
        testPlaneAndSleep();
        testStairs();
        testMeshLifetimeAndSnapshotReplacement();
        testHumanoidJoints();
        testFrameRatePauseAndBadInput();
        testConstrainedMotion();
        std::cout << "6 ragdoll physics suites passed (actual double precision Bullet solver)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Ragdoll physics test failed: " << error.what() << '\n';
        return 1;
    }
}
