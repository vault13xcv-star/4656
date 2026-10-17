#include "ragdoll.hpp"

#include <components/misc/strings/lower.hpp>
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
        std::vector<osg::Matrixf> mBodyMatrices;
        std::vector<osg::Vec3f> mCollisionVertices;
        float mElapsed = 0.f;
        float mSinceSnapshot = 0.f;
        bool mBudgetSlot = false;

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
        SceneUtil::Skeleton& skeleton, CollisionSnapshot snapshot, const Settings& settings)
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
            body.mRadius = std::max(1.f, std::min(torsoLength * radii[i], length * 0.45f));
            body.mLength = std::max(btScalar(0), length - 2.f * body.mRadius);
            body.mMass = masses[i];
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
            joint.mFrameWorld = bodies[child].mTransform;
            joint.mFrameWorld.setOrigin(toBullet(starts[child]));
            // Limits are relative to the captured pose, so transition does not snap to a guessed bind pose.
            // Knees/elbows have a narrower swing; wrists/ankles remain part of their distal segment.
            const bool hinge = child == 4 || child == 6 || child == 8 || child == 10;
            const bool torso = child == 1 || child == 2;
            const float swing = hinge ? 1.25f : torso ? 0.5f : 1.0f;
            joint.mAngularLower = btVector3(-swing, -0.35f, hinge ? -0.15f : -swing);
            joint.mAngularUpper = btVector3(swing, 0.35f, hinge ? 0.15f : swing);
            joints.push_back(joint);
        }

        auto impl = std::make_unique<Impl>();
        impl->mRoot = &root;
        impl->mSkeleton = &skeleton;
        impl->mSnapshot = std::move(snapshot);
        impl->mSettings = settings;
        impl->mCollisionVertices = std::move(vertices);
        impl->mSimulation = std::make_unique<Ragdoll::Simulation>(std::move(bodies), std::move(joints), std::move(colliders));
        if (!impl->mSimulation->isValid())
            return nullptr;
        impl->mBones = std::move(visitor.mBones);
        impl->mBodyMatrices.reserve(driven.size());
        for (std::size_t i = 0; i < driven.size(); ++i)
            impl->mBodyMatrices.push_back(toOsg(impl->mSimulation->getBodyTransform(i)));
        ++sSimulatingCorpses;
        impl->mBudgetSlot = true;

        // A small, deterministic loss-of-balance impulse avoids perfectly upright statues.
        // This is deliberately not presented as an inferred lethal-hit direction.
        osg::Vec3f forward = osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), rootWorldMatrix(root));
        forward.z() = 0.f;
        if (forward.normalize() != 0.f)
            impl->mSimulation->applyImpulse(1, toBullet(forward * 80.f), toBullet(neck));

        auto result = std::unique_ptr<RagdollController>(new RagdollController(std::move(impl)));
        result->refreshCallbacks();
        result->mImpl->applyPose();
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
        if (mImpl->mSimulation && duration > 0.f && std::isfinite(duration))
        {
            const float timeStep = std::min(duration, 0.1f);
            mImpl->mElapsed += timeStep;
            mImpl->mSinceSnapshot += timeStep;
            if (mImpl->mSinceSnapshot >= mImpl->mSettings.mCollisionRefreshInterval)
            {
                auto vertices = mImpl->mSnapshot(getPosition(), mImpl->mSettings.mCollisionRadius);
                if (vertices.empty())
                {
                    // An empty snapshot also represents a failed/over-budget query. Do not keep
                    // simulating against geometry that may have disappeared since the last query.
                    mImpl->freeze();
                    mImpl->applyPose();
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
                for (std::size_t i = 0; i < mImpl->mBodyMatrices.size(); ++i)
                    mImpl->mBodyMatrices[i] = toOsg(mImpl->mSimulation->getBodyTransform(i));
                if (mImpl->mSimulation->isSleeping() || mImpl->mElapsed >= mImpl->mSettings.mMaxSimulationTime)
                    mImpl->freeze();
            }
        }
        mImpl->applyPose();
    }

    osg::Vec3f RagdollController::getPosition() const
    {
        return mImpl->mBodyMatrices.front().getTrans();
    }
}
