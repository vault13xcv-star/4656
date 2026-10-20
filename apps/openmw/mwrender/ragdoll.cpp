#include "ragdoll.hpp"
#include "ragdollappearance.hpp"

#include <components/esm3/ragdollstate.hpp>
#include <components/misc/constants.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/ragdoll/simulation.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/skeleton.hpp>

#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/ShapeDrawable>
#include <osg/Geode>

#include <BulletCollision/CollisionShapes/btBvhTriangleMeshShape.h>
#include <BulletCollision/CollisionShapes/btTriangleMesh.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>

namespace MWRender
{
    namespace
    {
        // Animation destruction may happen on the resource-unref thread.
        std::atomic<int> sSimulatingCorpses{ 0 };

        btVector3 toBullet(const osg::Vec3f& v)
        {
            return { v.x(), v.y(), v.z() };
        }
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

        std::array<float, 7> packTransform(const btTransform& transform)
        {
            const auto p = transform.getOrigin();
            const auto q = transform.getRotation();
            return { float(p.x()), float(p.y()), float(p.z()), float(q.x()), float(q.y()), float(q.z()), float(q.w()) };
        }
        btTransform unpackTransform(const std::array<float, 7>& value)
        {
            const btQuaternion rotation(value[3], value[4], value[5], value[6]);
            if (!std::isfinite(rotation.length2()) || std::abs(rotation.length2() - 1) > 0.01)
                throw std::invalid_argument("Invalid saved ragdoll rotation");
            return btTransform(rotation.normalized(), btVector3(value[0], value[1], value[2]));
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
            return btTransform(
                btQuaternion(rotation.x(), rotation.y(), rotation.z(), rotation.w()), toBullet((start + end) * 0.5f));
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
                mesh->mTriangles.addTriangle(
                    toBullet(vertices[i]), toBullet(vertices[i + 1]), toBullet(vertices[i + 2]));
            mesh->mShape = std::make_unique<btBvhTriangleMeshShape>(&mesh->mTriangles, true);
            Ragdoll::StaticCollider collider;
            collider.mShape = std::shared_ptr<btCollisionShape>(mesh, mesh->mShape.get());
            collider.mTransform.setIdentity();
            return { std::move(collider) };
        }
    }

    struct RagdollController::Impl
    {
        // Keep weak leases only. Temporarily locking a lease during eviction
        // prevents destruction by the resource-unref thread while it is frozen.
        inline static std::mutex sRegistryMutex;
        inline static std::vector<std::weak_ptr<Impl>> sRegistry;
        inline static std::atomic<std::uint64_t> sNextActivation{ 0 };
        std::uint64_t mActivation = 0;
        osg::ref_ptr<osg::Group> mRoot;
        osg::ref_ptr<SceneUtil::Skeleton> mSkeleton;
        CollisionSnapshot mSnapshot;
        Settings mSettings;
        std::vector<BonePose> mBones;
        std::unique_ptr<Ragdoll::Simulation> mSimulation;
        std::unique_ptr<RagdollAppearance> mAppearance;
        RagdollAppearance::SkinParts mSkinParts;
        std::vector<osg::Matrixf> mBodyMatrices;
        std::vector<Ragdoll::BodyDefinition> mDefinitions;
        std::vector<Ragdoll::JointDefinition> mJoints;
        Ragdoll::BodyMode mMode = Ragdoll::BodyMode::Articulated;
        std::vector<osg::Vec3f> mCollisionVertices;
        float mElapsed = 0.f;
        float mSimulationAge = 0.f;
        float mConfiguredSimulationTime = 10.f;
        float mSinceSnapshot = 0.f;
        bool mBudgetSlot = false;
        float mMass = 70.f;
        float mBodyBaseMass = 70.f;
        bool mFloating = false;
        float mTorsoLength = 40.f;
        float mChestRadius = 8.f;
        float mPelvisRadius = 8.f;
        Ragdoll::DeathElement mAppearanceElement = Ragdoll::DeathElement::None;
        bool mShock = false;
        bool mPinPending = false;
        bool mPinned = false;
        osg::Vec3f mPinDirection;
        osg::Vec3f mPinStart;
        std::size_t mPinBody = 1;
        bool mPinSpecific = false;
        osg::Vec3f mPinPoint;
        osg::Vec3f mRight;
        osg::Vec3f mForward;
        Ragdoll::Wounds mWounds;
        float mSinceBlood = .25f;
        struct WoundVisual
        {
            osg::ref_ptr<osg::MatrixTransform> mNode;
            std::size_t mBody;
            osg::Matrixf mLocal;
        };
        std::vector<WoundVisual> mWoundVisuals;

        void addWoundVisual(osg::Shape* shape, std::size_t body, const osg::Matrixf& local)
        {
            osg::ref_ptr<osg::MatrixTransform> node = new osg::MatrixTransform;
            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            osg::ref_ptr<osg::ShapeDrawable> drawable = new osg::ShapeDrawable(shape);
            drawable->setColor(osg::Vec4(.35f, .015f, .02f, 1));
            geode->addDrawable(drawable);
            geode->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
            node->addChild(geode);
            mRoot->addChild(node);
            mWoundVisuals.push_back({ node, body, local });
        }

        void initWounds()
        {
            for (std::size_t i = 0; i < mJoints.size(); ++i)
            {
                const auto& joint = mJoints[i];
                if (!mWounds.cuts(joint.mChild))
                    continue;
                if (mSimulation)
                    mSimulation->detachJoint(i);
                const float radius = mWounds.mBurstHead && joint.mChild == 2
                    ? mChestRadius * .45f : mDefinitions[joint.mChild].mRadius * .9f;
                const auto disc = osg::Matrixf::rotate(1.5707963f, osg::Vec3f(0, 1, 0));
                addWoundVisual(new osg::Cylinder(osg::Vec3(), radius, .8f), joint.mParent,
                    disc * toOsg(joint.mFrameInParent));
                if (!(mWounds.mBurstHead && joint.mChild == 2))
                    addWoundVisual(new osg::Cylinder(osg::Vec3(), radius, .8f), joint.mChild,
                        disc * toOsg(joint.mFrameInChild));
            }
            if (mWounds.mBurstHead)
                for (std::size_t part = 15; part < mDefinitions.size(); ++part)
                    addWoundVisual(new osg::Box(osg::Vec3(), mDefinitions[part].mRadius * 1.5f), part,
                        osg::Matrixf::identity());
        }

