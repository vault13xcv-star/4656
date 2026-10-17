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
    }

    struct Simulation::Impl
    {
        Settings mSettings;
        btScalar mAccumulator = 0;
        bool mValid = true;
        std::unique_ptr<btDefaultCollisionConfiguration> mConfiguration;
        std::unique_ptr<btCollisionDispatcher> mDispatcher;
        std::unique_ptr<btDbvtBroadphase> mBroadphase;
        std::unique_ptr<btSequentialImpulseConstraintSolver> mSolver;
        std::unique_ptr<btDiscreteDynamicsWorld> mWorld;
        std::vector<std::unique_ptr<btCollisionShape>> mShapes;
        std::vector<std::unique_ptr<btRigidBody>> mBodies;
        std::vector<std::unique_ptr<btTypedConstraint>> mConstraints;
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
            mWorld = std::make_unique<btDiscreteDynamicsWorld>(
                mDispatcher.get(), mBroadphase.get(), mSolver.get(), mConfiguration.get());
            mWorld->setGravity(settings.mGravity);
            mWorld->getSolverInfo().m_numIterations = 30;
            mWorld->getSolverInfo().m_splitImpulse = true;
        }

        ~Impl()
        {
            // Bullet's world stores non-owning pointers. Remove every object
            // before destroying its shape, constraint, or retained mesh data.
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
            mBodies.clear();
            mShapes.clear();
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
                || !rigid(joint.mFrameWorld) || !finite(joint.mAngularLower) || !finite(joint.mAngularUpper))
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
            info.m_friction = 0.8;
            info.m_restitution = 0;
            auto body = std::make_unique<btRigidBody>(info);
            body->setSleepingThresholds(1, 0.15);
            body->setCcdMotionThreshold(definition.mRadius * 0.5);
            body->setCcdSweptSphereRadius(definition.mRadius * 0.8);
            // Character body replacement meshes often overlap at the joints.
            // Self-collision would push these apart violently; the constraints
            // define the articulated body, and every capsule collides with scenery.
            mImpl->mWorld->addRigidBody(body.get(), sBodyGroup, sStaticGroup);
            mImpl->mShapes.push_back(std::move(shape));
            mImpl->mBodies.push_back(std::move(body));
            mImpl->mLastFinitePose.push_back(definition.mTransform);
        }
        mImpl->mConstraints.reserve(joints.size());
        for (const JointDefinition& joint : joints)
        {
            btRigidBody& parent = *mImpl->mBodies[joint.mParent];
            btRigidBody& child = *mImpl->mBodies[joint.mChild];
            const btTransform localParent = parent.getWorldTransform().inverse() * joint.mFrameWorld;
            const btTransform localChild = child.getWorldTransform().inverse() * joint.mFrameWorld;
            auto constraint = std::make_unique<btGeneric6DofConstraint>(parent, child, localParent, localChild, true);
            constraint->setLinearLowerLimit(btVector3(0, 0, 0));
            constraint->setLinearUpperLimit(btVector3(0, 0, 0));
            constraint->setAngularLowerLimit(joint.mAngularLower);
            constraint->setAngularUpperLimit(joint.mAngularUpper);
            mImpl->mWorld->addConstraint(constraint.get(), true);
            mImpl->mConstraints.push_back(std::move(constraint));
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

    std::size_t Simulation::getNumBodies() const
    {
        return mImpl->mBodies.size();
    }

    const btTransform& Simulation::getBodyTransform(std::size_t body) const
    {
        return mImpl->mBodies.at(body)->getWorldTransform();
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
