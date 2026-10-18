#include "ragdoll.hpp"
#include "ragdollappearance.hpp"

#include <components/misc/strings/lower.hpp>
#include <components/misc/constants.hpp>
#include <components/ragdoll/simulation.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/skeleton.hpp>

#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>

#include <BulletCollision/CollisionShapes/btBvhTriangleMeshShape.h>
#include <BulletCollision/CollisionShapes/btTriangleMesh.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <utility>

namespace MWRender
{
    namespace
    {
        // Animation destruction may happen on the resource-unref thread.
        std::atomic<int> sSimulatingCorpses{ 0 };

        btVector3 toBullet(const osg::Vec3f& v) { return { v.x(), v.y(), v.z() }; }
        osg::Vec3f toOsg(const btVector3& v)
        {
            return { static_cast<float>(v.x()), static_cast<float>(v.y()), static_cast<float>(v.z()) };
        }

        osg::Matrixf toOsg(const btTransform& transform)
        {
            const btQuaternion rotation = transform.getRotation();
            return osg::Matrixf::rotate(osg::Quat(rotation.x(), rotation.y(), rotation.z(), rotation.w()))
                * osg::Matrixf::translate(toOsg(transform.getOrigin()));
        }

        osg::Vec3f unit(osg::Vec3f value, const osg::Vec3f& fallback = osg::Vec3f(0, 1, 0))
        {
            if (!std::isfinite(value.length2()) || value.normalize() < 0.0001f)
                return fallback;
            return value;
        }

        // A joint's X axis follows the segment and Z is the anatomical flexion axis.
        // Separate parent/child frames retain the real flexion angle at death; zero
        // means straight, rather than whichever arbitrary pose was captured.
        btTransform hingeFrame(const osg::Vec3f& point, const osg::Vec3f& segment, const osg::Vec3f& bendAxis)
        {
            const osg::Vec3f x = unit(segment);
            const osg::Vec3f z = unit(bendAxis - x * (bendAxis * x), unit(x ^ osg::Vec3f(0, 0, 1)));
            const osg::Vec3f y = unit(z ^ x);
            btMatrix3x3 basis(x.x(), y.x(), z.x(), x.y(), y.y(), z.y(), x.z(), y.z(), z.z());
            return btTransform(basis, toBullet(point));
        }

        btTransform capsuleTransform(const osg::Vec3f& start, const osg::Vec3f& end)
        {
            osg::Quat rotation;
            rotation.makeRotate(osg::Vec3f(0, 1, 0), end - start);
            return btTransform(btQuaternion(rotation.x(), rotation.y(), rotation.z(), rotation.w()),
                toBullet((start + end) * 0.5f));
        }

        osg::Matrixf rootWorldMatrix(const osg::Node& node)
        {
            const osg::NodePathList paths = node.getParentalNodePaths();
            return paths.empty() ? osg::Matrixf::identity() : osg::Matrixf(osg::computeLocalToWorld(paths.front()));
        }

        class PoseCallback : public SceneUtil::NodeCallback<PoseCallback, osg::MatrixTransform*>
        {
        public:
            osg::Matrixf mMatrix;

            void operator()(osg::MatrixTransform* node, osg::NodeVisitor* visitor)
            {
                node->setMatrix(mMatrix);
                traverse(node, visitor);
            }
        };

        struct BonePose
        {
            osg::ref_ptr<osg::MatrixTransform> mNode;
            osg::NodePath mParents;
            osg::Matrixf mRestLocal;
            osg::Matrixf mRestWorld;
            osg::ref_ptr<PoseCallback> mCallback;
            int mBody = -1;
            osg::Matrixf mBoneToBody;
        };

        class CollectBones : public osg::NodeVisitor
        {
        public:
            std::vector<BonePose> mBones;
            std::map<std::string, std::size_t, std::less<>> mNames;
            const osg::Matrixf mRootWorld;

            explicit CollectBones(const osg::Matrixf& rootWorld)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mRootWorld(rootWorld)
            {
            }