        void updateWounds(float duration)
        {
            const auto inverseRoot = osg::Matrixf::inverse(rootWorldMatrix(*mRoot));
            for (const auto& visual : mWoundVisuals)
                visual.mNode->setMatrix(visual.mLocal * mBodyMatrices[visual.mBody] * inverseRoot);
            mSinceBlood += std::max(0.f, duration);
            if (!mSettings.mBloodEffect || mElapsed >= mSettings.mBleedDuration || mSinceBlood < .25f)
                return;
            mSinceBlood = 0;
            for (const auto& joint : mJoints)
            {
                if (!mWounds.cuts(joint.mChild))
                    continue;
                mSettings.mBloodEffect((toOsg(joint.mFrameInParent) * mBodyMatrices[joint.mParent]).getTrans());
                if (!(mWounds.mBurstHead && joint.mChild == 2))
                    mSettings.mBloodEffect((toOsg(joint.mFrameInChild) * mBodyMatrices[joint.mChild]).getTrans());
            }
        }

        void updateAppearance()
        {
            std::vector<osg::Vec3f> positions(mBodyMatrices.size());
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
            const float envelope = std::min(1.f, (mSettings.mShockDuration - mElapsed) / 0.5f);
            const float strength
                = mMass * mTorsoLength * mTorsoLength * 1.6f * mSettings.mShockStrength * envelope * timeStep;
            for (std::size_t joint = 0; joint < mJoints.size(); ++joint)
            {
                const float pulse = std::sin(mElapsed * 18.f + joint * 1.3f);
                const osg::Vec3f axis = joint >= 6 || joint < 2 ? mRight : mForward;
                mSimulation->applyJointTorqueImpulse(joint, toBullet(axis * (strength * pulse)));
            }
        }

        void updatePin()
        {
            if (!mPinPending || mPinned)
                return;
            const osg::Vec3f partPosition = toOsg(mSimulation->getBodyTransform(mPinBody).getOrigin());
            if (mElapsed > 2.f
                || (partPosition - mPinStart).length() > mSettings.mPinMaxDistance * Constants::UnitsPerMeter)
            {
                mPinPending = false;
                return;
            }
            // Legacy/scripted contacts without a point retain the torso fallback.
            // Native contacts may pin only their explicitly struck segment.
            const std::array<std::size_t, 2> candidates{ mPinBody, mPinSpecific ? mPinBody : 0u };
            for (const std::size_t body : candidates)
            {
                btVector3 point, normal;
                if (mSimulation->sweepPartStatic(
                        body, toBullet(mPinDirection * mSettings.mPinContactDistance), point, normal)
                    && std::abs(normal.z()) < 0.65 && normal.dot(toBullet(mPinDirection)) < -0.2)
                {
                    // Attach the struck segment only after its capsule reaches
                    // the surface. Another limb cannot create an invisible tether.
                    mPinned = mSimulation->pinBody(body, point);
                    if (mPinned)
                    {
                        mPinBody = body;
                        mPinPoint = toOsg(point);
                        mPinPending = false;
                        return;
                    }
                }
            }
        }

