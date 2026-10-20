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
            if ((settings.mBodyMode != BodyMode::Articulated && settings.mBodyMode != BodyMode::Soft
                    && settings.mBodyMode != BodyMode::Rigid)
                || !finite(settings.mGravity) || settings.mGravity.length2() > 1e12
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
                if (mOwner.clampRigidMotion(dt))
                    return;
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
        std::unique_ptr<btCompoundShape> mCompoundShape;
        std::vector<std::unique_ptr<btRigidBody>> mBodies;
        // A rigid corpse has one Bullet body and one immutable local transform
        // for each visible part. The public indices always refer to those parts.
        std::vector<btTransform> mRigidOffsets;
        std::vector<btTransform> mPartTransforms;
        std::vector<btScalar> mPartMasses;
        std::size_t mJointCount = 0;
        std::vector<std::unique_ptr<btTypedConstraint>> mConstraints;
        std::unique_ptr<btPoint2PointConstraint> mPin;
        std::vector<std::pair<std::size_t, std::size_t>> mInitiallyOverlapping;
        btScalar mTotalMass = 0;
        std::optional<btScalar> mWaterLevel;
        btVector3 mAttractionTarget{ 0, 0, 0 };
        btScalar mAttractionLeft = 0;
        btScalar mAttractionDuration = 0;
        btScalar mAttractionDistance = 0;
        btScalar mAttractionMass = 0;
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
                if (constraint->isEnabled())
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
            mCompoundShape.reset();
            mShapes.clear();
        }

        btRigidBody& bodyForPart(std::size_t part)
        {
            // Validate against visible part count even for a single rigid body.
            (void)mPartMasses.at(part);
            return *mBodies[mRigidOffsets.empty() ? part : 0];
        }

        bool clampRigidMotion(btScalar dt)
        {
            if (!mCompoundShape || !mBodies.front()->isActive())
                return false;
            btRigidBody& body = *mBodies.front();
            btTransform predicted;
            body.predictIntegratedTransform(dt, predicted);
            btScalar fraction = 1;
            // Bullet's standard CCD handles convex shapes only. A compound is
            // non-convex, so sweep its capsule children and clamp the one rigid
            // transform to the earliest contact instead of letting a frozen
            // corpse tunnel through thin walls at maximum launch velocity.
            struct Sweep : btCollisionWorld::ClosestConvexResultCallback
            {
                const btRigidBody& mBody;
                Sweep(const btRigidBody& body, const btVector3& from, const btVector3& to)
                    : btCollisionWorld::ClosestConvexResultCallback(from, to)
                    , mBody(body)
                {
                    m_collisionFilterGroup = sBodyGroup;
                    m_collisionFilterMask = sStaticGroup;
                }

                btScalar addSingleResult(btCollisionWorld::LocalConvexResult& result, bool worldNormal) override
                {
                    const btVector3 normal = worldNormal ? result.m_hitNormalLocal
                        : result.m_hitCollisionObject->getWorldTransform().getBasis() * result.m_hitNormalLocal;
                    const btVector3 velocity = mBody.getVelocityInLocalPoint(
                        result.m_hitPointLocal - mBody.getCenterOfMassPosition());
                    // Existing touching surfaces must not prevent motion away
                    // from them or a rigid corpse rolling along the ground.
                    if (normal.dot(velocity) >= -1e-6)
                        return 1;
                    return ClosestConvexResultCallback::addSingleResult(result, worldNormal);
                }
            };
            btScalar remaining = dt;
            bool clamped = false;
            for (int iteration = 0; iteration < 12; ++iteration)
            {
                body.predictIntegratedTransform(remaining, predicted);
                fraction = 1;
                btVector3 contactNormal(0, 0, 0), contactPoint(0, 0, 0);
                for (std::size_t i = 0; i < mRigidOffsets.size(); ++i)
                {
                    const auto& capsule = static_cast<const btCapsuleShape&>(*mShapes[i]);
                    const btTransform from = body.getWorldTransform() * mRigidOffsets[i];
                    const btTransform to = predicted * mRigidOffsets[i];
                    const btScalar motion = from.getOrigin().distance(to.getOrigin())
                        + body.getAngularVelocity().length() * remaining * (capsule.getHalfHeight() + capsule.getRadius());
                    if (!clamped && motion < capsule.getRadius() * 0.5)
                        continue;
                    Sweep sweep(body, from.getOrigin(), to.getOrigin());
                    sweep.m_closestHitFraction = fraction;
                    mWorld->convexSweepTest(&capsule, from, to, sweep);
                    if (sweep.hasHit())
                    {
                        fraction = sweep.m_closestHitFraction;
                        contactNormal = sweep.m_hitNormalWorld.normalized();
                        contactPoint = sweep.m_hitPointWorld;
                    }
                }
                if (fraction >= 1)
                {
                    if (!clamped) return false;
                    body.proceedToTransform(predicted);
                    break;
                }
                clamped = true;
                body.predictIntegratedTransform(remaining * fraction, predicted);
                body.proceedToTransform(predicted);
                // Resolve the inward contact velocity, then sweep the remaining
                // tangential motion. Merely setting hitFraction=0 freezes an icy
                // statue at its first touching surface, even on a steep slope.
                const btVector3 offset = contactPoint - body.getCenterOfMassPosition();
                const btScalar inward = contactNormal.dot(body.getVelocityInLocalPoint(offset));
                const btScalar denominator = body.computeImpulseDenominator(contactPoint, contactNormal);
                if (inward < 0 && denominator > SIMD_EPSILON)
                    body.applyImpulse(contactNormal * (-inward / denominator), offset);
                remaining *= 1 - fraction;
                if (remaining < 1e-7) break;
            }
            body.setHitFraction(0);
            return true;
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
                if (!constraint->isEnabled() || constraint->getConstraintType() != HINGE_CONSTRAINT_TYPE)
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
                if (btFabs(safeSpeed - speed) < SIMD_EPSILON)
                    continue;

                // A torque around the centres of mass alone changes the joint
                // anchor velocities. Applied after Bullet's contact solve, that
                // would stretch the limb before the next substep can repair it.
                // Project the angular stop impulse into the point constraint:
                // K maps an anchor impulse to relative point velocity, while the
                // compensating impulse cancels the point motion from unit torque.
                const btVector3 parentArm = parent.getWorldTransform().getBasis() * hinge.getAFrame().getOrigin();
                const btVector3 childArm = child.getWorldTransform().getBasis() * hinge.getBFrame().getOrigin();
                const btMatrix3x3& parentInertia = parent.getInvInertiaTensorWorld();
                const btMatrix3x3& childInertia = child.getInvInertiaTensorWorld();
                const auto pointResponse = [&](const btVector3& impulse) {
                    return impulse * (parent.getInvMass() + child.getInvMass())
                        + (parentInertia * parentArm.cross(impulse)).cross(parentArm)
                        + (childInertia * childArm.cross(impulse)).cross(childArm);
                };
                const btVector3 x = pointResponse(btVector3(1, 0, 0));
                const btVector3 y = pointResponse(btVector3(0, 1, 0));
                const btVector3 z = pointResponse(btVector3(0, 0, 1));
                const btMatrix3x3 response(x.x(), y.x(), z.x(), x.y(), y.y(), z.y(), x.z(), y.z(), z.z());
                const btVector3 pointMotion = (parentInertia * axis).cross(parentArm)
                    + (childInertia * axis).cross(childArm);
                const btVector3 compensation = -(response.inverse() * pointMotion);
                const btScalar inverseInertia = axis.dot((parentInertia + childInertia) * axis
                    + parentInertia * parentArm.cross(compensation) + childInertia * childArm.cross(compensation));
                if (inverseInertia <= SIMD_EPSILON)
                    continue;
                const btScalar magnitude = (safeSpeed - speed) / inverseInertia;
                const btVector3 torque = axis * magnitude;
                const btVector3 impulse = compensation * magnitude;
                parent.applyTorqueImpulse(-torque);
                child.applyTorqueImpulse(torque);
                parent.applyImpulse(-impulse, parentArm);
                child.applyImpulse(impulse, childArm);
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
        mImpl->mPartMasses.reserve(bodies.size());
        mImpl->mJointCount = joints.size();
        for (const BodyDefinition& definition : bodies)
        {
            mImpl->mShapes.push_back(std::make_unique<btCapsuleShape>(definition.mRadius, definition.mLength));
            mImpl->mPartMasses.push_back(definition.mMass);
            mImpl->mTotalMass += definition.mMass;
        }
        const auto addBody = [&](btCollisionShape* shape, const btTransform& transform,
                                 btScalar mass, const btVector3& inertia, btScalar radius) {
            btRigidBody::btRigidBodyConstructionInfo info(mass, nullptr, shape, inertia);
            info.m_startWorldTransform = transform;
            info.m_linearDamping = settings.mLinearDamping;
            info.m_angularDamping = settings.mBodyMode == BodyMode::Rigid ? 0.08 : settings.mAngularDamping;
            info.m_friction = settings.mBodyMode == BodyMode::Rigid ? 0.22 : 0.85;
            info.m_rollingFriction = settings.mBodyMode == BodyMode::Rigid ? 0.005 : 0.05;
            info.m_spinningFriction = settings.mBodyMode == BodyMode::Rigid ? 0.01 : 0.1;
            info.m_restitution = 0;
            auto body = std::make_unique<btRigidBody>(info);
            body->setSleepingThresholds(1, 0.15);
            if (settings.mBodyMode == BodyMode::Rigid)
                body->setActivationState(DISABLE_DEACTIVATION);
            body->setCcdMotionThreshold(radius * 0.5);
            body->setCcdSweptSphereRadius(radius * 0.8);
            mImpl->mWorld->addRigidBody(body.get(), sBodyGroup, sStaticGroup | sBodyGroup);
            mImpl->mBodies.push_back(std::move(body));
            mImpl->mLastFinitePose.push_back(transform);
        };
        if (settings.mBodyMode == BodyMode::Rigid)
        {
            // Freeze every captured part into one collision object, with the
            // actual mass-weighted centre and principal moments of inertia.
            // Locked articulated constraints would still flex under hard impacts.
            btTransform origin = btTransform::getIdentity();
            btVector3 centre(0, 0, 0);
            for (const BodyDefinition& definition : bodies)
                centre += definition.mTransform.getOrigin() * (definition.mMass / mImpl->mTotalMass);
            origin.setOrigin(centre);
            mImpl->mCompoundShape = std::make_unique<btCompoundShape>(true, static_cast<int>(bodies.size()));
            for (std::size_t i = 0; i < bodies.size(); ++i)
                mImpl->mCompoundShape->addChildShape(origin.inverse() * bodies[i].mTransform, mImpl->mShapes[i].get());
            btTransform principal;
            btVector3 inertia;
            mImpl->mCompoundShape->calculatePrincipalAxisTransform(mImpl->mPartMasses.data(), principal, inertia);
            mImpl->mRigidOffsets.reserve(bodies.size());
            mImpl->mPartTransforms.reserve(bodies.size());
            btScalar minRadius = bodies.front().mRadius;
            for (std::size_t i = 0; i < bodies.size(); ++i)
            {
                const btTransform local = principal.inverse() * mImpl->mCompoundShape->getChildTransform(static_cast<int>(i));
                mImpl->mCompoundShape->updateChildTransform(static_cast<int>(i), local, false);
                mImpl->mRigidOffsets.push_back(local);
                mImpl->mPartTransforms.push_back(bodies[i].mTransform);
                minRadius = std::min(minRadius, bodies[i].mRadius);
            }
            mImpl->mCompoundShape->recalculateLocalAabb();
            addBody(mImpl->mCompoundShape.get(), origin * principal, mImpl->mTotalMass, inertia, minRadius);
            replaceStaticColliders(std::move(colliders));
            return;
        }
        for (std::size_t i = 0; i < bodies.size(); ++i)
        {
            btVector3 inertia;
            mImpl->mShapes[i]->calculateLocalInertia(bodies[i].mMass, inertia);
            addBody(mImpl->mShapes[i].get(), bodies[i].mTransform, bodies[i].mMass, inertia, bodies[i].mRadius);
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
            if (settings.mBodyMode == BodyMode::Soft)
            {
                // Acid removes anatomical angular stiffness, but the attachment
                // points stay locked so the body bends freely without separating.
                constraint = std::make_unique<btPoint2PointConstraint>(
                    parent, child, localParent.getOrigin(), localChild.getOrigin());
            }
            else if (joint.mType == JointDefinition::Type::Hinge)
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

    void Simulation::attract(const btVector3& target, btScalar distance, btScalar mass, btScalar duration)
    {
        if (!finite(target) || !finite(distance) || !finite(mass) || !finite(duration) || distance < 0 || mass <= 0
            || duration <= 0 || duration > 10)
            throw std::invalid_argument("Invalid corpse attraction");
        mImpl->mAttractionTarget = target;
        mImpl->mAttractionDistance = distance;
        mImpl->mAttractionMass = mass;
        mImpl->mAttractionDuration = duration;
        mImpl->mAttractionLeft = duration;
    }

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
            for (const auto& body : mImpl->mBodies)
            {
                btVector3 minimum, maximum;
                body->getAabb(minimum, maximum);
                const btScalar submerged = mImpl->mWaterLevel && !mImpl->mPin
                    ? std::clamp(
                          (*mImpl->mWaterLevel - minimum.z()) / std::max(btScalar(.1), maximum.z() - minimum.z()),
                          btScalar(0), btScalar(1))
                    : 0;
                if (submerged > 0)
                {
                    body->activate(true);
                    // All included corpses float, including heavy equipped ones.
                    // This gameplay buoyancy uses the body's current mass once.
                    body->applyCentralForce(
                        btVector3(0, 0, -mImpl->mSettings.mGravity.z() * body->getMass() * 1.4 * submerged)
                        - body->getLinearVelocity() * (body->getMass() * 4 * submerged));
                }
                body->setDamping(mImpl->mSettings.mLinearDamping,
                    std::max(mImpl->mSettings.mBodyMode == BodyMode::Rigid ? btScalar(.08)
                                                                           : mImpl->mSettings.mAngularDamping,
                        btScalar(.7) * submerged));
            }
            if (mImpl->mAttractionLeft > 0 && !mImpl->mPin)
            {
                btVector3 center(0, 0, 0), velocity(0, 0, 0);
                for (const auto& body : mImpl->mBodies)
                {
                    center += body->getCenterOfMassPosition() * body->getMass();
                    velocity += body->getLinearVelocity() * body->getMass();
                }
                center /= mImpl->mTotalMass;
                velocity /= mImpl->mTotalMass;
                const btVector3 delta = mImpl->mAttractionTarget - center;
                const btScalar distance = delta.length();
                if (distance > 0.01)
                {
                    const auto direction = delta / distance;
                    const auto duration = mImpl->mAttractionDuration;
                    const auto referenceMass = mImpl->mAttractionMass;
                    const auto spring = 16 * referenceMass / (duration * duration);
                    const auto drag = 8 * referenceMass / duration;
                    const auto maximum = 9.143 * referenceMass * mImpl->mAttractionDistance / (duration * duration);
                    // Critical damping for the reference mass; light bodies are
                    // overdamped and heavy bodies accelerate less. No target
                    // transform, per-limb compression, or teleport is imposed.
                    const auto force
                        = std::clamp(spring * distance - drag * velocity.dot(direction), -maximum, maximum);
                    for (const auto& body : mImpl->mBodies)
                    {
                        body->activate(true);
                        body->applyCentralForce(direction * force * (body->getMass() / mImpl->mTotalMass));
                    }
                }
            }
            mImpl->mAttractionLeft = std::max(btScalar(0), mImpl->mAttractionLeft - step);
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

    Simulation::AttractionState Simulation::attractionState() const
    {
        return { mImpl->mAttractionTarget, mImpl->mAttractionDistance, mImpl->mAttractionMass,
            mImpl->mAttractionDuration, mImpl->mAttractionLeft };
    }
    void Simulation::restoreAttraction(const AttractionState& state)
    {
        if (state.mTimeLeft <= 0)
            return;
        if (!finite(state.mTimeLeft) || state.mTimeLeft > state.mDuration)
            throw std::invalid_argument("Invalid remaining attraction time");
        attract(state.mTarget, state.mDistance, state.mReferenceMass, state.mDuration);
        mImpl->mAttractionLeft = state.mTimeLeft;
    }
    void Simulation::setBodyVelocity(std::size_t part, const btVector3& linear, const btVector3& angular)
    {
        if (!finite(linear) || !finite(angular))
            throw std::invalid_argument("Invalid saved body velocity");
        auto& body = mImpl->bodyForPart(part);
        body.setAngularVelocity(angular);
        body.setLinearVelocity(
            linear - angular.cross(getBodyTransform(part).getOrigin() - body.getCenterOfMassPosition()));
        body.activate(true);
        limitVelocity(body, mImpl->mSettings);
    }
    void Simulation::setWaterLevel(std::optional<btScalar> level)
    {
        if (level && !finite(*level))
            throw std::invalid_argument("Invalid water level");
        mImpl->mWaterLevel = level;
    }
    bool Simulation::isBelowWaterSurface() const
    {
        if (!mImpl->mWaterLevel || mImpl->mPin)
            return false;
        btVector3 minimum(1e20, 1e20, 1e20), maximum(-1e20, -1e20, -1e20);
        for (const auto& body : mImpl->mBodies)
        {
            btVector3 a, b;
            body->getAabb(a, b);
            minimum.setMin(a);
            maximum.setMax(b);
        }
        return (minimum.z() + maximum.z()) * .5 < *mImpl->mWaterLevel - (maximum.z() - minimum.z()) * .4;
    }
    void Simulation::setTotalMass(btScalar mass)
    {
        if (!finite(mass) || mass <= 0 || mass > 5000)
            throw std::invalid_argument("Invalid updated corpse mass");
        const auto ratio = mass / mImpl->mTotalMass;
        for (auto& partMass : mImpl->mPartMasses)
            partMass *= ratio;
        for (const auto& body : mImpl->mBodies)
        {
            btVector3 inertia;
            const auto bodyMass = body->getMass() * ratio;
            body->getCollisionShape()->calculateLocalInertia(bodyMass, inertia);
            body->setMassProps(bodyMass, inertia);
            body->updateInertiaTensor();
        }
        mImpl->mTotalMass = mass;
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

    bool Simulation::sweepPartStatic(
        std::size_t part, const btVector3& displacement, btVector3& point, btVector3& normal) const
    {
        if (!finite(displacement) || displacement.fuzzyZero())
            return false;
        const auto from = getBodyTransform(part);
        auto to = from;
        to.setOrigin(from.getOrigin() + displacement);
        struct Sweep : btCollisionWorld::ClosestConvexResultCallback
        {
            btVector3 mDirection;
            Sweep(const btVector3& from, const btVector3& to, const btVector3& direction)
                : ClosestConvexResultCallback(from, to)
                , mDirection(direction)
            {
            }
            btScalar addSingleResult(btCollisionWorld::LocalConvexResult& result, bool worldNormal) override
            {
                const auto normal = worldNormal
                    ? result.m_hitNormalLocal
                    : result.m_hitCollisionObject->getWorldTransform().getBasis() * result.m_hitNormalLocal;
                if (btFabs(normal.z()) >= 0.65 || normal.dot(mDirection) >= -0.2)
                    return 1;
                return ClosestConvexResultCallback::addSingleResult(result, worldNormal);
            }
        } sweep(from.getOrigin(), to.getOrigin(), displacement.normalized());
        sweep.m_collisionFilterGroup = sBodyGroup;
        sweep.m_collisionFilterMask = sStaticGroup;
        mImpl->mWorld->convexSweepTest(
            static_cast<const btConvexShape*>(mImpl->mShapes.at(part).get()), from, to, sweep);
        if (!sweep.hasHit())
            return false;
        point = sweep.m_hitPointWorld;
        normal = sweep.m_hitNormalWorld;
        return true;
    }

    void Simulation::applyImpulse(std::size_t body, const btVector3& impulse, const btVector3& worldPoint)
    {
        if (!finite(impulse) || !finite(worldPoint))
            throw std::invalid_argument("Invalid ragdoll impulse");
        btRigidBody& target = mImpl->bodyForPart(body);
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

    void Simulation::detachJoint(std::size_t joint)
    {
        auto& constraint = *mImpl->mConstraints.at(joint);
        if (!constraint.isEnabled())
            return;
        mImpl->mWorld->removeConstraint(&constraint);
        constraint.setEnabled(false);
        constraint.getRigidBodyA().activate(true);
        constraint.getRigidBodyB().activate(true);
    }

    void Simulation::applyJointTorqueImpulse(std::size_t joint, const btVector3& impulse)
    {
        if (!finite(impulse))
            throw std::invalid_argument("Invalid ragdoll joint torque");
        if (joint >= mImpl->mJointCount)
            throw std::out_of_range("Invalid ragdoll joint index");
        // Internal torques cannot deform or launch a single frozen solid body.
        if (!mImpl->mRigidOffsets.empty())
            return;
        btTypedConstraint& constraint = *mImpl->mConstraints.at(joint);
        if (!mImpl->mValid || !constraint.isEnabled())
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
        btRigidBody& target = mImpl->bodyForPart(body);
        if (!mImpl->mValid || mImpl->mPin)
            return false;
        const btVector3 partLocal = getBodyTransform(body).inverse() * worldPoint;
        // A pin is a short bolt through a body, never an invisible tether to a
        // distant wall. Caller must wait for an actual collision approach.
        const auto& shape = static_cast<const btCapsuleShape&>(*mImpl->mShapes[body]);
        if (partLocal.length() > shape.getHalfHeight() + shape.getRadius() * 2)
            return false;
        const btVector3 local = target.getWorldTransform().inverse() * worldPoint;
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
        return mImpl->mPartMasses.size();
    }

    const btTransform& Simulation::getBodyTransform(std::size_t body) const
    {
        if (!mImpl->mRigidOffsets.empty())
        {
            btTransform& result = mImpl->mPartTransforms.at(body);
            result = mImpl->mBodies.front()->getWorldTransform() * mImpl->mRigidOffsets.at(body);
            return result;
        }
        return mImpl->mBodies.at(body)->getWorldTransform();
    }

    btVector3 Simulation::getBodyLinearVelocity(std::size_t body) const
    {
        btRigidBody& target = mImpl->bodyForPart(body);
        return target.getVelocityInLocalPoint(getBodyTransform(body).getOrigin() - target.getCenterOfMassPosition());
    }

    const btVector3& Simulation::getBodyAngularVelocity(std::size_t body) const
    {
        return mImpl->bodyForPart(body).getAngularVelocity();
    }

    btScalar Simulation::getBodyMass(std::size_t body) const
    {
        return mImpl->mPartMasses.at(body);
    }

    btScalar Simulation::getTotalMass() const
    {
        return mImpl->mTotalMass;
    }

    btScalar Simulation::getJointAngle(std::size_t joint) const
    {
        if (joint >= mImpl->mJointCount)
            throw std::out_of_range("Invalid ragdoll joint index");
        if (!mImpl->mRigidOffsets.empty())
            return 0;
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