            void apply(osg::MatrixTransform& node) override
            {
                // Freeze the complete transform hierarchy, including unmodelled neck, hands and feet.
                // The eleven rigid bodies then drive the appropriate subset in parent-first order.
                BonePose pose;
                pose.mNode = &node;
                pose.mParents = getNodePath();
                pose.mParents.pop_back();
                pose.mRestLocal = node.getMatrix();
                pose.mRestWorld = pose.mRestLocal * osg::computeLocalToWorld(pose.mParents) * mRootWorld;
                pose.mCallback = new PoseCallback;
                pose.mCallback->mMatrix = pose.mRestLocal;
                mNames.emplace(Misc::StringUtils::lowerCase(node.getName()), mBones.size());
                mBones.push_back(std::move(pose));
                traverse(node);
            }
        };

        struct CollisionMesh
        {
            // Destruction order matters: the shape must die before its triangle storage.
            btTriangleMesh mTriangles;
            std::unique_ptr<btBvhTriangleMeshShape> mShape;
        };

        std::vector<Ragdoll::StaticCollider> makeCollisionMesh(const std::vector<osg::Vec3f>& vertices)
        {
            if (vertices.empty() || vertices.size() % 3 != 0)
                return {};
            auto mesh = std::make_shared<CollisionMesh>();
            for (std::size_t i = 0; i < vertices.size(); i += 3)
                mesh->mTriangles.addTriangle(toBullet(vertices[i]), toBullet(vertices[i + 1]), toBullet(vertices[i + 2]));
            mesh->mShape = std::make_unique<btBvhTriangleMeshShape>(&mesh->mTriangles, true);
            Ragdoll::StaticCollider collider;
            collider.mShape = std::shared_ptr<btCollisionShape>(mesh, mesh->mShape.get());
            collider.mTransform.setIdentity();
            return { std::move(collider) };
        }
    }

    struct RagdollController::Impl
    {
        osg::ref_ptr<osg::Group> mRoot;
        osg::ref_ptr<SceneUtil::Skeleton> mSkeleton;
        CollisionSnapshot mSnapshot;
        Settings mSettings;
        std::vector<BonePose> mBones;
        std::unique_ptr<Ragdoll::Simulation> mSimulation;
        std::unique_ptr<RagdollAppearance> mAppearance;
        RagdollAppearance::SkinParts mSkinParts;
        std::vector<osg::Matrixf> mBodyMatrices;
        std::vector<osg::Vec3f> mCollisionVertices;
        float mElapsed = 0.f;
        float mSinceSnapshot = 0.f;
        bool mBudgetSlot = false;
        float mMass = 80.f;
        float mTorsoLength = 40.f;
        float mChestRadius = 8.f;
        float mPelvisRadius = 8.f;
        bool mShock = false;
        bool mPinPending = false;
        bool mPinned = false;
        osg::Vec3f mPinDirection;
        osg::Vec3f mPinStart;
        osg::Vec3f mRight;
        osg::Vec3f mForward;

        void updateAppearance()
        {
            std::array<osg::Vec3f, 11> positions;
            for (std::size_t i = 0; i < positions.size(); ++i)
                positions[i] = mBodyMatrices[i].getTrans();
            mAppearance->update(mElapsed, mSkinParts, positions);
        }

        void updateShock(float timeStep)
        {
            if (!mShock || mElapsed > mSettings.mShockDuration)
                return;
            // Internal equal-and-opposite impulses flex joints without launching
            // the centre of mass. Smooth decay and solver limits bound every spasm.
            const float envelope = 1.f - mElapsed / std::max(0.001f, mSettings.mShockDuration);
            const float strength = std::min(2000.f, mMass * mTorsoLength * mTorsoLength * 0.004f)
                * mSettings.mShockStrength * envelope * timeStep * 12.f;
            for (const std::size_t joint : { 2u, 3u, 4u, 5u, 7u, 9u })
            {
                const float pulse = std::sin(mElapsed * 44.f + joint * 2.1f);
                const osg::Vec3f axis = joint >= 7 ? mRight : mForward;
                mSimulation->applyJointTorqueImpulse(joint, toBullet(axis * (strength * pulse)));
            }
        }