        ~Impl()
        {
            for (const auto& visual : mWoundVisuals)
                mRoot->removeChild(visual.mNode);
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

        void enforceBudget(int limit)
        {
            std::vector<std::shared_ptr<Impl>> active;
            {
                std::lock_guard lock(sRegistryMutex);
                std::erase_if(sRegistry, [](const auto& value) { return value.expired(); });
                for (const auto& value : sRegistry)
                    if (auto corpse = value.lock(); corpse && corpse->mBudgetSlot)
                        active.push_back(std::move(corpse));
            }
            const auto priority = [&](const std::shared_ptr<Impl>& corpse) {
                return std::tuple(corpse->mSimulation->isSleeping(),
                    (corpse->mBodyMatrices.front().getTrans() - mSettings.mObserverPosition).length2(),
                    std::numeric_limits<std::uint64_t>::max() - corpse->mActivation);
            };
            while (sSimulatingCorpses.load() > limit)
            {
                auto victim = active.end();
                for (auto it = active.begin(); it != active.end(); ++it)
                    if ((*it)->mBudgetSlot && (victim == active.end() || priority(*it) > priority(*victim)))
                        victim = it;
                if (victim == active.end())
                    break;
                (*victim)->freeze();
            }
        }

        void acquireBudget(const std::shared_ptr<Impl>& self)
        {
            enforceBudget(std::clamp(mSettings.mMaxActive, 1, 200) - 1);
            mActivation = ++sNextActivation;
            mBudgetSlot = true;
            ++sSimulatingCorpses;
            std::lock_guard lock(sRegistryMutex);
            sRegistry.push_back(self);
        }

        void freeze()
        {
            // Keep the final matrices and pose overrides, but release physics world and mesh memory.
            mSimulation.reset();
            mFloating = false;
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

    RagdollController::RagdollController(std::shared_ptr<Impl> impl)
        : mImpl(std::move(impl))
    {
    }

    RagdollController::~RagdollController() = default;

    std::unique_ptr<RagdollController> RagdollController::create(osg::Group& root, SceneUtil::Skeleton& skeleton,
        CollisionSnapshot snapshot, const Settings& settings, const Ragdoll::Impact& impact)
    {
        if (&root != &skeleton || settings.mMaxActive < 1)
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
        const std::array<int, 15> driven = { find({ "bip01 pelvis" }),
            find({ "bip01 spine2", "bip01 spine1", "bip01 spine" }), find({ "bip01 head" }),
            find({ "bip01 l upperarm" }), find({ "bip01 l forearm" }), find({ "bip01 r upperarm" }),
            find({ "bip01 r forearm" }), find({ "bip01 l thigh" }), find({ "bip01 l calf" }), find({ "bip01 r thigh" }),
            find({ "bip01 r calf" }), find({ "bip01 l hand" }), find({ "bip01 r hand" }), find({ "bip01 l foot" }),
            find({ "bip01 r foot" }) };
        const std::array<int, 5> ends = { find({ "bip01 neck" }), find({ "bip01 l hand" }), find({ "bip01 r hand" }),
            find({ "bip01 l foot" }), find({ "bip01 r foot" }) };
        const bool humanoid = !std::ranges::any_of(driven, [](int i) { return i < 0; })
            && !std::ranges::any_of(ends, [](int i) { return i < 0; });
        float torsoLength = 40, legLength = 40, mass = settings.mBaseMass;
        std::vector<Ragdoll::BodyDefinition> bodies;
        std::vector<Ragdoll::JointDefinition> joints;
        const osg::Matrixf rootMatrix = rootWorldMatrix(root);
        const osg::Vec3f forward = unit(osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), rootMatrix));
        const osg::Vec3f right = unit(osg::Matrixf::transform3x3(osg::Vec3f(1, 0, 0), rootMatrix));
        if (humanoid)
        {
            const auto position = [&](int index) { return osg::Vec3f(visitor.mBones[index].mRestWorld.getTrans()); };
            const osg::Vec3f pelvis = position(driven[0]);
            const osg::Vec3f neck = position(ends[0]);
            torsoLength = (neck - pelvis).length();
            // Degenerate transforms, unusual scale or an uninitialised skeleton fall back to native death.
            if (!std::isfinite(torsoLength) || torsoLength < 8.f || torsoLength > 512.f)
                return nullptr;

            std::array<osg::Vec3f, 15> starts;
            std::array<osg::Vec3f, 15> stops;
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
            stops[11] = starts[11] + unit(stops[4] - starts[4]) * (torsoLength * 0.14f);
            stops[12] = starts[12] + unit(stops[6] - starts[6]) * (torsoLength * 0.14f);
            stops[13] = starts[13] + forward * (torsoLength * 0.2f);
            stops[14] = starts[14] + forward * (torsoLength * 0.2f);

            legLength = ((stops[7] - starts[7]).length() + (stops[8] - starts[8]).length()
                            + (stops[9] - starts[9]).length() + (stops[10] - starts[10]).length())
                * 0.5f;
            const float stature = legLength + torsoLength + (stops[2] - starts[2]).length();
            const float heightRatio = std::clamp(stature / settings.mReferenceHeight, 0.25f, 4.f);
            const auto scale = rootMatrix.getScale();
            const float volumeCorrection = std::abs(scale.z()) > 1e-6
                ? static_cast<float>(std::abs(scale.x() * scale.y()) / (scale.z() * scale.z()))
                : 1.f;
            mass = std::clamp(settings.mBaseMass * heightRatio * heightRatio * heightRatio * volumeCorrection
                    + std::max(0.f, settings.mArmorMass),
                5.f, 5000.f);
            constexpr std::array<float, 15> masses{ 10.f, 20.f, 5.f, 3.f, 1.6f, 3.f, 1.6f, 7.f, 3.5f, 7.f, 3.5f, .4f,
                .4f, .5f, .5f };
            constexpr std::array<float, 15> radii{ 0.22f, 0.22f, 0.16f, 0.09f, 0.075f, 0.09f, 0.075f, 0.13f, 0.10f,
                0.13f, 0.10f, .07f, .07f, .09f, .09f };
            bodies.reserve(driven.size());
            for (std::size_t i = 0; i < driven.size(); ++i)
            {
                const float length = (stops[i] - starts[i]).length();
                if (!std::isfinite(length) || length < 1.f || length > torsoLength * 4.f)
                    return nullptr;
                Ragdoll::BodyDefinition body;
                body.mTransform = capsuleTransform(starts[i], stops[i]);
                body.mRadius
                    = std::max(1.f, i == 0 ? torsoLength * radii[i] : std::min(torsoLength * radii[i], length * 0.45f));
                body.mLength = std::max(btScalar(0), length - 2.f * body.mRadius);
                body.mMass = masses[i] * mass / 67.f;
                BonePose& bone = visitor.mBones[driven[i]];
                bone.mBody = static_cast<int>(i);
                // Include original scale and the offset between bone pivot and capsule centre.
                bone.mBoneToBody = bone.mRestWorld * osg::Matrixf::inverse(toOsg(body.mTransform));
                bodies.push_back(body);
            }

            constexpr std::array<std::size_t, 15> parents{ 0, 0, 1, 1, 3, 1, 5, 0, 7, 0, 9, 4, 6, 8, 10 };
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
                    else if (child >= 11)
                    {
                        joint.mAngularLower = btVector3(-.25, -.5, -.6);
                        joint.mAngularUpper = btVector3(.25, .5, .6);
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
        }
        else
        {
            if (!settings.mAllowGenericSkeleton)
                return nullptr;
            std::vector<std::size_t> selected;
            std::map<osg::Node*, std::size_t> bodyForNode;
            osg::Vec3f minimum(1e8, 1e8, 1e8), maximum(-1e8, -1e8, -1e8);
            for (std::size_t bone = 0; bone < visitor.mBones.size() && selected.size() < 64; ++bone)
            {
                const auto name = Misc::StringUtils::lowerCase(visitor.mBones[bone].mNode->getName());
                if (name == "bip01" || name == "root" || name == "rootbone" || name.empty()
                    || name.find("weapon") != name.npos || name.find("shield") != name.npos)
                    continue;
                bool anatomical = false;
                for (const auto key :
                    { "bip", "bone", "joint", "head", "spine", "neck", "tail", "wing", "leg", "arm", "pelvis" })
                    anatomical |= name.find(key) != name.npos;
                if (!anatomical)
                    continue;
                const osg::Vec3f point(visitor.mBones[bone].mRestWorld.getTrans());
                if (!std::isfinite(point.length2()))
                    return nullptr;
                for (int axis = 0; axis < 3; ++axis)
                {
                    minimum[axis] = std::min(minimum[axis], point[axis]);
                    maximum[axis] = std::max(maximum[axis], point[axis]);
                }
                bodyForNode[visitor.mBones[bone].mNode] = selected.size();
                selected.push_back(bone);
            }
            if (selected.size() < 2)
                return nullptr;
            const auto span = maximum - minimum;
            const float size = std::max({ span.x(), span.y(), span.z() });
            if (!std::isfinite(size) || size < 2 || size > 4096)
                return nullptr;
            const float ratio = std::clamp(size / settings.mReferenceHeight, .1f, 10.f);
            mass = std::clamp(
                settings.mBaseMass * ratio * ratio * ratio + std::max(0.f, settings.mArmorMass), .25f, 5000.f);
            torsoLength = std::max(4.f, size * .4f);
            legLength = size * .3f;
            std::vector<int> parents(selected.size(), -1);
            for (std::size_t part = 0; part < selected.size(); ++part)
            {
                const auto& path = visitor.mBones[selected[part]].mParents;
                for (auto parent = path.rbegin(); parent != path.rend(); ++parent)
                    if (auto found = bodyForNode.find(*parent); found != bodyForNode.end())
                    {
                        parents[part] = found->second;
                        break;
                    }
            }
            float volume = 0;
            for (std::size_t part = 0; part < selected.size(); ++part)
            {
                auto& bone = visitor.mBones[selected[part]];
                const osg::Vec3f start = bone.mRestWorld.getTrans();
                osg::Vec3f end = start + forward * std::max(1.f, size * .04f);
                float longest = 0;
                for (std::size_t child = 0; child < selected.size(); ++child)
                    if (parents[child] == static_cast<int>(part))
                    {
                        const osg::Vec3f point = visitor.mBones[selected[child]].mRestWorld.getTrans();
                        if ((point - start).length2() > longest)
                        {
                            longest = (point - start).length2();
                            end = point;
                        }
                    }
                const float length = std::max(1.f, (end - start).length());
                Ragdoll::BodyDefinition body;
                body.mTransform = capsuleTransform(start, end);
                body.mRadius = std::clamp(length * .18f, .4f, std::max(.4f, size * .12f));
                body.mLength = std::max(0.f, length - 2.f * float(body.mRadius));
                body.mMass = body.mRadius * body.mRadius * (body.mLength + 2.f * body.mRadius);
                volume += body.mMass;
                bone.mBody = part;
                bone.mBoneToBody = bone.mRestWorld * osg::Matrixf::inverse(toOsg(body.mTransform));
                bodies.push_back(body);
            }
            const float minimumMass = std::min(.002f, mass / (2.f * bodies.size()));
            const float remainingMass = mass - minimumMass * bodies.size();
            for (auto& body : bodies)
                body.mMass = minimumMass + body.mMass * (remainingMass / volume);
            for (std::size_t part = 1; part < selected.size(); ++part)
            {
                const auto parent = parents[part] >= 0 ? std::size_t(parents[part]) : 0;
                Ragdoll::JointDefinition joint;
                joint.mParent = parent;
                joint.mChild = part;
                joint.mUseLocalFrames = true;
                auto frame = btTransform::getIdentity();
                frame.setOrigin(toBullet(visitor.mBones[selected[part]].mRestWorld.getTrans()));
                joint.mFrameInParent = bodies[parent].mTransform.inverse() * frame;
                joint.mFrameInChild = bodies[part].mTransform.inverse() * frame;
                joint.mAngularLower = btVector3(-.6, -.8, -.8);
                joint.mAngularUpper = btVector3(.6, .8, .8);
                joints.push_back(joint);
            }
        }
        auto vertices = snapshot(toOsg(bodies.front().mTransform.getOrigin()), settings.mCollisionRadius);
        auto colliders = makeCollisionMesh(vertices);
        if (colliders.empty())
            return nullptr;

        auto impl = std::make_shared<Impl>();
        impl->mRoot = &root;
        impl->mSkeleton = &skeleton;
        impl->mSnapshot = std::move(snapshot);
        impl->mSettings = settings;
        impl->mConfiguredSimulationTime = settings.mMaxSimulationTime;
        impl->mMass = mass;
        impl->mBodyBaseMass = std::max(.25f, mass - std::max(0.f, settings.mArmorMass));
        impl->mElapsed = std::max(0.f, settings.mDeathAge);
        impl->mSimulationAge = impl->mElapsed;
        impl->mTorsoLength = torsoLength;
        impl->mChestRadius = bodies[1].mRadius;
        impl->mPelvisRadius = bodies[0].mRadius;
        impl->mAppearanceElement = impact.mValid ? impact.mDeathElement : Ragdoll::DeathElement::None;
        impl->mShock
            = impact.mValid && impact.mDeathElement == Ragdoll::DeathElement::Shock && settings.mShockDuration > 0.f;
        impl->mRight = right;
        impl->mForward = forward;
        impl->mCollisionVertices = std::move(vertices);
        if (humanoid && settings.mNpc)
        {
            std::size_t closest = 0;
            float distance = std::numeric_limits<float>::max();
            const osg::Vec3f contact = impact.mHasWeaponContact ? impact.mWeaponPosition : impact.mPosition;
            for (std::size_t part = 0; part < bodies.size(); ++part)
            {
                // Distance to the capsule axis, not its centre: a head contact
                // must not be attributed to the centre of a long upper arm.
                const auto local = bodies[part].mTransform.inverse() * toBullet(contact);
                const auto y = std::clamp(local.y(), -bodies[part].mLength * .5, bodies[part].mLength * .5);
                const float d = std::max(0., double((local - btVector3(0, y, 0)).length() - bodies[part].mRadius));
                if (d < distance)
                {
                    distance = d;
                    closest = part;
                }
            }
            impl->mWounds = Ragdoll::selectWounds(impact, closest, settings.mWoundRoll, settings.mWoundChance);
            if (impl->mWounds.mBurstHead)
            {
                const auto head = bodies[2];
                bodies[2].mMass = .001;
                bodies[2].mRadius = .25;
                bodies[2].mLength = 0;
                for (unsigned i = 0; i < 6; ++i)
                {
                    auto gib = head;
                    gib.mMass = (head.mMass - .001) / 6;
                    gib.mRadius = head.mRadius * .35;
                    gib.mLength = 0;
                    const float angle = i * 6.2831853f / 6;
                    gib.mTransform.getOrigin() += btVector3(std::cos(angle), std::sin(angle), (i % 2) * .5)
                        * head.mRadius * .7;
                    bodies.push_back(gib);
                }
            }
        }
        Ragdoll::Settings physics;
        if (impact.mValid && impact.mDeathElement == Ragdoll::DeathElement::Poison)
            physics.mBodyMode = Ragdoll::BodyMode::Soft;
        else if (impact.mValid && impact.mDeathElement == Ragdoll::DeathElement::Frost)
            physics.mBodyMode = Ragdoll::BodyMode::Rigid;
        if (impl->mWounds.mCuts)
            physics.mBodyMode = Ragdoll::BodyMode::Articulated;
        impl->mMode = physics.mBodyMode;
        impl->mDefinitions = bodies;
        impl->mJoints = joints;
        impl->mSimulation = std::make_unique<Ragdoll::Simulation>(
            std::move(bodies), std::move(joints), std::move(colliders), physics);
        impl->mAppearance = std::make_unique<RagdollAppearance>(root,
            impact.mValid ? impact.mDeathElement : Ragdoll::DeathElement::None,
            RagdollAppearance::Settings{ settings.mPoisonTransitionTime, settings.mFireTransitionTime,
                settings.mFrostTransitionTime, settings.mFireEffectDuration, settings.mPoisonEffectDuration,
                settings.mShockDuration });
        if (!impl->mSimulation->isValid())
            return nullptr;
        impl->mBones = std::move(visitor.mBones);
        impl->mBodyMatrices.reserve(impl->mDefinitions.size());
        for (std::size_t i = 0; i < impl->mDefinitions.size(); ++i)
            impl->mBodyMatrices.push_back(toOsg(impl->mSimulation->getBodyTransform(i)));
        impl->initWounds();
        for (std::size_t i = 0; i < impl->mJoints.size(); ++i)
        {
            const auto& joint = impl->mJoints[i];
            if (impl->mWounds.cuts(joint.mChild))
            {
                const auto outward = unit(toOsg(impl->mDefinitions[joint.mChild].mTransform.getOrigin())
                    - toOsg(impl->mDefinitions[joint.mParent].mTransform.getOrigin()), right);
                impl->mSimulation->applyImpulse(joint.mChild,
                    toBullet((outward * 90.f + osg::Vec3f(0, 0, 100)) * float(impl->mDefinitions[joint.mChild].mMass)),
                    impl->mDefinitions[joint.mChild].mTransform.getOrigin());
            }
        }
        for (std::size_t part = 15; impl->mWounds.mBurstHead && part < impl->mDefinitions.size(); ++part)
        {
            const float angle = (part - 15) * 6.2831853f / 6;
            impl->mSimulation->setBodyVelocity(part, btVector3(std::cos(angle) * 130, std::sin(angle) * 130, 140),
                btVector3(8, -5, 6));
        }
        impl->acquireBudget(impl);
        if (std::isfinite(settings.mInitialVelocity.length2()) && settings.mInitialVelocity.length2() > 0)
            impl->mSimulation->applyWholeBodyImpulse(toBullet(settings.mInitialVelocity * mass));

        bool launched = false;
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
            osg::Vec3f weaponVelocity
                = launchVelocity(impact.mWeaponDistance, impact.mWeaponDirection) * impact.mWeaponMultiplier;
            osg::Vec3f magicVelocity = impact.mMagicAttraction
                ? osg::Vec3f()
                : launchVelocity(impact.mMagicDistance, impact.mMagicDirection);
            osg::Vec3f velocity
                = (weaponVelocity
                      + (impact.mMagicAttraction ? osg::Vec3f()
                                                 : launchVelocity(impact.mMagicDistance, impact.mMagicDirection)))
                * (settings.mImpulseReferenceMass / mass * settings.mImpulseMultiplier);
            const float speed = velocity.length();
            if (speed > settings.mMaxLaunchSpeed)
            {
                const float factor = settings.mMaxLaunchSpeed / speed;
                velocity *= factor;
                weaponVelocity *= factor;
                magicVelocity *= factor;
            }
            launched = velocity.length2() > 0.01f;
            if (impact.mHasWeaponContact && weaponVelocity.length2() > 0.f)
            {
                std::size_t closest = 0;
                float distance = std::numeric_limits<float>::max();
                for (std::size_t part = 0; part < impl->mSimulation->getNumBodies(); ++part)
                {
                    const float d
                        = (toOsg(impl->mSimulation->getBodyTransform(part).getOrigin()) - impact.mWeaponPosition)
                              .length2();
                    if (d < distance)
                    {
                        distance = d;
                        closest = part;
                    }
                }
                impl->mSimulation->applyImpulse(closest,
                    toBullet(weaponVelocity * (settings.mImpulseReferenceMass * settings.mImpulseMultiplier)),
                    toBullet(impact.mWeaponPosition));
                impl->mPinBody = closest;
                impl->mPinSpecific = true;
                impl->mSimulation->applyWholeBodyImpulse(
                    toBullet(magicVelocity * (settings.mImpulseReferenceMass * settings.mImpulseMultiplier)));
            }
            else
                impl->mSimulation->applyWholeBodyImpulse(toBullet(velocity * mass));
            if (impact.mMagicAttraction && impact.mMagicDistance > 0.f)
                impl->mSimulation->attract(toBullet(impact.mMagicOrigin),
                    impact.mMagicDistance * Constants::UnitsPerMeter, settings.mImpulseReferenceMass);
            if (impact.mCrossbow && !impl->mWounds.mBurstHead && settings.mPinEnabled
                && impact.mWeaponDirection.length2() > 0.0001f)
            {
                impl->mPinDirection = unit(impact.mWeaponDirection);
                impl->mPinStart = toOsg(impl->mSimulation->getBodyTransform(impl->mPinBody).getOrigin());
                btVector3 point, normal;
                // Enchantment momentum can carry a bolt victim farther than the
                // weapon-only throw. Probe the configured range; actual near
                // contact below remains mandatory before attaching the body.
                const float reach = settings.mPinMaxDistance * Constants::UnitsPerMeter;
                impl->mPinPending = impl->mSimulation->sweepPartStatic(
                                        impl->mPinBody, toBullet(impl->mPinDirection * reach), point, normal)
                    && std::abs(normal.z()) < 0.65 && normal.dot(toBullet(impl->mPinDirection)) < -0.2;
            }
        }
        // Tiny off-centre imbalance for dagger/arrow/no-impact deaths: no launch.
        const float imbalance = physics.mBodyMode == Ragdoll::BodyMode::Rigid ? 18.f : 0.6f;
        if (!launched)
            impl->mSimulation->applyImpulse(1, toBullet(forward * (mass * imbalance)),
                impl->mDefinitions[1].mTransform
                    * btVector3(0, impl->mDefinitions[1].mLength * .5 + impl->mDefinitions[1].mRadius, 0));

        auto result = std::unique_ptr<RagdollController>(new RagdollController(std::move(impl)));
        result->refreshCallbacks();
        result->mImpl->applyPose();
        result->mImpl->updateAppearance();
        result->mImpl->updateWounds(0);
        return result;
    }

