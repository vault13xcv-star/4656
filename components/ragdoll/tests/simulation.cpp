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

        // A launch into a nearby wall exercises the whole articulated body,
        // contact impulses and fixed stepping at each supported rendering rate.
        Ragdoll::StaticCollider wall;
        wall.mShape = std::make_shared<btBoxShape>(btVector3(3, 500, 500));
        wall.mTransform.setOrigin(btVector3(120, 0, 250));
        for (const btScalar frame : { btScalar(1.0 / 30), btScalar(1.0 / 60), btScalar(1.0 / 120) })
        {
            Ragdoll::Simulation launched(bodies, joints, { plane(), wall });
            launched.applyWholeBodyImpulse(btVector3(400, 30, 160) * launched.getTotalMass());
            runFor(launched, 10, frame);
            for (std::size_t i = 0; i < bodies.size(); ++i)
            {
                const auto& origin = launched.getBodyTransform(i).getOrigin();
                check(origin.z() > bodies[i].mRadius - 0.5 && origin.z() < 200,
                    "An impact launched a segment through the floor or left it airborne");
                check(origin.x() < 120, "A high-speed ragdoll tunneled through a wall");
            }
            for (const auto& joint : joints)
            {
                const btTransform localA = bodies[joint.mParent].mTransform.inverse() * joint.mFrameWorld;
                const btTransform localB = bodies[joint.mChild].mTransform.inverse() * joint.mFrameWorld;
                const btVector3 a = (launched.getBodyTransform(joint.mParent) * localA).getOrigin();
                const btVector3 b = (launched.getBodyTransform(joint.mChild) * localB).getOrigin();
                check(a.distance(b) < 0.8, "A wall impact pulled apart an articulated humanoid");
            }
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

    void testMassWeightedImpactAndConservation()
    {
        auto first = capsule(btVector3(-20, 0, 30));
        auto second = capsule(btVector3(20, 0, 30));
        first.mMass = 5;
        second.mMass = 15;
        Ragdoll::Settings settings;
        settings.mGravity.setZero();
        settings.mLinearDamping = 0;
        Ragdoll::Simulation light({ first, second }, {}, {}, settings);
        const btVector3 impulse(200, 40, 0);
        light.applyWholeBodyImpulse(impulse);
        check(btFabs(light.getTotalMass() - 20) < 1e-8, "Total body mass is incorrect");
        check(light.getBodyLinearVelocity(0).distance(impulse / 20) < 1e-8
                && light.getBodyLinearVelocity(1).distance(impulse / 20) < 1e-8,
            "Impact stretched lighter limbs instead of distributing the body's momentum");
        first.mMass *= 2;
        second.mMass *= 2;
        Ragdoll::Simulation heavy({ first, second }, {}, {}, settings);
        heavy.applyWholeBodyImpulse(impulse);
        runFor(light, 0.5);
        runFor(heavy, 0.5);
        const btScalar lightTravel = light.getBodyTransform(0).getOrigin().x() + 20;
        const btScalar heavyTravel = heavy.getBodyTransform(0).getOrigin().x() + 20;
        check(btFabs(lightTravel - heavyTravel * 2) < 1e-6, "Double mass did not halve impact velocity");

        Ragdoll::JointDefinition joint;
        joint.mChild = 1;
        joint.mFrameWorld.setOrigin(btVector3(0, 0, 30));
        Ragdoll::Simulation convulsing({ first, second }, { joint }, {}, settings);
        convulsing.applyJointTorqueImpulse(0, btVector3(50, -30, 20));
        check(convulsing.getBodyLinearVelocity(0).length2() == 0
                && convulsing.getBodyLinearVelocity(1).length2() == 0,
            "Joint convulsion injected a linear launch");
        runFor(convulsing, 0.5);
        const btVector3 momentum = convulsing.getBodyLinearVelocity(0) * first.mMass
            + convulsing.getBodyLinearVelocity(1) * second.mMass;
        check(momentum.length() < 1e-7, "Joint convulsion changed total linear momentum");
    }

    void testAnatomicalHingeLimits()
    {
        // The local frames represent a limb which was already flexed 0.7 rad
        // before death. Its limits are anatomical, not +/- the captured angle.
        // Different world orientations also catch post-solver angular impulses
        // that accidentally invalidate the already solved point-anchor velocity.
        for (const btScalar orientation : { btScalar(0), btScalar(0.1), btScalar(0.3), btScalar(0.7),
                 btScalar(1.1), btScalar(1.5), btScalar(2.2), btScalar(2.8) })
        for (const btScalar frame : { btScalar(1.0 / 30), btScalar(1.0 / 60), btScalar(1.0 / 120) })
        {
            auto proximal = capsule(btVector3(0, 10, 60), 3, 14);
            auto distal = capsule(btVector3(0, -10, 60), 3, 14);
            proximal.mMass = 5;
            distal.mMass = 3;
            btTransform anchor = btTransform::getIdentity();
            anchor.setOrigin(btVector3(0, 0, 60));
            const btQuaternion flexion(btVector3(0, 0, 1), 0.7);
            distal.mTransform.setOrigin(anchor.getOrigin() + quatRotate(flexion, btVector3(0, -10, 0)));
            distal.mTransform.setRotation(flexion);
            btTransform distalFrame = anchor;
            distalFrame.setRotation(flexion);
            const btQuaternion worldRotation(btVector3(1, 2, 3).normalized(), orientation);
            const btTransform world(worldRotation, btVector3(0, 0, 0));
            proximal.mTransform = world * proximal.mTransform;
            distal.mTransform = world * distal.mTransform;
            anchor = world * anchor;
            distalFrame = world * distalFrame;
            Ragdoll::JointDefinition knee;
            knee.mChild = 1;
            knee.mType = Ragdoll::JointDefinition::Type::Hinge;
            knee.mUseLocalFrames = true;
            knee.mFrameInParent = proximal.mTransform.inverse() * anchor;
            knee.mFrameInChild = distal.mTransform.inverse() * distalFrame;
            knee.mAngularLower = btVector3(0, 0, 0);
            knee.mAngularUpper = btVector3(0, 0, 2.4);
            Ragdoll::Settings settings;
            settings.mGravity.setZero();
            Ragdoll::Simulation simulation({ proximal, distal }, { knee }, {}, settings);
            check(btFabs(simulation.getJointAngle(0) - 0.7) < 1e-6,
                "Anatomical hinge lost the captured limb flexion");
            check(simulation.getBodyTransform(1).getOrigin() == distal.mTransform.getOrigin(),
                "Creating a hinge snapped the body into a different pose");
            for (const btScalar torque : { btScalar(-5000), btScalar(5000), btScalar(-5000) })
            {
                simulation.applyJointTorqueImpulse(0, quatRotate(worldRotation, btVector3(0, 0, torque)));
                for (int i = 0; i < static_cast<int>(2 / frame); ++i)
                {
                    check(simulation.advance(frame), "Hinge impact became invalid");
                    const btScalar angle = simulation.getJointAngle(0);
                    check(angle > -0.08 && angle < 2.48,
                        "Knee/elbow hinge bent backwards or folded past its anatomical stop: " + std::to_string(angle));
                    const btTransform a = simulation.getBodyTransform(0) * knee.mFrameInParent;
                    const btTransform b = simulation.getBodyTransform(1) * knee.mFrameInChild;
                    check(a.getOrigin().distance(b.getOrigin()) < 0.75, "A hinge separated under a strong impact: " + std::to_string(a.getOrigin().distance(b.getOrigin())));
                    check(a.getBasis().getColumn(2).dot(b.getBasis().getColumn(2)) > 0.999,
                        "A knee/elbow hinge developed sideways twist");
                }
            }
        }
    }

    void testSelfCollisionAndOverlappingAnimation()
    {
        Ragdoll::Settings settings;
        settings.mGravity.setZero();
        settings.mLinearDamping = 0;
        Ragdoll::Simulation collision({ capsule(btVector3(-10, 0, 0)), capsule(btVector3(10, 0, 0)) }, {}, {}, settings);
        collision.applyImpulse(0, btVector3(25, 0, 0), btVector3(-10, 0, 0));
        collision.applyImpulse(1, btVector3(-25, 0, 0), btVector3(10, 0, 0));
        runFor(collision, 1);
        check(collision.getBodyTransform(1).getOrigin().x() - collision.getBodyTransform(0).getOrigin().x() > 3.9,
            "Unlinked body segments passed through each other");

        Ragdoll::Simulation overlap({ capsule(btVector3(-0.5, 0, 0)), capsule(btVector3(0.5, 0, 0)) }, {}, {}, settings);
        runFor(overlap, 0.2);
        check(overlap.getBodyLinearVelocity(0).length() < 0.01
                && overlap.getBodyLinearVelocity(1).length() < 0.01,
            "An initial overlapping animation pose caused a depenetration explosion");
        overlap.applyImpulse(0, btVector3(-10, 0, 0), btVector3(-0.5, 0, 0));
        overlap.applyImpulse(1, btVector3(10, 0, 0), btVector3(0.5, 0, 0));
        runFor(overlap, 0.5);
        overlap.applyImpulse(0, btVector3(20, 0, 0), overlap.getBodyTransform(0).getOrigin());
        overlap.applyImpulse(1, btVector3(-20, 0, 0), overlap.getBodyTransform(1).getOrigin());
        runFor(overlap, 1);
        check(overlap.getBodyTransform(1).getOrigin().x() - overlap.getBodyTransform(0).getOrigin().x() > 3.9,
            "Initially overlapping limbs did not recover self-collision after separation");
    }

    void testStaticRayAndWallPin()
    {
        Ragdoll::StaticCollider wall;
        wall.mShape = std::make_shared<btBoxShape>(btVector3(1, 100, 100));
        wall.mTransform.setOrigin(btVector3(20, 0, 50));
        auto torso = capsule(btVector3(15, 0, 60), 4, 10);
        torso.mMass = 25;
        Ragdoll::Simulation simulation({ torso }, {}, { plane(), wall });
        btVector3 point, normal;
        check(simulation.raycastStatic(btVector3(0, 0, 60), btVector3(30, 0, 60), point, normal),
            "Static wall ray missed the wall");
        check(btFabs(point.x() - 19) < 0.01 && normal.x() < -0.99,
            "Wall ray hit the corpse itself or returned the wrong impact normal");
        check(!simulation.pinBody(0, btVector3(1000, 0, 60)), "Pinned a corpse before it reached a distant wall");
        check(simulation.pinBody(0, point) && simulation.isPinned(), "Wall pin could not be created");
        check(!simulation.pinBody(0, point), "Created a duplicate pin constraint");
        const btVector3 localPin = torso.mTransform.inverse() * point;
        simulation.applyWholeBodyImpulse(btVector3(2000, 1000, 1000));
        runFor(simulation, 10);
        check((simulation.getBodyTransform(0) * localPin).distance(point) < 0.3,
            "Wall pin detached under gravity and an impact");
        check(simulation.getBodyTransform(0).getOrigin().z() > 50, "Pinned corpse fell to the floor");
        simulation.replaceStaticColliders({ plane(), wall });
        runFor(simulation, 0.5);
        check((simulation.getBodyTransform(0) * localPin).distance(point) < 0.3,
            "Replacing collision geometry invalidated the pin's world anchor");
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
        testMassWeightedImpactAndConservation();
        testAnatomicalHingeLimits();
        testSelfCollisionAndOverlappingAnimation();
        testStaticRayAndWallPin();
        std::cout << "10 ragdoll physics suites passed (actual double precision Bullet solver)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Ragdoll physics test failed: " << error.what() << '\n';
        return 1;
    }
}