        void updatePin()
        {
            if (!mPinPending || mPinned)
                return;
            const osg::Vec3f chest = toOsg(mSimulation->getBodyTransform(1).getOrigin());
            if (mElapsed > 2.f || (chest - mPinStart).length() > mSettings.mPinMaxDistance * Constants::UnitsPerMeter)
            {
                mPinPending = false;
                return;
            }
            for (const std::size_t body : { 1u, 0u })
            {
                const osg::Vec3f center = toOsg(mSimulation->getBodyTransform(body).getOrigin());
                const float radius = body == 1 ? mChestRadius : mPelvisRadius;
                btVector3 point, normal;
                const osg::Vec3f end = center + mPinDirection * (radius + mSettings.mPinContactDistance);
                if (mSimulation->raycastStatic(toBullet(center), toBullet(end), point, normal)
                    && std::abs(normal.z()) < 0.65 && normal.dot(toBullet(mPinDirection)) < -0.2)
                {
                    // Attach the torso/pelvis that actually reached the surface.
                    // Limbs striking first cannot create a long invisible tether.
                    mPinned = mSimulation->pinBody(body, point);
                    if (mPinned)
                    {
                        mPinPending = false;
                        return;
                    }
                }
            }
        }

        ~Impl()
        {
            for (const BonePose& bone : mBones)
            {
                bone.mNode->removeUpdateCallback(bone.mCallback);
                bone.mCallback->setNestedCallback(nullptr);
                // Unanimated bones must also return to their captured offsets on resurrection.
                bone.mNode->setMatrix(bone.mRestLocal);
            }
            releaseBudget();
        }

        void releaseBudget()
        {
            if (mBudgetSlot)
            {
                --sSimulatingCorpses;
                mBudgetSlot = false;
            }
        }

        void freeze()
        {
            // Keep the final matrices and pose overrides, but release physics world and mesh memory.
            mSimulation.reset();
            std::vector<osg::Vec3f>().swap(mCollisionVertices);
            releaseBudget();
        }

        void applyPose()
        {
            const osg::Matrixf world = rootWorldMatrix(*mRoot);
            for (BonePose& bone : mBones)
            {
                osg::Matrixf local = bone.mRestLocal;
                if (bone.mBody >= 0)
                {
                    const osg::Matrixf parentWorld = osg::computeLocalToWorld(bone.mParents) * world;
                    local = bone.mBoneToBody * mBodyMatrices[bone.mBody] * osg::Matrixf::inverse(parentWorld);
                }
                bone.mCallback->mMatrix = local;
                // Pre-apply before traversal: skinned body parts may precede some bones in the graph.
                bone.mNode->setMatrix(local);
            }
            mSkeleton->setActive(SceneUtil::Skeleton::Active);
        }
    };

    RagdollController::RagdollController(std::unique_ptr<Impl> impl)
        : mImpl(std::move(impl))
    {
    }

    RagdollController::~RagdollController() = default;