    void RagdollController::writeState(ESM::RagdollState& state) const
    {
        auto& impl = *mImpl;
        const float effectsEnd = std::max({ impl.mSettings.mFireEffectDuration, impl.mSettings.mPoisonEffectDuration,
            impl.mSettings.mShockDuration });
        if (!impl.mSimulation && state.mPresent && state.mTimeLeft == 0 && state.mElapsed >= effectsEnd
            && state.mMass == impl.mMass && state.mFloating == impl.mFloating
            && state.mMode == static_cast<int>(impl.mMode) && state.mFireDuration == impl.mSettings.mFireEffectDuration
            && state.mPoisonDuration == impl.mSettings.mPoisonEffectDuration
            && state.mShockDuration == impl.mSettings.mShockDuration
            && state.mBleedDuration == impl.mSettings.mBleedDuration)
            return;
        state.mElement = static_cast<int>(impl.mAppearanceElement);
        state.mMode = static_cast<int>(impl.mMode);
        state.mElapsed = impl.mElapsed;
        state.mTimeLeft
            = impl.mSimulation ? std::max(0.f, impl.mSettings.mMaxSimulationTime - impl.mSimulationAge) : 0.f;
        state.mMass = impl.mMass;
        state.mTorsoLength = impl.mTorsoLength;
        state.mArmorMass = impl.mSettings.mArmorMass;
        state.mFloating = impl.mFloating;
        state.mFireDuration = impl.mSettings.mFireEffectDuration;
        state.mPoisonDuration = impl.mSettings.mPoisonEffectDuration;
        state.mShockDuration = impl.mSettings.mShockDuration;
        state.mCuts = impl.mWounds.mCuts;
        state.mBurstHead = impl.mWounds.mBurstHead;
        state.mBleedDuration = impl.mSettings.mBleedDuration;
        state.mPinnedBody = impl.mPinned ? static_cast<int>(impl.mPinBody) : -1;
        state.mPinPoint = { impl.mPinPoint.x(), impl.mPinPoint.y(), impl.mPinPoint.z() };
        if (!state.mPresent)
        {
            state.mJoints.clear();
            state.mBones.clear();
            for (const auto& joint : impl.mJoints)
            {
                ESM::RagdollJointState saved;
                saved.mParent = joint.mParent;
                saved.mChild = joint.mChild;
                saved.mType = static_cast<int>(joint.mType);
                saved.mParentFrame = packTransform(joint.mFrameInParent);
                saved.mChildFrame = packTransform(joint.mFrameInChild);
                for (int axis = 0; axis < 3; ++axis)
                {
                    saved.mLower[axis] = joint.mAngularLower[axis];
                    saved.mUpper[axis] = joint.mAngularUpper[axis];
                }
                state.mJoints.push_back(std::move(saved));
            }
            std::set<std::string> savedNames;
            for (const auto& bone : impl.mBones)
            {
                if (bone.mNode->getName().empty())
                    continue;
                if (!savedNames.insert(Misc::StringUtils::lowerCase(bone.mNode->getName())).second)
                    continue;
                if (bone.mBody < 0 && state.mBones.size() >= 960)
                    continue;
                ESM::RagdollBoneState saved;
                saved.mName = bone.mNode->getName();
                saved.mBody = bone.mBody;
                std::copy_n(bone.mRestLocal.ptr(), 16, saved.mRest.begin());
                std::copy_n(bone.mBoneToBody.ptr(), 16, saved.mBoneToBody.begin());
                state.mBones.push_back(std::move(saved));
            }
        }
        state.mBodies.resize(impl.mDefinitions.size());
        for (std::size_t part = 0; part < impl.mDefinitions.size(); ++part)
        {
            const auto& definition = impl.mDefinitions[part];
            auto& saved = state.mBodies[part];
            const auto matrix = impl.mBodyMatrices[part];
            const auto q = matrix.getRotate();
            const auto position = matrix.getTrans();
            saved.mTransform = { float(position.x()), float(position.y()), float(position.z()), float(q.x()),
                float(q.y()), float(q.z()), float(q.w()) };
            saved.mRadius = definition.mRadius;
            saved.mLength = definition.mLength;
            saved.mMass = definition.mMass;
            saved.mLinearVelocity = {};
            saved.mAngularVelocity = {};
            if (impl.mSimulation)
                for (int axis = 0; axis < 3; ++axis)
                {
                    saved.mLinearVelocity[axis] = impl.mSimulation->getBodyLinearVelocity(part)[axis];
                    saved.mAngularVelocity[axis] = impl.mSimulation->getBodyAngularVelocity(part)[axis];
                }
        }
        state.mAttractionLeft = 0;
        if (impl.mSimulation)
        {
            const auto attraction = impl.mSimulation->attractionState();
            if (attraction.mTimeLeft > 0)
            {
                state.mAttractionTarget
                    = { float(attraction.mTarget.x()), float(attraction.mTarget.y()), float(attraction.mTarget.z()) };
                state.mAttractionDistance = attraction.mDistance;
                state.mAttractionDuration = attraction.mDuration;
                state.mAttractionLeft = attraction.mTimeLeft;
            }
        }
        state.mPresent = true;
    }

