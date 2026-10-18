#include "simulation.hpp"

#include <btBulletDynamicsCommon.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace Ragdoll
{
    namespace
    {
        constexpr short sBodyGroup = 1;
        constexpr short sStaticGroup = 2;

        bool finite(btScalar value)
        {
            return std::isfinite(value);
        }

        bool finite(const btVector3& value)
        {
            return finite(value.x()) && finite(value.y()) && finite(value.z());
        }

        bool finite(const btTransform& value)
        {
            return finite(value.getOrigin()) && finite(value.getBasis().getRow(0))
                && finite(value.getBasis().getRow(1)) && finite(value.getBasis().getRow(2));
        }

        bool rigid(const btTransform& value)
        {
            // Keep AABB and inertia arithmetic away from floating-point
            // overflow even when a malformed model contains finite extremes.
            if (!finite(value) || value.getOrigin().length2() > 1e18)
                return false;
            const btMatrix3x3& basis = value.getBasis();
            constexpr btScalar tolerance = 0.001;
            return btFabs(basis.getRow(0).length2() - 1) < tolerance
                && btFabs(basis.getRow(1).length2() - 1) < tolerance
                && btFabs(basis.getRow(2).length2() - 1) < tolerance
                && btFabs(basis.getRow(0).dot(basis.getRow(1))) < tolerance
                && btFabs(basis.getRow(0).dot(basis.getRow(2))) < tolerance
                && btFabs(basis.getRow(1).dot(basis.getRow(2))) < tolerance
                && btFabs(basis.determinant() - 1) < tolerance;
        }

        void validateSettings(const Settings& settings)
        {
            if (!finite(settings.mGravity) || settings.mGravity.length2() > 1e12
                || !finite(settings.mFixedTimeStep) || settings.mFixedTimeStep < 1.0 / 1000.0
                || settings.mFixedTimeStep > 1.0 / 30.0 || !finite(settings.mMaxFrameTime)
                || settings.mMaxFrameTime < settings.mFixedTimeStep || settings.mMaxFrameTime > 1
                || !finite(settings.mLinearDamping) || settings.mLinearDamping < 0 || settings.mLinearDamping > 1
                || !finite(settings.mAngularDamping) || settings.mAngularDamping < 0 || settings.mAngularDamping > 1
                || !finite(settings.mMaxLinearSpeed) || settings.mMaxLinearSpeed <= 0
                || settings.mMaxLinearSpeed > 1e6 || !finite(settings.mMaxAngularSpeed)
                || settings.mMaxAngularSpeed <= 0 || settings.mMaxAngularSpeed > 1000)
                throw std::invalid_argument("Invalid ragdoll simulation settings");
        }

        void limitVelocity(btRigidBody& body, const Settings& settings)
        {
            const btVector3 linear = body.getLinearVelocity();
            if (linear.length2() > settings.mMaxLinearSpeed * settings.mMaxLinearSpeed)
                body.setLinearVelocity(linear.normalized() * settings.mMaxLinearSpeed);
            const btVector3 angular = body.getAngularVelocity();
            if (angular.length2() > settings.mMaxAngularSpeed * settings.mMaxAngularSpeed)
                body.setAngularVelocity(angular.normalized() * settings.mMaxAngularSpeed);
        }

        // Squared distance between capsule centre segments, including zero-length
        // capsules. This is used only to defer collisions for overlapping meshes
        // in the captured animation pose; contact generation stays with Bullet.
        btScalar segmentDistance2(const btVector3& p, const btVector3& endP,
            const btVector3& q, const btVector3& endQ)
        {
            const btVector3 d = endP - p;
            const btVector3 e = endQ - q;
            const btVector3 r = p - q;
            const btScalar a = d.dot(d);
            const btScalar b = d.dot(e);
            const btScalar c = e.dot(e);
            const btScalar f = e.dot(r);
            btScalar s = 0;
            btScalar t = 0;
            if (a < SIMD_EPSILON && c < SIMD_EPSILON)
                return r.length2();
            if (a < SIMD_EPSILON)
                t = std::clamp(f / c, btScalar(0), btScalar(1));
            else
            {
                const btScalar g = d.dot(r);
                if (c < SIMD_EPSILON)
                    s = std::clamp(-g / a, btScalar(0), btScalar(1));
                else
                {
                    const btScalar denominator = a * c - b * b;
                    if (denominator > SIMD_EPSILON)
                        s = std::clamp((b * f - g * c) / denominator, btScalar(0), btScalar(1));
                    t = (b * s + f) / c;
                    if (t < 0)
                    {
                        t = 0;
                        s = std::clamp(-g / a, btScalar(0), btScalar(1));
                    }
                    else if (t > 1)
                    {
                        t = 1;
                        s = std::clamp((b - g) / a, btScalar(0), btScalar(1));
                    }
                }
            }
            return (r + s * d - t * e).length2();
        }
    }

    struct Simulation::Impl
    {
        struct World final : btDiscreteDynamicsWorld
        {
            Impl& mOwner;

            World(Impl& owner, btDispatcher* dispatcher, btBroadphaseInterface* broadphase,
                btConstraintSolver* solver, btCollisionConfiguration* configuration)
                : btDiscreteDynamicsWorld(dispatcher, broadphase, solver, configuration)
                , mOwner(owner)
            {
            }

            void integrateTransforms(btScalar dt) override
            {
                // Contact constraints may add angular velocity after the normal
                // pre-step limit check. Enforce anatomical stop velocities at the
                // actual integration boundary too, without snapping body poses.
                mOwner.anticipateHingeStops(dt, 1);
                btDiscreteDynamicsWorld::integrateTransforms(dt);
            }
        };

        Settings mSettings;
        btScalar mAccumulator = 0;
        bool mValid = true;
        std::unique_ptr<btDefaultCollisionConfiguration> mConfiguration;
        std::unique_ptr<btCollisionDispatcher> mDispatcher;
        std::unique_ptr<btDbvtBroadphase> mBroadphase;
        std::unique_ptr<btSequentialImpulseConstraintSolver> mSolver;
        std::unique_ptr<World> mWorld;
        std::vector<std::unique_ptr<btCollisionShape>> mShapes;
        std::vector<std::unique_ptr<btRigidBody>> mBodies;
        std::vector<std::unique_ptr<btTypedConstraint>> mConstraints;
        std::unique_ptr<btPoint2PointConstraint> mPin;
        std::vector<std::pair<std::size_t, std::size_t>> mInitiallyOverlapping;
        btScalar mTotalMass = 0;
        std::vector<StaticCollider> mStaticShapes;
        std::vector<std::unique_ptr<btCollisionObject>> mStaticObjects;
        std::vector<btTransform> mLastFinitePose;

        explicit Impl(const Settings& settings)
            : mSettings(settings)
        {
            mConfiguration = std::make_unique<btDefaultCollisionConfiguration>();
            mDispatcher = std::make_unique<btCollisionDispatcher>(mConfiguration.get());
            mBroadphase = std::make_unique<btDbvtBroadphase>();
            mSolver = std::make_unique<btSequentialImpulseConstraintSolver>();
            mWorld = std::make_unique<World>(
                *this, mDispatcher.get(), mBroadphase.get(), mSolver.get(), mConfiguration.get());
            mWorld->setGravity(settings.mGravity);
            mWorld->getSolverInfo().m_numIterations = 60;
            mWorld->getSolverInfo().m_splitImpulse = true;
        }

        ~Impl()
        {
            // Bullet's world stores non-owning pointers. Remove every object
            // before destroying its shape, constraint, or retained mesh data.
            if (mPin)
                mWorld->removeConstraint(mPin.get());
            for (const auto& constraint : mConstraints)
                mWorld->removeConstraint(constraint.get());
            for (const auto& body : mBodies)
                mWorld->removeRigidBody(body.get());
            for (const auto& object : mStaticObjects)
                mWorld->removeCollisionObject(object.get());
            mWorld.reset();
            mStaticObjects.clear();
            mStaticShapes.clear();
            mConstraints.clear();
            mPin.reset();
            mBodies.clear();
            mShapes.clear();
        }

        btScalar capsuleSeparation(std::size_t first, std::size_t second) const
        {
            const auto& a = static_cast<const btCapsuleShape&>(*mShapes[first]);
            const auto& b = static_cast<const btCapsuleShape&>(*mShapes[second]);
            const auto& ta = mBodies[first]->getWorldTransform();
            const auto& tb = mBodies[second]->getWorldTransform();
            return btSqrt(segmentDistance2(ta * btVector3(0, -a.getHalfHeight(), 0),
                              ta * btVector3(0, a.getHalfHeight(), 0), tb * btVector3(0, -b.getHalfHeight(), 0),
                              tb * btVector3(0, b.getHalfHeight(), 0)))
                - a.getRadius() - b.getRadius();
        }

        void restoreSeparatedCollisions()
        {
            for (auto it = mInitiallyOverlapping.begin(); it != mInitiallyOverlapping.end();)
            {
                if (capsuleSeparation(it->first, it->second) < 0.1)
                {
                    ++it;
                    continue;
                }
                mBodies[it->first]->setIgnoreCollisionCheck(mBodies[it->second].get(), false);
                mBodies[it->second]->setIgnoreCollisionCheck(mBodies[it->first].get(), false);
                it = mInitiallyOverlapping.erase(it);
            }
        }

        void anticipateHingeStops(btScalar dt, btScalar horizon = 2)
        {
            for (const auto& constraint : mConstraints)
            {
                if (constraint->getConstraintType() != HINGE_CONSTRAINT_TYPE)
                    continue;
                auto& hinge = static_cast<btHingeConstraint&>(*constraint);
                btRigidBody& parent = hinge.getRigidBodyA();
                btRigidBody& child = hinge.getRigidBodyB();
                const btVector3 axis = (parent.getWorldTransform() * hinge.getAFrame()).getBasis().getColumn(2);
                const btScalar angle = hinge.getHingeAngle();
                const btScalar speed = axis.dot(child.getAngularVelocity() - parent.getAngularVelocity());
                // Bullet's hinge stop activates at the current angle. With a
                // strong impact that would permit one substep of backwards
                // flexion. Apply its unilateral angular impulse speculatively
                // when this substep would cross the anatomical limit.
                const btScalar safeSpeed = std::clamp(speed,
                    (hinge.getLowerLimit() - angle) / (horizon * dt), (hinge.getUpperLimit() - angle) / (horizon * dt));
                const btScalar inverseInertia = axis.dot(
                    (parent.getInvInertiaTensorWorld() + child.getInvInertiaTensorWorld()) * axis);
                if (inverseInertia <= SIMD_EPSILON || btFabs(safeSpeed - speed) < SIMD_EPSILON)
                    continue;
                const btVector3 impulse = axis * ((safeSpeed - speed) / inverseInertia);
                parent.applyTorqueImpulse(-impulse);
                child.applyTorqueImpulse(impulse);
            }
        }

        void freeze()
        {
            for (std::size_t i = 0; i < mBodies.size(); ++i)
            {
                btRigidBody& body = *mBodies[i];
                body.setWorldTransform(mLastFinitePose[i]);
                body.setInterpolationWorldTransform(mLastFinitePose[i]);
                body.setLinearVelocity(btVector3(0, 0, 0));
                body.setAngularVelocity(btVector3(0, 0, 0));
                body.clearForces();
                body.forceActivationState(ISLAND_SLEEPING);
            }
            mValid = false;
        }
    };

    Simulation::Simulation(const std::vector<BodyDefinition>& bodies, const std::vector<JointDefinition>& joints,
        std::vector<StaticCollider> colliders, const Settings& settings)
    {
        validateSettings(settings);
        if (bodies.empty() || bodies.size() > 64 || joints.size() > 128)
            throw std::invalid_argument("Invalid ragdoll body or joint count");
        // Validate everything before Bullet sees input from an unusual skeleton.
        for (const BodyDefinition& body : bodies)
        {
            if (!rigid(body.mTransform) || !finite(body.mRadius) || body.mRadius < 0.001 || body.mRadius > 10000
                || !finite(body.mLength) || body.mLength < 0 || body.mLength > 10000 || !finite(body.mMass)
                || body.mMass < 0.001 || body.mMass > 100000)
                throw std::invalid_argument("Invalid ragdoll body definition");
        }
        for (const JointDefinition& joint : joints)
        {
            if (joint.mParent >= bodies.size() || joint.mChild >= bodies.size() || joint.mParent == joint.mChild
                || !rigid(joint.mFrameWorld) || !finite(joint.mAngularLower) || !finite(joint.mAngularUpper)
                || (joint.mUseLocalFrames && (!rigid(joint.mFrameInParent) || !rigid(joint.mFrameInChild))))
                throw std::invalid_argument("Invalid ragdoll joint definition");
            for (int axis = 0; axis < 3; ++axis)
            {
                if (joint.mAngularLower[axis] > joint.mAngularUpper[axis]
                    || joint.mAngularLower[axis] < -SIMD_PI || joint.mAngularUpper[axis] > SIMD_PI)
                    throw std::invalid_argument("Invalid ragdoll joint limits");
            }
        }

        mImpl = std::make_unique<Impl>(settings);
        mImpl->mShapes.reserve(bodies.size());
        mImpl->mBodies.reserve(bodies.size());
        mImpl->mLastFinitePose.reserve(bodies.size());
        for (const BodyDefinition& definition : bodies)
        {
            auto shape = std::make_unique<btCapsuleShape>(definition.mRadius, definition.mLength);
            btVector3 inertia;
            shape->calculateLocalInertia(definition.mMass, inertia);
            btRigidBody::btRigidBodyConstructionInfo info(definition.mMass, nullptr, shape.get(), inertia);
            info.m_startWorldTransform = definition.mTransform;
            info.m_linearDamping = settings.mLinearDamping;
            info.m_angularDamping = settings.mAngularDamping;
            info.m_friction = 0.85;
            info.m_rollingFriction = 0.05;
            info.m_spinningFriction = 0.1;
            info.m_restitution = 0;
            auto body = std::make_unique<btRigidBody>(info);
            body->setSleepingThresholds(1, 0.15);
            body->setCcdMotionThreshold(definition.mRadius * 0.5);
            body->setCcdSweptSphereRadius(definition.mRadius * 0.8);
            mImpl->mWorld->addRigidBody(body.get(), sBodyGroup, sStaticGroup | sBodyGroup);
            mImpl->mShapes.push_back(std::move(shape));
            mImpl->mBodies.push_back(std::move(body));
            mImpl->mLastFinitePose.push_back(definition.mTransform);
            mImpl->mTotalMass += definition.mMass;
        }
        mImpl->mConstraints.reserve(joints.size());
        for (const JointDefinition& joint : joints)
        {
            btRigidBody& parent = *mImpl->mBodies[joint.mParent];
            btRigidBody& child = *mImpl->mBodies[joint.mChild];
            const btTransform localParent = joint.mUseLocalFrames ? joint.mFrameInParent
                : parent.getWorldTransform().inverse() * joint.mFrameWorld;
            const btTransform localChild = joint.mUseLocalFrames ? joint.mFrameInChild
                : child.getWorldTransform().inverse() * joint.mFrameWorld;
            std::unique_ptr<btTypedConstraint> constraint;
            if (joint.mType == JointDefinition::Type::Hinge)
            {
                auto hinge = std::make_unique<btHingeConstraint>(parent, child, localParent, localChild, true);
                hinge->setLimit(joint.mAngularLower.z(), joint.mAngularUpper.z(), 0.95, 0.3, 1.0);
                constraint = std::move(hinge);
            }
            else
            {
                auto ball = std::make_unique<btGeneric6DofConstraint>(parent, child, localParent, localChild, true);
                ball->setLinearLowerLimit(btVector3(0, 0, 0));
                ball->setLinearUpperLimit(btVector3(0, 0, 0));
                ball->setAngularLowerLimit(joint.mAngularLower);
                ball->setAngularUpperLimit(joint.mAngularUpper);
                constraint = std::move(ball);
            }
            constraint->setOverrideNumSolverIterations(80);
            mImpl->mWorld->addConstraint(constraint.get(), true);
            mImpl->mConstraints.push_back(std::move(constraint));
        }
        for (std::size_t i = 0; i < bodies.size(); ++i)
        {
            for (std::size_t j = i + 1; j < bodies.size(); ++j)
            {
                // Linked capsules naturally overlap at their common joint and
                // Bullet already excludes that pair through addConstraint(true).
                if (std::any_of(joints.begin(), joints.end(), [=](const JointDefinition& joint) {
                        return (joint.mParent == i && joint.mChild == j)
                            || (joint.mParent == j && joint.mChild == i);
                    }))
                    continue;
                if (mImpl->capsuleSeparation(i, j) >= -0.1)
                    continue;
                // A weapon animation can start with forearms inside the torso.
                // Defer only those initial contacts until separation, rather than
                // firing a violent depenetration impulse or disabling all limbs.
                mImpl->mBodies[i]->setIgnoreCollisionCheck(mImpl->mBodies[j].get(), true);
                mImpl->mBodies[j]->setIgnoreCollisionCheck(mImpl->mBodies[i].get(), true);
                mImpl->mInitiallyOverlapping.emplace_back(i, j);
            }
        }
        replaceStaticColliders(std::move(colliders));
    }

    Simulation::~Simulation() = default;

    bool Simulation::advance(btScalar dt)
    {
        if (!mImpl->mValid || !finite(dt))
            return false;
        if (dt <= 0)
            return true;
        mImpl->mAccumulator += std::min(dt, mImpl->mSettings.mMaxFrameTime);
        const btScalar step = mImpl->mSettings.mFixedTimeStep;
        while (mImpl->mAccumulator + step * 1e-9 >= step)
        {
            mImpl->restoreSeparatedCollisions();
            mImpl->anticipateHingeStops(step);
            mImpl->mWorld->stepSimulation(step, 0, step);
            mImpl->mAccumulator = std::max(btScalar(0), mImpl->mAccumulator - step);
            for (const auto& body : mImpl->mBodies)
            {
                if (!finite(body->getWorldTransform()) || !finite(body->getLinearVelocity())
                    || !finite(body->getAngularVelocity()))
                {
                    mImpl->freeze();
                    return false;
                }
                limitVelocity(*body, mImpl->mSettings);
            }
            for (std::size_t i = 0; i < mImpl->mBodies.size(); ++i)
                mImpl->mLastFinitePose[i] = mImpl->mBodies[i]->getWorldTransform();
        }
        return true;
    }

    void Simulation::replaceStaticColliders(std::vector<StaticCollider> colliders)
    {
        std::vector<std::unique_ptr<btCollisionObject>> objects;
        objects.reserve(colliders.size());
        for (const StaticCollider& collider : colliders)
        {
            if (!collider.mShape || !rigid(collider.mTransform) || !finite(collider.mFriction)
                || collider.mFriction < 0 || collider.mFriction > 10)
                throw std::invalid_argument("Invalid ragdoll static collider");
            auto object = std::make_unique<btCollisionObject>();
            object->setCollisionShape(collider.mShape.get());
            object->setWorldTransform(collider.mTransform);
            object->setCollisionFlags(btCollisionObject::CF_STATIC_OBJECT);
            object->setFriction(collider.mFriction);
            object->setRestitution(0);
            objects.push_back(std::move(object));
        }
        for (const auto& object : mImpl->mStaticObjects)
            mImpl->mWorld->removeCollisionObject(object.get());
        // Destroy the old objects before their retained shapes/mesh data.
        mImpl->mStaticObjects.clear();
        mImpl->mStaticShapes = std::move(colliders);
        mImpl->mStaticObjects = std::move(objects);
        for (const auto& object : mImpl->mStaticObjects)
            mImpl->mWorld->addCollisionObject(object.get(), sStaticGroup, sBodyGroup);
        // A floor can disappear or a door can change its collision snapshot.
        for (const auto& body : mImpl->mBodies)
            body->activate(true);
    }

    void Simulation::applyImpulse(std::size_t body, const btVector3& impulse, const btVector3& worldPoint)
    {
        if (!finite(impulse) || !finite(worldPoint))
            throw std::invalid_argument("Invalid ragdoll impulse");
        btRigidBody& target = *mImpl->mBodies.at(body);
        if (!mImpl->mValid)
            return;
        target.activate(true);
        target.applyImpulse(impulse, worldPoint - target.getCenterOfMassPosition());
        if (!finite(target.getLinearVelocity()) || !finite(target.getAngularVelocity()))
            mImpl->freeze();
        else
            limitVelocity(target, mImpl->mSettings);
    }

    void Simulation::applyWholeBodyImpulse(const btVector3& impulse)
    {
        if (!finite(impulse))
            throw std::invalid_argument("Invalid ragdoll whole-body impulse");
        if (!mImpl->mValid)
            return;
        for (const auto& body : mImpl->mBodies)
        {
            body->activate(true);
            body->applyCentralImpulse(impulse * (body->getMass() / mImpl->mTotalMass));
            if (!finite(body->getLinearVelocity()))
            {
                mImpl->freeze();
                return;
            }
            limitVelocity(*body, mImpl->mSettings);
        }
    }

    void Simulation::applyJointTorqueImpulse(std::size_t joint, const btVector3& impulse)
    {
        if (!finite(impulse))
            throw std::invalid_argument("Invalid ragdoll joint torque");
        btTypedConstraint& constraint = *mImpl->mConstraints.at(joint);
        if (!mImpl->mValid)
            return;
        for (const auto& [body, sign] : { std::pair{ &constraint.getRigidBodyA(), btScalar(-1) },
                 std::pair{ &constraint.getRigidBodyB(), btScalar(1) } })
        {
            body->activate(true);
            body->applyTorqueImpulse(impulse * sign);
            if (!finite(body->getAngularVelocity()))
            {
                mImpl->freeze();
                return;
            }
            limitVelocity(*body, mImpl->mSettings);
        }
    }

    bool Simulation::raycastStatic(const btVector3& from, const btVector3& to,
        btVector3& point, btVector3& normal) const
    {
        if (!finite(from) || !finite(to))
            throw std::invalid_argument("Invalid ragdoll ray");
        if (!mImpl->mValid || from.distance2(to) < SIMD_EPSILON)
            return false;
        btCollisionWorld::ClosestRayResultCallback callback(from, to);
        callback.m_collisionFilterGroup = sBodyGroup;
        callback.m_collisionFilterMask = sStaticGroup;
        mImpl->mWorld->rayTest(from, to, callback);
        if (!callback.hasHit())
            return false;
        point = callback.m_hitPointWorld;
        normal = callback.m_hitNormalWorld;
        return finite(point) && finite(normal);
    }

    bool Simulation::pinBody(std::size_t body, const btVector3& worldPoint)
    {
        if (!finite(worldPoint))
            throw std::invalid_argument("Invalid ragdoll pin point");
        btRigidBody& target = *mImpl->mBodies.at(body);
        if (!mImpl->mValid || mImpl->mPin)
            return false;
        const btVector3 local = target.getWorldTransform().inverse() * worldPoint;
        // A pin is a short bolt through a body, never an invisible tether to a
        // distant wall. Caller must wait for an actual collision approach.
        const auto& shape = static_cast<const btCapsuleShape&>(*mImpl->mShapes[body]);
        if (local.length() > shape.getHalfHeight() + shape.getRadius() * 2)
            return false;
        auto pin = std::make_unique<btPoint2PointConstraint>(target, local);
        pin->setPivotB(worldPoint);
        pin->m_setting.m_tau = 0.3;
        pin->setOverrideNumSolverIterations(100);
        mImpl->mWorld->addConstraint(pin.get(), true);
        mImpl->mPin = std::move(pin);
        for (const auto& segment : mImpl->mBodies)
            segment->activate(true);
        return true;
    }

    bool Simulation::isPinned() const
    {
        return mImpl->mPin != nullptr;
    }

    std::size_t Simulation::getNumBodies() const
    {
        return mImpl->mBodies.size();
    }

    const btTransform& Simulation::getBodyTransform(std::size_t body) const
    {
        return mImpl->mBodies.at(body)->getWorldTransform();
    }

    const btVector3& Simulation::getBodyLinearVelocity(std::size_t body) const
    {
        return mImpl->mBodies.at(body)->getLinearVelocity();
    }

    const btVector3& Simulation::getBodyAngularVelocity(std::size_t body) const
    {
        return mImpl->mBodies.at(body)->getAngularVelocity();
    }

    btScalar Simulation::getBodyMass(std::size_t body) const
    {
        return mImpl->mBodies.at(body)->getMass();
    }

    btScalar Simulation::getTotalMass() const
    {
        return mImpl->mTotalMass;
    }

    btScalar Simulation::getJointAngle(std::size_t joint) const
    {
        btTypedConstraint& constraint = *mImpl->mConstraints.at(joint);
        return constraint.getConstraintType() == HINGE_CONSTRAINT_TYPE
            ? static_cast<btHingeConstraint&>(constraint).getHingeAngle() : 0;
    }

    bool Simulation::isSleeping() const
    {
        return std::all_of(mImpl->mBodies.begin(), mImpl->mBodies.end(), [](const auto& body) {
            return !body->isActive();
        });
    }

    bool Simulation::isValid() const
    {
        return mImpl->mValid;
    }
}
