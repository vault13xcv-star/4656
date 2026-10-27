#include "ragdoll.hpp"
#include "ragdollappearance.hpp"
#include "brutalgore.hpp"
#include "brutaltextures.hpp"
#include "brutalbloodlightingbase.hpp"
#include <components/ragdoll/blood.hpp>

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
#include <BulletCollision/CollisionShapes/btCapsuleShape.h>
#include <BulletCollision/CollisionShapes/btTriangleMesh.h>
#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>

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

        struct HumanoidPose
        {
            std::array<int, 15> mDriven;
            std::array<osg::Vec3f, 15> mStarts, mStops;
            float mTorsoLength = 0.f;
            bool mValid = false;

            HumanoidPose(const CollectBones& visitor, const osg::Vec3f& forward)
            {
                const auto find = [&](std::initializer_list<const char*> names) -> int {
                    for (const char* name : names)
                        if (const auto found = visitor.mNames.find(name); found != visitor.mNames.end())
                            return static_cast<int>(found->second);
                    return -1;
                };
                // The order is also the saved humanoid body/joint numbering.
                mDriven = { find({ "bip01 pelvis" }), find({ "bip01 spine2", "bip01 spine1", "bip01 spine" }),
                    find({ "bip01 head" }), find({ "bip01 l upperarm" }), find({ "bip01 l forearm" }),
                    find({ "bip01 r upperarm" }), find({ "bip01 r forearm" }), find({ "bip01 l thigh" }),
                    find({ "bip01 l calf" }), find({ "bip01 r thigh" }), find({ "bip01 r calf" }),
                    find({ "bip01 l hand" }), find({ "bip01 r hand" }), find({ "bip01 l foot" }), find({ "bip01 r foot" }) };
                const std::array<int, 5> ends{ find({ "bip01 neck" }), find({ "bip01 l hand" }),
                    find({ "bip01 r hand" }), find({ "bip01 l foot" }), find({ "bip01 r foot" }) };
                if (std::ranges::any_of(mDriven, [](int i) { return i < 0; })
                    || std::ranges::any_of(ends, [](int i) { return i < 0; }))
                    return;
                const auto position = [&](int index) { return osg::Vec3f(visitor.mBones[index].mRestWorld.getTrans()); };
                const osg::Vec3f pelvis = position(mDriven[0]);
                const osg::Vec3f neck = position(ends[0]);
                mTorsoLength = (neck - pelvis).length();
                if (!std::isfinite(mTorsoLength) || mTorsoLength < 8.f || mTorsoLength > 512.f)
                    return;
                for (std::size_t i = 0; i < mDriven.size(); ++i)
                    mStarts[i] = position(mDriven[i]);
                mStops[0] = pelvis + (neck - pelvis) * .3f;
                mStarts[1] = mStops[0];
                mStops[1] = neck;
                osg::Vec3f headAxis = position(mDriven[2]) - neck;
                if (headAxis.length2() < 1.f)
                    headAxis = (neck - pelvis) * .2f;
                mStarts[2] = neck;
                mStops[2] = position(mDriven[2]) + headAxis * .5f;
                mStops[3] = position(mDriven[4]);
                mStops[4] = position(ends[1]);
                mStops[5] = position(mDriven[6]);
                mStops[6] = position(ends[2]);
                mStops[7] = position(mDriven[8]);
                mStops[8] = position(ends[3]);
                mStops[9] = position(mDriven[10]);
                mStops[10] = position(ends[4]);
                mStops[11] = mStarts[11] + unit(mStops[4] - mStarts[4]) * (mTorsoLength * .14f);
                mStops[12] = mStarts[12] + unit(mStops[6] - mStarts[6]) * (mTorsoLength * .14f);
                mStops[13] = mStarts[13] + forward * (mTorsoLength * .2f);
                mStops[14] = mStarts[14] + forward * (mTorsoLength * .2f);
                mValid = true;
                for (std::size_t i = 0; i < mDriven.size(); ++i)
                {
                    const float length = (mStops[i] - mStarts[i]).length();
                    mValid &= std::isfinite(length) && length >= 1.f && length <= mTorsoLength * 4.f;
                }
            }

            float radius(std::size_t part) const
            {
                constexpr std::array<float, 15> radii{ .22f, .22f, .16f, .09f, .075f, .09f, .075f,
                    .13f, .10f, .13f, .10f, .07f, .07f, .09f, .09f };
                const float length = (mStops[part] - mStarts[part]).length();
                return std::max(1.f, part == 0 ? mTorsoLength * radii[part]
                    : std::min(mTorsoLength * radii[part], length * .45f));
            }

            int hitBody(const osg::Vec3f& contact) const
            {
                if (!mValid || !std::isfinite(contact.length2()))
                    return -1;
                int closest = -1;
                float distance = std::numeric_limits<float>::max();
                for (std::size_t part = 0; part < mDriven.size(); ++part)
                {
                    const auto axis = mStops[part] - mStarts[part];
                    const float length = axis.length();
                    const float r = radius(part);
                    const auto centre = (mStarts[part] + mStops[part]) * .5f;
                    const auto direction = axis / length;
                    const float half = std::max(0.f, length * .5f - r);
                    const auto point = centre + direction * std::clamp((contact - centre) * direction, -half, half);
                    // Keep signed depth: clamping overlapping capsules to zero
                    // made their array order choose the pelvis over an arm/head.
                    const float d = (contact - point).length() - r;
                    if (d < distance)
                    {
                        distance = d;
                        closest = static_cast<int>(part);
                    }
                }
                // Bulky clothing may sit outside a capsule, but distant contacts
                // and unsupported body layouts must never invent an amputation.
                return distance <= mTorsoLength * .25f ? closest : -1;
            }

            RagdollController::HitContact hitContact(const osg::Vec3f& from, const osg::Vec3f& to) const
            {
                RagdollController::HitContact result;
                const auto ray = to - from;
                if (!mValid || !std::isfinite(from.length2()) || !std::isfinite(to.length2()) || ray.length2() < .001f)
                    return result;
                float fraction = 2.f;
                const btTransform rayFrom(btQuaternion::getIdentity(), toBullet(from));
                const btTransform rayTo(btQuaternion::getIdentity(), toBullet(to));
                for (std::size_t part = 0; part < mDriven.size(); ++part)
                {
                    const float r = radius(part);
                    btCapsuleShape capsule(r, std::max(0.f, (mStops[part] - mStarts[part]).length() - 2.f * r));
                    btCollisionObject object;
                    object.setCollisionShape(&capsule);
                    object.setWorldTransform(capsuleTransform(mStarts[part], mStops[part]));
                    btCollisionWorld::ClosestRayResultCallback hit(toBullet(from), toBullet(to));
                    btCollisionWorld::rayTestSingle(rayFrom, rayTo, &object, &capsule, object.getWorldTransform(), hit);
                    if (hit.hasHit() && hit.m_closestHitFraction < fraction)
                    {
                        fraction = hit.m_closestHitFraction;
                        result = { static_cast<int>(part), toOsg(hit.m_hitPointWorld) };
                    }
                }
                if (result.mBody >= 0)
                    return result;
                // Native melee uses a cone wider than a crosshair ray. For a
                // confirmed damaging hit, choose the nearest segment to that
                // aim instead of requiring a second mesh hit or random blood point.
                float distance = mTorsoLength * 2.f;
                for (std::size_t part = 0; part < mDriven.size(); ++part)
                {
                    const auto axis = mStops[part] - mStarts[part];
                    const float length = axis.length();
                    const float r = radius(part);
                    const auto centre = (mStarts[part] + mStops[part]) * .5f;
                    const float half = std::max(0.f, length * .5f - r);
                    const auto start = centre - axis * (half / length);
                    const auto segment = axis * (2.f * half / length);
                    const auto delta = from - start;
                    const float a = ray.length2(), e = segment.length2();
                    const float b = ray * segment, c = ray * delta, f = segment * delta;
                    float s = 0.f, t = 0.f;
                    if (e < .0001f)
                        s = std::clamp(-c / a, 0.f, 1.f);
                    else
                    {
                        const float denominator = a * e - b * b;
                        s = denominator > .0001f ? std::clamp((b * f - c * e) / denominator, 0.f, 1.f) : 0.f;
                        t = (b * s + f) / e;
                        if (t < 0.f)
                        {
                            t = 0.f;
                            s = std::clamp(-c / a, 0.f, 1.f);
                        }
                        else if (t > 1.f)
                        {
                            t = 1.f;
                            s = std::clamp((b - c) / a, 0.f, 1.f);
                        }
                    }
                    const auto point = start + segment * t;
                    const auto offset = from + ray * s - point;
                    const float gap = offset.length() - r;
                    if (gap < distance)
                    {
                        distance = gap;
                        result = { static_cast<int>(part), point + unit(offset, -unit(ray)) * r };
                    }
                }
                return result;
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
        int mGutsStart = -1;
        int mGutsCount = 0;
        std::vector<osg::ref_ptr<osg::Geode>> mGutNodes;
        bool mGutsRendered = false;
        std::unique_ptr<BloodTrail> mBlood;
        BloodTrail::Surface mBloodSurface;
        osg::Vec3f mBloodSurfaceCenter;
        float mBloodSurfaceTime=-1, mBloodSurfaceAttempt=-1;
        std::vector<Ragdoll::StaticCollider> mBloodColliders;
        std::map<osg::Node*, std::pair<osg::ref_ptr<osg::Node>, osg::Node::NodeMask>> mSplitParts;
        std::array<osg::Matrixf, 3> mSplitBindLeft, mSplitBindRight;
        osg::Vec3f mSliceOrigin,mSliceUp{0,0,1},mSliceForward{0,1,0};
        float mSliceHeight=1;
        std::map<std::pair<std::size_t, bool>, PartPose> mPartPoses;
        float mSinceBlood = .25f;
        std::size_t mNextBloodSprite = 0;
        struct WoundVisual
        {
            osg::ref_ptr<osg::MatrixTransform> mNode;
            std::size_t mBody;
            osg::Matrixf mLocal;
            osg::Node* mSource = nullptr;
        };
        std::vector<WoundVisual> mWoundVisuals;

        void prepareBrutalBodies(std::vector<Ragdoll::BodyDefinition>& bodies,
            std::vector<Ragdoll::JointDefinition>& joints, std::vector<BonePose>& bones)
        {
            if (mWounds.mLongitudinal)
            {
                const auto original = bodies;
                const auto oldJoints = joints;
                for (unsigned i = 0; i < 3; ++i)
                {
                    auto right = original[i];
                    const auto offset = toBullet(mRight * float(right.mRadius) * .45f);
                    bodies[i].mTransform.getOrigin() -= offset;
                    right.mTransform.getOrigin() += offset;
                    bodies[i].mMass *= .5; right.mMass *= .5;
                    bodies[i].mRadius *= .52; right.mRadius *= .52;
                    bodies.push_back(right); // 15 pelvis, 16 torso, 17 head.
                }
                for (auto& bone : bones)
                    if (bone.mBody >= 0 && bone.mBody < 3)
                        bone.mBoneToBody = bone.mRestWorld * osg::Matrixf::inverse(toOsg(bodies[bone.mBody].mTransform));
                for (std::size_t i = 0; i < joints.size(); ++i)
                {
                    auto& joint = joints[i];
                    const auto& old = oldJoints[i];
                    const auto parentWorld = original[old.mParent].mTransform * old.mFrameInParent;
                    const auto childWorld = original[old.mChild].mTransform * old.mFrameInChild;
                    if (joint.mChild == 5) joint.mParent = 16;
                    if (joint.mChild == 9) joint.mParent = 15;
                    joint.mFrameInParent = bodies[joint.mParent].mTransform.inverse() * parentWorld;
                    joint.mFrameInChild = bodies[joint.mChild].mTransform.inverse() * childWorld;
                }
                for (unsigned i = 0; i < 2; ++i)
                {
                    auto joint = oldJoints[i];
                    const auto parentWorld = original[joint.mParent].mTransform * joint.mFrameInParent;
                    const auto childWorld = original[joint.mChild].mTransform * joint.mFrameInChild;
                    joint.mParent += 15; joint.mChild += 15;
                    joint.mFrameInParent = bodies[joint.mParent].mTransform.inverse() * parentWorld;
                    joint.mFrameInChild = bodies[joint.mChild].mTransform.inverse() * childWorld;
                    joints.push_back(joint);
                }
            }
            if (!mWounds.cuts(1) && !mWounds.mLongitudinal) return;
            mGutsStart = static_cast<int>(bodies.size());
            mGutsCount = 36;
            const float scale = std::clamp(mTorsoLength / 30.f, .35f, 3.f);
            const auto waist = toOsg((bodies[joints[0].mParent].mTransform * joints[0].mFrameInParent).getOrigin());
            const float mass = std::min(.06f * scale * scale * scale, float(bodies[1].mMass) * .12f / mGutsCount);
            bodies[1].mMass -= mass * mGutsCount;
            for (unsigned chain = 0; chain < 2; ++chain)
            {
                const auto anchor = waist + mForward * (mPelvisRadius * .95f) + mRight * ((chain ? 2.f : -2.f) * scale);
                auto previous = anchor;
                for (unsigned i = 0; i < 18; ++i)
                {
                    const float t = (i + 1) * .8f;
                    const auto next = anchor + mRight * (std::sin(t) * 3.f * scale)
                        + mForward * ((1.f - std::cos(t)) * 3.f * scale)
                        - osg::Vec3f(0, 0, (i + 1) * 1.7f * scale);
                    Ragdoll::BodyDefinition body;
                    body.mTransform = capsuleTransform(previous, next);
                    body.mRadius = .68f * scale;
                    body.mLength = std::max(.1f, (next - previous).length() - float(body.mRadius) * 2);
                    body.mMass = mass;
                    Ragdoll::JointDefinition joint;
                    joint.mParent = i ? bodies.size() - 1 : 1;
                    joint.mChild = bodies.size();
                    joint.mUseLocalFrames = true;
                    const auto frame = hingeFrame(previous, next - previous, mRight);
                    joint.mFrameInParent = bodies[joint.mParent].mTransform.inverse() * frame;
                    joint.mFrameInChild = body.mTransform.inverse() * frame;
                    joint.mAngularLower = btVector3(-1.2, -1.2, -1.2);
                    joint.mAngularUpper = btVector3(1.2, 1.2, 1.2);
                    bodies.push_back(body); joints.push_back(joint);
                    previous = next;
                }
            }
        }

        void prepareSplitBindings()
        {
            if (!mWounds.mLongitudinal) return;
            for (unsigned i = 0; i < 3; ++i)
            {
                const auto bone = std::find_if(mBones.begin(), mBones.end(), [i](const auto& b) { return b.mBody == int(i); });
                if (bone == mBones.end()) continue;
                mSplitBindLeft[i] = osg::Matrixf::inverse(bone->mBoneToBody) * bone->mRestWorld;
                mSplitBindRight[i] = mSplitBindLeft[i];
                mSplitBindRight[i].setTrans(mSplitBindLeft[i].getTrans()
                    + mRight * float(mDefinitions[i].mRadius / .52 * .9));
            }
            const osg::Vec3f pelvis=(mSplitBindLeft[0].getTrans()+mSplitBindRight[0].getTrans())*.5f;
            const osg::Vec3f head=(mSplitBindLeft[2].getTrans()+mSplitBindRight[2].getTrans())*.5f;
            mSliceUp=unit(head-pelvis);
            mSliceForward=unit(mForward-mSliceUp*(mForward*mSliceUp),mForward);
            const float bottom=float(mDefinitions[0].mRadius/.52);
            const float top=float(mDefinitions[2].mLength*.5+mDefinitions[2].mRadius/.52);
            mSliceOrigin=pelvis-mSliceUp*bottom;
            mSliceHeight=std::max(1.f,(head-pelvis)*mSliceUp+bottom+top);
        }

        void refreshBloodSurface(const osg::Vec3f& center)
        {
            if (!mSettings.mBloodSurface || mBloodSurfaceAttempt == mElapsed) return;
            if (mBloodSurface && mElapsed-mBloodSurfaceTime<.5f
                && (center-mBloodSurfaceCenter).length2() <=128.f*128.f) return;
            mBloodSurfaceAttempt=mElapsed;
            auto surface=mSettings.mBloodSurface(center,768.f*std::max(1.f,Ragdoll::bloodLaunchScale(mSettings.mBloodAmount)));
            if (surface) {mBloodSurface=std::move(surface);mBloodSurfaceCenter=center;mBloodSurfaceTime=mElapsed;}
        }
        bool bloodFlightRay(const osg::Vec3f& from,const osg::Vec3f& to,osg::Vec3f& point,osg::Vec3f& normal)
        {
            if (!mSettings.mBloodSurface) return bloodRay(from,to,point,normal);
            refreshBloodSurface(mBodyMatrices.front().getTrans());
            return mBloodSurface.mRaycast && mBloodSurface.mRaycast(from,to,point,normal);
        }

        bool bloodRay(const osg::Vec3f& from, const osg::Vec3f& to, osg::Vec3f& point, osg::Vec3f& normal)
        {
            if (mSimulation)
            {
                btVector3 p, n;
                if (!mSimulation->raycastStatic(toBullet(from), toBullet(to), p, n)) return false;
                point = toOsg(p); normal = toOsg(n); return true;
            }
            if (mBloodColliders.empty()) mBloodColliders = makeCollisionMesh(mCollisionVertices);
            btCollisionWorld::ClosestRayResultCallback hit(toBullet(from), toBullet(to));
            for (const auto& collider : mBloodColliders)
            {
                btCollisionObject object;
                object.setCollisionShape(collider.mShape.get()); object.setWorldTransform(collider.mTransform);
                btTransform a = btTransform::getIdentity(), b = a;
                a.setOrigin(toBullet(from)); b.setOrigin(toBullet(to));
                btCollisionWorld::rayTestSingle(a, b, &object, collider.mShape.get(), object.getWorldTransform(), hit);
            }
            if (!hit.hasHit()) return false;
            point = toOsg(hit.m_hitPointWorld); normal = toOsg(hit.m_hitNormalWorld); return true;
        }

        void updateGuts()
        {
            if (mGutsStart < 0 || (!mSimulation && mGutsRendered)) return;
            const auto inverseRoot = osg::Matrixf::inverse(rootWorldMatrix(*mRoot));
            for (int chain = 0; chain < mGutsCount / 18; ++chain)
            {
                if (chain >= int(mGutNodes.size()))
                {
                    osg::ref_ptr<osg::Geode> node = new osg::Geode;
                    node->setName("Brutal Morrowind physical intestines"); node->setStateSet(createWetGoreState());
                    mRoot->addChild(node); mGutNodes.push_back(node);
                }
                std::vector<osg::Vec3f> centers;
                for (int i = 0; i < 18; ++i) centers.push_back(mBodyMatrices.at(mGutsStart + chain * 18 + i).getTrans() * inverseRoot);
                const float radius = float(mDefinitions[mGutsStart].mRadius) * 1.05f;
                auto& node = *mGutNodes[chain];
                if (!node.getNumDrawables())
                    node.addDrawable(makeIntestineTube(centers, radius));
                else
                    updateIntestineTube(*node.getDrawable(0)->asGeometry(), centers, radius);
            }
            mGutsRendered = true;
        }

        void addWoundVisual(osg::Shape* shape, std::size_t body, const osg::Matrixf& local,bool torso=false)
        {
            osg::ref_ptr<osg::Shape> ownedShape=shape;
            osg::ref_ptr<osg::MatrixTransform> node = new osg::MatrixTransform;
            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            auto* assets=findBrutalTextures(*mRoot);
            auto* texture=assets?assets->mTextures[torso?BrutalTextures::Torso:BrutalTextures::Limb].get():nullptr;
            if(texture)
            {
                geode->setStateSet(createTexturedGoreState(*texture,assets->mMasks[torso?BrutalTextures::Torso:BrutalTextures::Limb]));
                geode->setUserValue("RealCombatPreserveMaterial",true);
                geode->addDrawable(makeWoundGeometry(*shape,torso,assets));
                attachBloodLighting(*geode,*mRoot);
            }
            else geode->addDrawable(createWoundDrawable(shape));
            node->addChild(geode);
            mRoot->addChild(node);
            mWoundVisuals.push_back({ node, body, local });
        }

        void initWounds()
        {
            prepareSplitBindings();
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
                    disc * toOsg(joint.mFrameInParent),joint.mChild==1);
                if (!(mWounds.mBurstHead && joint.mChild == 2))
                    addWoundVisual(new osg::Cylinder(osg::Vec3(), radius, .8f), joint.mChild,
                        disc * toOsg(joint.mFrameInChild),joint.mChild==1);
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
            updateGuts();
            if (!hasBleedingWounds())
                return;
            if (!mBlood && mSettings.mBloodDeposit && mWounds.any()
                && mElapsed < mSettings.mBleedDuration && mSettings.mBloodAmount > 0)
                mBlood = std::make_unique<BloodTrail>(*mRoot,
                    [this](const auto& a, const auto& b, auto& p, auto& n) { return bloodFlightRay(a, b, p, n); }, mSettings.mBloodDeposit,
                    mSettings.mBloodSurface ? BloodTrail::Projector{[this](const auto& point,const auto& normal,float radius) {
                        refreshBloodSurface(mBodyMatrices.front().getTrans());
                        return mBloodSurface.mProjector ? mBloodSurface.mProjector(point,normal,radius) : std::vector<osg::Vec3f>{};
                    }} : BloodTrail::Projector{},
                    [this] {
                        if (!mSettings.mBloodSurface) return true;
                        refreshBloodSurface(mBodyMatrices.front().getTrans());
                        return bool(mBloodSurface);
                    },mSettings.mBloodProjectionBudget);
            if (mBlood)
            {
                std::vector<BloodTrail::Source> sources;
                if (mElapsed < mSettings.mBleedDuration)
                {
                    const float pressure = 1.f - .8f * mElapsed / std::max(.1f, mSettings.mBleedDuration);
                    for (const auto& joint : mJoints)
                    {
                        if (!mWounds.cuts(joint.mChild)) continue;
                        const auto parent = toOsg(joint.mFrameInParent) * mBodyMatrices[joint.mParent];
                        sources.push_back({ parent.getTrans(), unit(osg::Matrixf::transform3x3(osg::Vec3f(1, 0, 0), parent)), pressure, joint.mChild == 2, joint.mChild == 1 || joint.mChild == 2 });
                        if (!(mWounds.mBurstHead && joint.mChild == 2))
                        {
                            const auto child = toOsg(joint.mFrameInChild) * mBodyMatrices[joint.mChild];
                            sources.push_back({ child.getTrans(), -unit(osg::Matrixf::transform3x3(osg::Vec3f(1, 0, 0), child)), pressure, false, joint.mChild == 1 || joint.mChild == 2 });
                        }
                    }
                    if (mWounds.mLongitudinal)
                        for (unsigned part=0;part<3;++part)
                            for (unsigned half=0;half<2;++half)
                            {
                                const unsigned body=part+half*15;
                                if (body>=mBodyMatrices.size()) continue;
                                const auto localRight=osg::Matrixf::transform3x3(mRight,osg::Matrixf::inverse(toOsg(mDefinitions[body].mTransform)));
                                const auto right=unit(osg::Matrixf::transform3x3(localRight,mBodyMatrices[body]));
                                const auto normal=half ? -right : right;
                                sources.push_back({mBodyMatrices[body].getTrans()+normal*float(mDefinitions[body].mRadius/.52*.45),normal,pressure,false,true});
                            }
                }
                mBlood->update(duration, sources, mSettings.mBloodAmount, mElapsed);
            }
            if (mElapsed >= mSettings.mBleedDuration + BloodTrail::MaxDropAge + BloodTrail::MaxPendingAge + .25f)
            {
                mBlood.reset();
                mBloodColliders.clear();
                if (!mSimulation) std::vector<osg::Vec3f>().swap(mCollisionVertices);
            }
            mSinceBlood += std::max(0.f, duration);
            if (mSettings.mBloodDeposit || !mSettings.mBloodEffect || mSettings.mBloodAmount <= 0 || mElapsed >= mSettings.mBleedDuration
                || mSinceBlood < .25f / std::max(.1f, mSettings.mBloodAmount))
                return;
            mSinceBlood = 0;
            std::vector<osg::Vec3f> spritePositions;
            for (const auto& joint : mJoints)
            {
                if (!mWounds.cuts(joint.mChild))
                    continue;
                spritePositions.push_back((toOsg(joint.mFrameInParent) * mBodyMatrices[joint.mParent]).getTrans());
                if (!(mWounds.mBurstHead && joint.mChild == 2))
                    spritePositions.push_back((toOsg(joint.mFrameInChild) * mBodyMatrices[joint.mChild]).getTrans());
            }
            // The ballistic trail already covers every wound. Rotate at most
            // two legacy NIF particle systems per pulse instead of duplicating
            // a whole system at both ends of every severed joint every pulse.
            for (std::size_t i = 0; i < std::min<std::size_t>(2, spritePositions.size()); ++i)
                mSettings.mBloodEffect(spritePositions[mNextBloodSprite++ % spritePositions.size()]);
        }

        bool hasBleedingWounds() const
        {
            return mWounds.any() && Ragdoll::woundsHaveBlood(mAppearanceElement);
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
            mBlood.reset();
            for (const auto& node : mGutNodes) mRoot->removeChild(node);
            for (const auto& [key, value] : mSplitParts) value.first->setNodeMask(value.second);
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
            enforceBudget(std::clamp(mSettings.mMaxActive, 1, 300) - 1);
            mActivation = ++sNextActivation;
            mBudgetSlot = true;
            ++sSimulatingCorpses;
            std::lock_guard lock(sRegistryMutex);
            sRegistry.push_back(self);
        }

        void freeze()
        {
            if (!mSettings.mBloodSurface && mSettings.mBloodDeposit && hasBleedingWounds() && mElapsed < mSettings.mBleedDuration + BloodTrail::MaxDropAge + BloodTrail::MaxPendingAge + .25f)
                mBloodColliders = makeCollisionMesh(mCollisionVertices);
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

    int RagdollController::findHitBody(osg::Group& root, const osg::Vec3f& contact)
    {
        const auto matrix = rootWorldMatrix(root);
        CollectBones visitor(matrix);
        root.accept(visitor);
        return HumanoidPose(visitor, unit(osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), matrix))).hitBody(contact);
    }

    RagdollController::HitContact RagdollController::findHitContact(
        osg::Group& root, const osg::Vec3f& from, const osg::Vec3f& to)
    {
        const auto matrix = rootWorldMatrix(root);
        CollectBones visitor(matrix);
        root.accept(visitor);
        return HumanoidPose(visitor, unit(osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), matrix))).hitContact(from, to);
    }

    std::unique_ptr<RagdollController> RagdollController::create(osg::Group& root, SceneUtil::Skeleton& skeleton,
        CollisionSnapshot snapshot, const Settings& settings, const Ragdoll::Impact& impact)
    {
        if (&root != &skeleton || settings.mMaxActive < 1)
            return nullptr;

        CollectBones visitor(rootWorldMatrix(root));
        root.accept(visitor);
        float torsoLength = 40, legLength = 40, mass = settings.mBaseMass;
        std::vector<Ragdoll::BodyDefinition> bodies;
        std::vector<Ragdoll::JointDefinition> joints;
        const osg::Matrixf rootMatrix = rootWorldMatrix(root);
        const osg::Vec3f forward = unit(osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), rootMatrix));
        const osg::Vec3f right = unit(osg::Matrixf::transform3x3(osg::Vec3f(1, 0, 0), rootMatrix));
        const HumanoidPose anatomy(visitor, forward);
        const bool humanoid = anatomy.mValid;
        if (humanoid)
        {
            const auto& driven = anatomy.mDriven;
            const auto& starts = anatomy.mStarts;
            const auto& stops = anatomy.mStops;
            const auto pelvis = starts[0];
            const auto neck = stops[1];
            torsoLength = anatomy.mTorsoLength;
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
            bodies.reserve(driven.size());
            for (std::size_t i = 0; i < driven.size(); ++i)
            {
                const float length = (stops[i] - starts[i]).length();
                if (!std::isfinite(length) || length < 1.f || length > torsoLength * 4.f)
                    return nullptr;
                Ragdoll::BodyDefinition body;
                body.mTransform = capsuleTransform(starts[i], stops[i]);
                body.mRadius = anatomy.radius(i);
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
            impl->mWounds = Ragdoll::selectWounds(impact, impact.mWeaponHitBody,
                settings.mWoundRoll, settings.mWoundChance);
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
        if (humanoid && settings.mNpc) impl->prepareBrutalBodies(bodies, joints, visitor.mBones);
        Ragdoll::Settings physics;
        physics.mMaxLinearSpeed = std::max(2000.f, settings.mMaxLaunchSpeed * 9.f);
        if (impact.mValid && impact.mDeathElement == Ragdoll::DeathElement::Poison)
            physics.mBodyMode = Ragdoll::BodyMode::Soft;
        else if (impact.mValid && impact.mDeathElement == Ragdoll::DeathElement::Frost)
            physics.mBodyMode = Ragdoll::BodyMode::Rigid;
        if (impl->mWounds.any())
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
        if (impl->mWounds.mLongitudinal)
            for (std::size_t part = 0; part < 18; ++part)
            {
                const bool rightHalf = part >= 15 || part == 5 || part == 6 || part == 9 || part == 10 || part == 12 || part == 14;
                impl->mSimulation->applyImpulse(part, toBullet((rightHalf ? right : -right)
                    * (80.f * float(impl->mDefinitions[part].mMass))), impl->mDefinitions[part].mTransform.getOrigin());
            }
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
            // Keep the flight time at the baseline. Scaling vertical lift with
            // horizontal speed squared the travel and made the slider misleading.
            const auto scaleTravel = [](osg::Vec3f velocity, float scale) {
                scale = std::isfinite(scale) ? std::clamp(scale, .5f, 3.f) : 1.f;
                if (velocity.x() * velocity.x() + velocity.y() * velocity.y() > .0001f)
                {
                    velocity.x() *= scale;
                    velocity.y() *= scale;
                }
                else
                    velocity.z() *= std::sqrt(scale);
                return velocity;
            };
            osg::Vec3f magicVelocity = impact.mMagicAttraction
                ? osg::Vec3f()
                : launchVelocity(impact.mMagicDistance / std::max(.5f, impact.mMagicImpulseScale),
                      impact.mMagicDirection);
            const float referenceFactor = settings.mImpulseReferenceMass / mass * settings.mImpulseMultiplier;
            const auto limitBase = [&](osg::Vec3f velocity) {
                const float speed = velocity.length() * referenceFactor;
                return speed > settings.mMaxLaunchSpeed ? velocity * (settings.mMaxLaunchSpeed / speed) : velocity;
            };
            // Limit the baseline, then scale. Limiting the final result erased
            // differences at the upper end (or even at both ends) of a slider.
            weaponVelocity = scaleTravel(limitBase(weaponVelocity), impact.mWeaponImpulseScale);
            magicVelocity = scaleTravel(limitBase(magicVelocity), impact.mMagicImpulseScale);
            osg::Vec3f velocity
                = (weaponVelocity + magicVelocity)
                * (settings.mImpulseReferenceMass / mass * settings.mImpulseMultiplier);
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
                impl->mSimulation->applyImpact(closest,
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
        state.mLongitudinal = impl.mWounds.mLongitudinal;
        state.mCrushed = impl.mWounds.mCrushed;
        state.mGutsStart = impl.mGutsStart;
        state.mGutsCount = impl.mGutsCount;
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
        impl->mWounds = { saved.mCuts, saved.mBurstHead, saved.mLongitudinal, saved.mCrushed };
        impl->mGutsStart = saved.mGutsStart;
        impl->mGutsCount = saved.mGutsCount;
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
            physics.mMaxLinearSpeed = std::max(2000.f, settings.mMaxLaunchSpeed * 9.f);
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
        if (!impl->mSimulation && settings.mBloodDeposit && impl->hasBleedingWounds()
            && impl->mElapsed < impl->mSettings.mBleedDuration)
            impl->mCollisionVertices = impl->mSnapshot(impl->mBodyMatrices.front().getTrans(), settings.mCollisionRadius);
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
        float fireDuration, float poisonDuration, float shockDuration, float bleedDuration, float bloodAmount)
    {
        auto& impl = *mImpl;
        if (impl.mSettings.mMaxActive != maxActive)
        {
            impl.mSettings.mMaxActive = maxActive;
            impl.enforceBudget(std::clamp(maxActive, 1, 300));
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
        if (impl.mSettings.mBleedDuration != bleedDuration && !impl.mSimulation && impl.mSettings.mBloodDeposit
            && impl.hasBleedingWounds() && impl.mElapsed < bleedDuration && impl.mBloodColliders.empty())
            impl.mCollisionVertices = impl.mSnapshot(impl.mBodyMatrices.front().getTrans(), impl.mSettings.mCollisionRadius);
        impl.mSettings.mBleedDuration = bleedDuration;
        impl.mSettings.mBloodAmount = bloodAmount;
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
                    if (mImpl->mSimulation->isBelowWaterSurface() && !mImpl->mPinned && !mImpl->mWounds.any())
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
                            physics.mMaxLinearSpeed = std::max(2000.f, mImpl->mSettings.mMaxLaunchSpeed * 9.f);
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
        for (auto it = mImpl->mSplitParts.begin(); it != mImpl->mSplitParts.end();)
        {
            if (std::find(skinParts.begin(), skinParts.end(), it->second.first) != skinParts.end()) { ++it; continue; }
            const auto* source = it->first;
            auto& visuals = mImpl->mWoundVisuals;
            visuals.erase(std::remove_if(visuals.begin(), visuals.end(), [&](const auto& v) {
                if (v.mSource != source) return false;
                mImpl->mRoot->removeChild(v.mNode); return true;
            }), visuals.end());
            it->second.first->setNodeMask(it->second.second);
            it = mImpl->mSplitParts.erase(it);
        }
        mImpl->mSkinParts = std::move(skinParts);
        for (const auto& visual : mImpl->mWoundVisuals)
            if (visual.mSource) mImpl->mSkinParts.push_back(visual.mNode);
    }

    void RagdollController::splitPart(osg::Node& part, std::size_t body) const
    {
        auto& impl = *mImpl;
        if (!impl.mWounds.mLongitudinal || body >= 3 || impl.mDefinitions.size() < 18) return;
        if (impl.mSplitParts.contains(&part)) { part.setNodeMask(0); return; }
        const auto& leftBind = impl.mSplitBindLeft[body];
        const auto& rightBind = impl.mSplitBindRight[body];
        const auto pose = osg::Matrixf::inverse(leftBind) * impl.mBodyMatrices[body];
        const auto plane = (leftBind.getTrans() + rightBind.getTrans()) * .5f;
        GoreSliceMapping mapping;
        mapping.mOrigin=impl.mSliceOrigin*pose;
        mapping.mForward=unit(osg::Matrixf::transform3x3(impl.mSliceForward,pose));
        mapping.mUp=unit(osg::Matrixf::transform3x3(impl.mSliceUp,pose));
        mapping.mHeight=impl.mSliceHeight;
        mapping.mTextures=findBrutalTextures(*impl.mRoot);
        mapping.mLightingRoot=impl.mRoot;
        auto halves = splitGoreMesh(part, plane * pose,
            unit(osg::Matrixf::transform3x3(impl.mRight, pose)),&mapping);
        if (!halves[0]->getNumChildren() && !halves[1]->getNumChildren()) return;
        impl.mSplitParts.emplace(&part, std::make_pair(osg::ref_ptr<osg::Node>(&part), part.getNodeMask()));
        part.setNodeMask(0);
        for (unsigned side = 0; side < 2; ++side)
        {
            if (!halves[side]->getNumChildren()) continue;
            osg::ref_ptr<osg::MatrixTransform> node = new osg::MatrixTransform;
            node->setName("Brutal Morrowind longitudinal half");
            node->addChild(halves[side]);
            const auto local = side ? osg::Matrixf::inverse(impl.mBodyMatrices[body]) * leftBind * osg::Matrixf::inverse(rightBind)
                : osg::Matrixf::inverse(impl.mBodyMatrices[body]);
            const auto owner = side ? body + 15 : body;
            node->setMatrix(local * impl.mBodyMatrices[owner] * osg::Matrixf::inverse(rootWorldMatrix(*impl.mRoot)));
            impl.mRoot->addChild(node);
            impl.mWoundVisuals.push_back({ node, owner, local, &part });
        }
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
    const RagdollController::PartPose& RagdollController::getPartPose(std::size_t body, bool whole) const
    {
        static const PartPose empty;
        if (!mImpl->mWounds.any() || body >= mImpl->mDefinitions.size())
            return empty;
        const auto key = std::make_pair(body, whole);
        if (const auto found = mImpl->mPartPoses.find(key); found != mImpl->mPartPoses.end())
            return found->second;
        const auto anchor = std::find_if(mImpl->mBones.begin(), mImpl->mBones.end(),
            [body](const auto& bone) { return bone.mBody == static_cast<int>(body); });
        if (anchor == mImpl->mBones.end())
            return empty;
        PartPose result;

        // The anatomy is parent-first. Each cut starts a separate connected
        // component, retaining elbow, knee, wrist and ankle articulation.
        std::vector<std::size_t> components(mImpl->mDefinitions.size(), 0);
        for (std::size_t i = 0; i < components.size(); ++i) components[i] = i;
        const auto component = [&](std::size_t i) { while (components[i] != i) i = components[i]; return i; };
        for (const auto& joint : mImpl->mJoints)
            if (!mImpl->mWounds.cuts(joint.mChild)) components[component(joint.mChild)] = component(joint.mParent);
        for (std::size_t i = 0; i < components.size(); ++i) components[i] = component(i);
        result.mAnchor = anchor->mNode->getName();
        const auto restAnchor = osg::Matrixf::inverse(anchor->mRestWorld);
        std::map<const osg::Node*, std::size_t> boneBodies;
        for (const auto& bone : mImpl->mBones)
        {
            std::size_t owner = 0;
            if (bone.mBody >= 0)
                owner = static_cast<std::size_t>(bone.mBody);
            else
                for (auto parent = bone.mParents.rbegin(); parent != bone.mParents.rend(); ++parent)
                    if (const auto found = boneBodies.find(*parent); found != boneBodies.end())
                    {
                        owner = found->second;
                        break;
                    }
            boneBodies.emplace(bone.mNode.get(), owner);
            // Clothing meshes have nonzero skin weights on adjacent parts even
            // when they are separate NIF body parts. Reconstruct foreign bones
            // relative to this part instead of blending toward a flying limb.
            if (whole || components[owner] != components[body])
                result.mOffsets.emplace(bone.mNode->getName(), bone.mRestWorld * restAnchor);
        }
        return mImpl->mPartPoses.emplace(key, std::move(result)).first->second;
    }

    osg::Vec3f RagdollController::getPosition() const
    {
        return mImpl->mBodyMatrices.front().getTrans();
    }
}