    std::unique_ptr<RagdollController> RagdollController::create(osg::Group& root,
        SceneUtil::Skeleton& skeleton, CollisionSnapshot snapshot, const Settings& settings, const Ragdoll::Impact& impact)
    {
        if (&root != &skeleton || sSimulatingCorpses.load() >= settings.mMaxActive)
            return nullptr;

        CollectBones visitor(rootWorldMatrix(root));
        root.accept(visitor);
        const auto find = [&](std::initializer_list<const char*> names) -> int {
            for (const char* name : names)
                if (const auto found = visitor.mNames.find(name); found != visitor.mNames.end())
                    return static_cast<int>(found->second);
            return -1;
        };

        // Morrowind's standard humanoid skeleton. Custom or creature skeletons are not guessed.
        const std::array<int, 11> driven = { find({ "bip01 pelvis" }),
            find({ "bip01 spine2", "bip01 spine1", "bip01 spine" }), find({ "bip01 head" }),
            find({ "bip01 l upperarm" }), find({ "bip01 l forearm" }),
            find({ "bip01 r upperarm" }), find({ "bip01 r forearm" }),
            find({ "bip01 l thigh" }), find({ "bip01 l calf" }),
            find({ "bip01 r thigh" }), find({ "bip01 r calf" }) };
        const std::array<int, 5> ends = { find({ "bip01 neck" }), find({ "bip01 l hand" }),
            find({ "bip01 r hand" }), find({ "bip01 l foot" }), find({ "bip01 r foot" }) };
        if (std::ranges::any_of(driven, [](int i) { return i < 0; })
            || std::ranges::any_of(ends, [](int i) { return i < 0; }))
            return nullptr;

        const auto position = [&](int index) { return osg::Vec3f(visitor.mBones[index].mRestWorld.getTrans()); };
        const osg::Vec3f pelvis = position(driven[0]);
        const osg::Vec3f neck = position(ends[0]);
        const float torsoLength = (neck - pelvis).length();
        // Degenerate transforms, unusual scale or an uninitialised skeleton fall back to native death.
        if (!std::isfinite(torsoLength) || torsoLength < 8.f || torsoLength > 512.f)
            return nullptr;

        auto vertices = snapshot(pelvis, settings.mCollisionRadius);
        auto colliders = makeCollisionMesh(vertices);
        if (colliders.empty())
            return nullptr;

        std::array<osg::Vec3f, 11> starts;
        std::array<osg::Vec3f, 11> stops;
        for (std::size_t i = 0; i < driven.size(); ++i)
            starts[i] = position(driven[i]);
        // Torso includes the intermediate spine bones, preserving their captured local offsets.
        stops[0] = pelvis + (neck - pelvis) * 0.3f;
        starts[1] = stops[0];
        stops[1] = neck;
        osg::Vec3f headAxis = position(driven[2]) - neck;
        if (headAxis.length2() < 1.f)
            headAxis = (neck - pelvis) * 0.2f;
        starts[2] = neck;
        stops[2] = position(driven[2]) + headAxis * 0.5f;
        stops[3] = position(driven[4]);
        stops[4] = position(ends[1]);
        stops[5] = position(driven[6]);
        stops[6] = position(ends[2]);
        stops[7] = position(driven[8]);
        stops[8] = position(ends[3]);
        stops[9] = position(driven[10]);
        stops[10] = position(ends[4]);

        const float legLength = ((stops[7] - starts[7]).length() + (stops[8] - starts[8]).length()
            + (stops[9] - starts[9]).length() + (stops[10] - starts[10]).length()) * 0.5f;
        const float stature = legLength + torsoLength + (stops[2] - starts[2]).length();
        const float heightRatio = std::clamp(stature / settings.mReferenceHeight, 0.25f, 4.f);
        const float mass = std::clamp(settings.mBaseMass * heightRatio * heightRatio * heightRatio
            + std::max(0.f, settings.mArmorMass), 5.f, 5000.f);
        const osg::Matrixf rootMatrix = rootWorldMatrix(root);
        const osg::Vec3f forward = unit(osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), rootMatrix));
        const osg::Vec3f right = unit(osg::Matrixf::transform3x3(osg::Vec3f(1, 0, 0), rootMatrix));
        constexpr std::array<float, 11> masses{ 10.f, 20.f, 5.f, 3.f, 2.f, 3.f, 2.f, 7.f, 4.f, 7.f, 4.f };
        constexpr std::array<float, 11> radii{ 0.22f, 0.22f, 0.16f, 0.09f, 0.075f,
            0.09f, 0.075f, 0.13f, 0.10f, 0.13f, 0.10f };
        std::vector<Ragdoll::BodyDefinition> bodies;
        bodies.reserve(driven.size());
        for (std::size_t i = 0; i < driven.size(); ++i)
        {
            const float length = (stops[i] - starts[i]).length();
            if (!std::isfinite(length) || length < 1.f || length > torsoLength * 4.f)
                return nullptr;
            Ragdoll::BodyDefinition body;
            body.mTransform = capsuleTransform(starts[i], stops[i]);
            body.mRadius = std::max(1.f, i == 0 ? torsoLength * radii[i]
                : std::min(torsoLength * radii[i], length * 0.45f));
            body.mLength = std::max(btScalar(0), length - 2.f * body.mRadius);
            body.mMass = masses[i] * mass / 67.f;
            BonePose& bone = visitor.mBones[driven[i]];
            bone.mBody = static_cast<int>(i);
            // Include original scale and the offset between bone pivot and capsule centre.
            bone.mBoneToBody = bone.mRestWorld * osg::Matrixf::inverse(toOsg(body.mTransform));
            bodies.push_back(body);
        }

        constexpr std::array<std::size_t, 11> parents{ 0, 0, 1, 1, 3, 1, 5, 0, 7, 0, 9 };
        std::vector<Ragdoll::JointDefinition> joints;
        joints.reserve(10);
        for (std::size_t child = 1; child < bodies.size(); ++child)
        {
            Ragdoll::JointDefinition joint;
            joint.mParent = parents[child];
            joint.mChild = child;
            const std::size_t parent = parents[child];
            const osg::Vec3f pivot = starts[child];
            const osg::Vec3f proximal = unit(stops[parent] - starts[parent]);
            const osg::Vec3f distal = unit(stops[child] - starts[child]);
            const bool hinge = child == 4 || child == 6 || child == 8 || child == 10;
            joint.mUseLocalFrames = true;
            if (hinge)
            {
                const bool knee = child == 8 || child == 10;
                // Knees flex behind the thigh, elbows in front of the upper arm.
                // For an arm pointed straight forward use the character's right
                // axis, never a random capsule twist from makeRotate().
                const osg::Vec3f desiredBend = knee ? -forward : forward;
                const osg::Vec3f bendAxis = knee
                    ? unit(-right + proximal * (right * proximal), unit(proximal ^ desiredBend, -right))
                    : unit(proximal ^ desiredBend, right);
                const btTransform parentWorld = hingeFrame(pivot, proximal, bendAxis);
                const btTransform childWorld = hingeFrame(pivot, distal, bendAxis);
                joint.mFrameInParent = bodies[parent].mTransform.inverse() * parentWorld;
                joint.mFrameInChild = bodies[child].mTransform.inverse() * childWorld;
                joint.mType = Ragdoll::JointDefinition::Type::Hinge;
                joint.mAngularLower = btVector3(0, 0, -0.035);
                joint.mAngularUpper = btVector3(0, 0, 2.53);
            }
            else
            {
                // Torso/head frames follow their real skeletal direction. Hip
                // and shoulder reference axes share the torso anatomical frame;
                // their child frames encode each actual limb direction.
                const osg::Vec3f torsoUp = unit(neck - pelvis, osg::Vec3f(0, 0, 1));
                const bool spine = child == 1 || child == 2;
                const osg::Vec3f reference = spine ? proximal : -torsoUp;
                const osg::Vec3f axis = unit(reference ^ forward, right);
                const btTransform parentWorld = hingeFrame(pivot, reference, axis);
                const btTransform childWorld = hingeFrame(pivot, distal, axis);
                joint.mFrameInParent = bodies[parent].mTransform.inverse() * parentWorld;
                joint.mFrameInChild = bodies[child].mTransform.inverse() * childWorld;
                if (spine)
                {
                    const float angle = child == 1 ? 0.3f : 0.55f;
                    joint.mAngularLower = btVector3(-0.35, -angle, -angle);
                    joint.mAngularUpper = btVector3(0.35, angle, angle);
                }
                else if (child == 7 || child == 9)
                {
                    joint.mAngularLower = btVector3(-0.35, -0.6, -1.65);
                    joint.mAngularUpper = btVector3(0.35, 0.6, 0.45);
                }
                else
                {
                    joint.mAngularLower = btVector3(-0.75, -1.75, -2.2);
                    joint.mAngularUpper = btVector3(0.75, 1.75, 1.2);
                }
            }
            joints.push_back(joint);
        }

        auto impl = std::make_unique<Impl>();
        impl->mRoot = &root;
        impl->mSkeleton = &skeleton;
        impl->mSnapshot = std::move(snapshot);
        impl->mSettings = settings;
        impl->mMass = mass;
        impl->mElapsed = std::max(0.f, settings.mDeathAge);
        impl->mTorsoLength = torsoLength;
        impl->mChestRadius = bodies[1].mRadius;
        impl->mPelvisRadius = bodies[0].mRadius;
        impl->mShock = impact.mValid && impact.mDeathElement == Ragdoll::DeathElement::Shock
            && settings.mShockDuration > 0.f;
        impl->mRight = right;
        impl->mForward = forward;
        impl->mCollisionVertices = std::move(vertices);
        Ragdoll::Settings physics;
        if (impact.mValid && impact.mDeathElement == Ragdoll::DeathElement::Poison)
            physics.mBodyMode = Ragdoll::BodyMode::Soft;
        else if (impact.mValid && impact.mDeathElement == Ragdoll::DeathElement::Frost)
            physics.mBodyMode = Ragdoll::BodyMode::Rigid;
        impl->mSimulation = std::make_unique<Ragdoll::Simulation>(
            std::move(bodies), std::move(joints), std::move(colliders), physics);
        impl->mAppearance = std::make_unique<RagdollAppearance>(root,
            impact.mValid ? impact.mDeathElement : Ragdoll::DeathElement::None,
            RagdollAppearance::Settings{ settings.mPoisonTransitionTime, settings.mFireTransitionTime,
                settings.mFrostTransitionTime, settings.mFireEffectDuration });
        if (!impl->mSimulation->isValid())
            return nullptr;
        impl->mBones = std::move(visitor.mBones);
        impl->mBodyMatrices.reserve(driven.size());
        for (std::size_t i = 0; i < driven.size(); ++i)
            impl->mBodyMatrices.push_back(toOsg(impl->mSimulation->getBodyTransform(i)));
        ++sSimulatingCorpses;
        impl->mBudgetSlot = true;

        if (impact.mValid)
        {
            // Convert each approximate throw distance into momentum for a reference
            // body. Equal momentum means a tall or armoured corpse moves less.
            const auto launchVelocity = [&](float metres, const osg::Vec3f& direction) {
                if (!(metres > 0.f) || !std::isfinite(metres) || direction.length2() < 0.0001f)
                    return osg::Vec3f();
                const float distance = metres * Constants::UnitsPerMeter;
                const float lift = std::min(160.f, distance * 0.25f);
                const float fallHeight = std::max(10.f, legLength + torsoLength * 0.5f);
                const float flightTime = (lift + std::sqrt(lift * lift + 2.f * 627.2f * fallHeight)) / 627.2f;
                return unit(direction) * (distance / flightTime) + osg::Vec3f(0, 0, lift);
            };
            osg::Vec3f velocity = (launchVelocity(impact.mWeaponDistance, impact.mWeaponDirection)
                + launchVelocity(impact.mMagicDistance, impact.mMagicDirection))
                * (settings.mBaseMass / mass * settings.mImpulseMultiplier);
            const float speed = velocity.length();
            if (speed > settings.mMaxLaunchSpeed)
                velocity *= settings.mMaxLaunchSpeed / speed;
            impl->mSimulation->applyWholeBodyImpulse(toBullet(velocity * mass));
            if (impact.mCrossbow && settings.mPinEnabled && impact.mWeaponDirection.length2() > 0.0001f)
            {
                impl->mPinDirection = unit(impact.mWeaponDirection);
                impl->mPinStart = toOsg(impl->mSimulation->getBodyTransform(1).getOrigin());
                btVector3 point, normal;
                // Enchantment momentum can carry a bolt victim farther than the
                // weapon-only throw. Probe the configured range; actual near
                // contact below remains mandatory before attaching the body.
                const float reach = settings.mPinMaxDistance * Constants::UnitsPerMeter;
                impl->mPinPending = impl->mSimulation->raycastStatic(toBullet(impl->mPinStart),
                    toBullet(impl->mPinStart + impl->mPinDirection * reach), point, normal)
                    && std::abs(normal.z()) < 0.65 && normal.dot(toBullet(impl->mPinDirection)) < -0.2;
            }
        }
        // Tiny off-centre imbalance for dagger/arrow/no-impact deaths: no launch.
        impl->mSimulation->applyImpulse(1, toBullet(forward * (mass * 0.6f)), toBullet(neck));

        auto result = std::unique_ptr<RagdollController>(new RagdollController(std::move(impl)));
        result->refreshCallbacks();
        result->mImpl->applyPose();
        result->mImpl->updateAppearance();
        return result;
    }

    void RagdollController::refreshCallbacks()
    {
        for (const BonePose& bone : mImpl->mBones)
        {
            bone.mNode->removeUpdateCallback(bone.mCallback);
            bone.mCallback->setNestedCallback(nullptr);
            bone.mNode->addUpdateCallback(bone.mCallback);
        }
    }

    void RagdollController::update(float duration)
    {
        // Surface transitions and short-lived fire continue after a sleeping or
        // frozen physics world has released its active-corpse budget.
        if (duration > 0.f && std::isfinite(duration))
            mImpl->mElapsed += duration;
        if (mImpl->mSimulation && duration > 0.f && std::isfinite(duration))
        {
            const float timeStep = std::min(duration, 0.1f);
            mImpl->mSinceSnapshot += duration;
            mImpl->updateShock(timeStep);
            if (mImpl->mSinceSnapshot >= mImpl->mSettings.mCollisionRefreshInterval)
            {
                auto vertices = mImpl->mSnapshot(getPosition(), mImpl->mSettings.mCollisionRadius);
                if (vertices.empty())
                {
                    // An empty snapshot also represents a failed/over-budget query. Do not keep
                    // simulating against geometry that may have disappeared since the last query.
                    mImpl->freeze();
                    mImpl->applyPose();
                    mImpl->updateAppearance();
                    return;
                }
                if (vertices != mImpl->mCollisionVertices)
                {
                    mImpl->mSimulation->replaceStaticColliders(makeCollisionMesh(vertices));
                    mImpl->mCollisionVertices = std::move(vertices);
                }
                mImpl->mSinceSnapshot = 0.f;
            }
            if (!mImpl->mSimulation->advance(timeStep))
                mImpl->freeze(); // Retain the last valid pose if Bullet rejects a divergent result.
            else
            {
                mImpl->updatePin();
                for (std::size_t i = 0; i < mImpl->mBodyMatrices.size(); ++i)
                    mImpl->mBodyMatrices[i] = toOsg(mImpl->mSimulation->getBodyTransform(i));
                if ((mImpl->mSimulation->isSleeping()
                        && (!mImpl->mShock || mImpl->mElapsed >= mImpl->mSettings.mShockDuration))
                    || mImpl->mElapsed >= mImpl->mSettings.mMaxSimulationTime)
                    mImpl->freeze();
            }
        }
        mImpl->applyPose();
        mImpl->updateAppearance();
    }

    void RagdollController::setSkinParts(std::vector<osg::ref_ptr<osg::Node>> skinParts)
    {
        mImpl->mSkinParts = std::move(skinParts);
    }
    float RagdollController::getAppearanceBlend() const { return mImpl->mAppearance->getBlend(); }
    bool RagdollController::hasFireEffect() const { return mImpl->mAppearance->hasFire(); }

    float RagdollController::getMass() const { return mImpl->mMass; }
    bool RagdollController::isSimulating() const { return bool(mImpl->mSimulation); }
    bool RagdollController::isPinned() const { return mImpl->mPinned; }

    osg::Vec3f RagdollController::getPosition() const
    {
        return mImpl->mBodyMatrices.front().getTrans();
    }
}