    std::unique_ptr<RagdollController> RagdollController::restore(osg::Group& root, SceneUtil::Skeleton& skeleton,
        CollisionSnapshot snapshot, const Settings& settings, const ESM::RagdollState& saved)
    {
        if (!saved.mPresent || saved.mBodies.empty() || saved.mBones.empty() || &root != &skeleton)
            return nullptr;
        auto impl = std::make_shared<Impl>();
        impl->mRoot = &root;
        impl->mSkeleton = &skeleton;
        impl->mSnapshot = std::move(snapshot);
        impl->mSettings = settings;
        impl->mConfiguredSimulationTime = settings.mMaxSimulationTime;
        impl->mSettings.mFireEffectDuration = saved.mFireDuration;
        impl->mSettings.mPoisonEffectDuration = saved.mPoisonDuration;
        impl->mSettings.mShockDuration = saved.mShockDuration;
        impl->mSettings.mBleedDuration = saved.mBleedDuration;
        impl->mWounds = { saved.mCuts, saved.mBurstHead };
        impl->mElapsed = saved.mElapsed;
        impl->mMass = saved.mMass;
        impl->mTorsoLength = saved.mTorsoLength;
        impl->mBodyBaseMass = std::max(.25f, saved.mMass - saved.mArmorMass);
        impl->mSettings.mArmorMass = saved.mArmorMass;
        impl->mFloating = saved.mFloating;
        impl->mMode = static_cast<Ragdoll::BodyMode>(saved.mMode);
        impl->mAppearanceElement = static_cast<Ragdoll::DeathElement>(saved.mElement);
        impl->mShock = impl->mAppearanceElement == Ragdoll::DeathElement::Shock;
        const auto rootMatrix = rootWorldMatrix(root);
        impl->mForward = unit(osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), rootMatrix));
        impl->mRight = unit(osg::Matrixf::transform3x3(osg::Vec3f(1, 0, 0), rootMatrix));
        CollectBones visitor(rootMatrix);
        root.accept(visitor);
        for (const auto& bone : saved.mBones)
        {
            const auto found = visitor.mNames.find(Misc::StringUtils::lowerCase(bone.mName));
            if (found == visitor.mNames.end())
            {
                if (bone.mBody >= 0)
                    return nullptr;
                continue;
            }
            auto& pose = visitor.mBones[found->second];
            pose.mBody = bone.mBody;
            pose.mRestLocal = osg::Matrixf(bone.mRest.data());
            pose.mBoneToBody = osg::Matrixf(bone.mBoneToBody.data());
        }
        impl->mBones = std::move(visitor.mBones);
        for (const auto& body : saved.mBodies)
        {
            Ragdoll::BodyDefinition definition;
            definition.mTransform = unpackTransform(body.mTransform);
            definition.mRadius = body.mRadius;
            definition.mLength = body.mLength;
            definition.mMass = body.mMass;
            impl->mDefinitions.push_back(definition);
            impl->mBodyMatrices.push_back(toOsg(definition.mTransform));
        }
        for (const auto& joint : saved.mJoints)
        {
            Ragdoll::JointDefinition definition;
            definition.mParent = joint.mParent;
            definition.mChild = joint.mChild;
            definition.mType = static_cast<Ragdoll::JointDefinition::Type>(joint.mType);
            definition.mUseLocalFrames = true;
            definition.mFrameInParent = unpackTransform(joint.mParentFrame);
            definition.mFrameInChild = unpackTransform(joint.mChildFrame);
            for (int axis = 0; axis < 3; ++axis)
            {
                definition.mAngularLower[axis] = joint.mLower[axis];
                definition.mAngularUpper[axis] = joint.mUpper[axis];
            }
            impl->mJoints.push_back(definition);
        }
        if (saved.mTimeLeft > 0 || saved.mFloating)
        {
            auto vertices = impl->mSnapshot(impl->mBodyMatrices.front().getTrans(), settings.mCollisionRadius);
            auto colliders = makeCollisionMesh(vertices);
            if (colliders.empty())
                return nullptr;
            impl->mCollisionVertices = std::move(vertices);
            Ragdoll::Settings physics;
            physics.mBodyMode = impl->mMode;
            impl->mSimulation = std::make_unique<Ragdoll::Simulation>(
                impl->mDefinitions, impl->mJoints, std::move(colliders), physics);
            for (std::size_t part = 0; part < saved.mBodies.size(); ++part)
            {
                const auto& body = saved.mBodies[part];
                impl->mSimulation->setBodyVelocity(part,
                    btVector3(body.mLinearVelocity[0], body.mLinearVelocity[1], body.mLinearVelocity[2]),
                    btVector3(body.mAngularVelocity[0], body.mAngularVelocity[1], body.mAngularVelocity[2]));
            }
            if (saved.mAttractionLeft > 0)
                impl->mSimulation->restoreAttraction(
                    { btVector3(saved.mAttractionTarget[0], saved.mAttractionTarget[1], saved.mAttractionTarget[2]),
                        saved.mAttractionDistance, settings.mImpulseReferenceMass, saved.mAttractionDuration,
                        saved.mAttractionLeft });
            impl->mSettings.mMaxSimulationTime = saved.mTimeLeft;
            impl->acquireBudget(impl);
        }
        impl->mPinned = saved.mPinnedBody >= 0;
        if (impl->mPinned)
        {
            impl->mPinBody = saved.mPinnedBody;
            impl->mPinPoint = osg::Vec3f(saved.mPinPoint[0], saved.mPinPoint[1], saved.mPinPoint[2]);
            if (impl->mSimulation)
                impl->mPinned = impl->mSimulation->pinBody(impl->mPinBody, toBullet(impl->mPinPoint));
        }
        impl->mAppearance = std::make_unique<RagdollAppearance>(root, impl->mAppearanceElement,
            RagdollAppearance::Settings{ settings.mPoisonTransitionTime, settings.mFireTransitionTime,
                settings.mFrostTransitionTime, saved.mFireDuration, saved.mPoisonDuration, saved.mShockDuration });
        impl->initWounds();
        auto result = std::unique_ptr<RagdollController>(new RagdollController(std::move(impl)));
        result->refreshCallbacks();
        result->mImpl->applyPose();
        result->mImpl->updateAppearance();
        result->mImpl->updateWounds(0);
        return result;
    }

    void RagdollController::setWaterLevel(std::optional<float> level)
    {
        mImpl->mSettings.mWaterLevel = level;
    }

    void RagdollController::setRuntimeSettings(int maxActive, float maxSimulationTime, float pinDistance,
        float fireDuration, float poisonDuration, float shockDuration, float bleedDuration)
    {
        auto& impl = *mImpl;
        if (impl.mSettings.mMaxActive != maxActive)
        {
            impl.mSettings.mMaxActive = maxActive;
            impl.enforceBudget(std::clamp(maxActive, 1, 200));
        }
        if (maxSimulationTime != impl.mConfiguredSimulationTime)
        {
            // Restored controllers store the remaining window, not a new full
            // interval. Adjust by the option delta so loading never renews it.
            impl.mSettings.mMaxSimulationTime
                = std::max(0.f, impl.mSettings.mMaxSimulationTime + maxSimulationTime - impl.mConfiguredSimulationTime);
            impl.mConfiguredSimulationTime = maxSimulationTime;
        }
        impl.mSettings.mPinMaxDistance = pinDistance;
        impl.mSettings.mFireEffectDuration = fireDuration;
        impl.mSettings.mPoisonEffectDuration = poisonDuration;
        impl.mSettings.mShockDuration = shockDuration;
        impl.mSettings.mBleedDuration = bleedDuration;
        impl.mAppearance->setEffectDurations(fireDuration, poisonDuration, shockDuration);
    }

    void RagdollController::setArmorMass(float armor)
    {
        if (!std::isfinite(armor))
            return;
        armor = std::max(0.f, armor);
        if (std::abs(armor - mImpl->mSettings.mArmorMass) < .001f)
            return;
        const float mass = std::clamp(mImpl->mBodyBaseMass + armor, .25f, 5000.f);
        const float ratio = mass / mImpl->mMass;
        for (auto& definition : mImpl->mDefinitions)
            definition.mMass *= ratio;
        if (mImpl->mSimulation)
            mImpl->mSimulation->setTotalMass(mass);
        mImpl->mMass = mass;
        mImpl->mSettings.mArmorMass = armor;
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
            mImpl->mSimulationAge += duration;
            const float timeStep = std::min(duration, 0.1f);
            mImpl->mSimulation->setWaterLevel(
                mImpl->mSettings.mWaterLevel ? std::optional<btScalar>(*mImpl->mSettings.mWaterLevel) : std::nullopt);
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
                if (mImpl->mSimulationAge >= mImpl->mSettings.mMaxSimulationTime)
                {
                    if (mImpl->mSimulation->isBelowWaterSurface() && !mImpl->mPinned && !mImpl->mWounds.mCuts)
                    {
                        if (!mImpl->mFloating)
                        {
                            // The articulated window ends on time. Continue only
                            // one rigid shape until the corpse reaches the surface.
                            std::vector<Ragdoll::BodyDefinition> bodies = mImpl->mDefinitions;
                            btVector3 velocity(0, 0, 0), angular(0, 0, 0), center(0, 0, 0);
                            for (std::size_t i = 0; i < bodies.size(); ++i)
                            {
                                bodies[i].mTransform = mImpl->mSimulation->getBodyTransform(i);
                                center += bodies[i].mTransform.getOrigin() * bodies[i].mMass;
                                velocity += mImpl->mSimulation->getBodyLinearVelocity(i) * bodies[i].mMass;
                                angular += mImpl->mSimulation->getBodyAngularVelocity(i) * bodies[i].mMass;
                            }
                            velocity /= mImpl->mMass;
                            angular /= mImpl->mMass;
                            center /= mImpl->mMass;
                            Ragdoll::Settings physics;
                            physics.mBodyMode = Ragdoll::BodyMode::Rigid;
                            mImpl->mSimulation = std::make_unique<Ragdoll::Simulation>(
                                bodies, mImpl->mJoints, makeCollisionMesh(mImpl->mCollisionVertices), physics);
                            mImpl->mDefinitions = std::move(bodies);
                            mImpl->mMode = Ragdoll::BodyMode::Rigid;
                            const auto pointVelocity = velocity
                                + angular.cross(mImpl->mSimulation->getBodyTransform(0).getOrigin() - center);
                            mImpl->mSimulation->setBodyVelocity(0, pointVelocity, angular);
                            mImpl->mFloating = true;
                        }
                    }
                    else
                        mImpl->freeze();
                }
            }
        }
        mImpl->applyPose();
        mImpl->updateAppearance();
        mImpl->updateWounds(duration);
    }

    void RagdollController::setSkinParts(std::vector<osg::ref_ptr<osg::Node>> skinParts)
    {
        mImpl->mSkinParts = std::move(skinParts);
    }
    float RagdollController::getAppearanceBlend() const
    {
        return mImpl->mAppearance->getBlend();
    }
    bool RagdollController::hasFireEffect() const
    {
        return mImpl->mAppearance->hasFire();
    }

    float RagdollController::getMass() const
    {
        return mImpl->mMass;
    }
    bool RagdollController::isSimulating() const
    {
        return bool(mImpl->mSimulation);
    }
    bool RagdollController::isPinned() const
    {
        return mImpl->mPinned;
    }
    Ragdoll::Wounds RagdollController::getWounds() const
    {
        return mImpl->mWounds;
    }
    std::map<std::string, osg::Matrixf> RagdollController::getUncutBoneMatrices() const
    {
        std::map<std::string, osg::Matrixf> result;
        if (!mImpl->mWounds.mCuts)
            return result;
        const auto chest = std::find_if(mImpl->mBones.begin(), mImpl->mBones.end(),
            [](const auto& bone) { return bone.mBody == 1; });
        if (chest == mImpl->mBones.end())
            return result;
        const auto restChest = osg::Matrixf::inverse(chest->mRestWorld
            * osg::Matrixf::inverse(chest->mBoneToBody));
        const auto chestToSkeleton = mImpl->mBodyMatrices[1] * osg::Matrixf::inverse(rootWorldMatrix(*mImpl->mRoot));
        for (const auto& bone : mImpl->mBones)
            result.emplace(bone.mNode->getName(), bone.mRestWorld * restChest * chestToSkeleton);
        return result;
    }

    osg::Vec3f RagdollController::getPosition() const
    {
        return mImpl->mBodyMatrices.front().getTrans();
    }
}
