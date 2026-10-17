#include <apps/openmw/mwrender/ragdoll.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/skeleton.hpp>

#include <osg/FrameStamp>
#include <osg/MatrixTransform>
#include <osgUtil/UpdateVisitor>

#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    bool near(const osg::Matrixf& a, const osg::Matrixf& b, float tolerance = 0.001f)
    {
        for (unsigned i = 0; i < 16; ++i)
            if (std::abs(a.ptr()[i] - b.ptr()[i]) > tolerance)
                return false;
        return true;
    }

    osg::Matrixf worldMatrix(osg::Node& node)
    {
        return osg::computeLocalToWorld(node.getParentalNodePaths().front());
    }

    struct SkeletonFixture
    {
        osg::ref_ptr<osg::MatrixTransform> mWorld = new osg::MatrixTransform;
        osg::ref_ptr<SceneUtil::Skeleton> mSkeleton = new SceneUtil::Skeleton;
        std::map<std::string, osg::ref_ptr<osg::MatrixTransform>, std::less<>> mBones;

        osg::MatrixTransform* add(const std::string& name, osg::Group* parent, const osg::Vec3f& position)
        {
            osg::ref_ptr<osg::MatrixTransform> node = new osg::MatrixTransform;
            node->setName("Bip01 " + name);
            node->setMatrix(osg::Matrixf::translate(position));
            parent->addChild(node);
            mBones.emplace(name, node);
            return node;
        }

        SkeletonFixture()
        {
            mWorld->setMatrix(osg::Matrixf::scale(1.2f, 1.2f, 1.2f)
                * osg::Matrixf::rotate(0.4f, osg::Vec3f(0, 0, 1)) * osg::Matrixf::translate(50, 30, 0));
            mWorld->addChild(mSkeleton);
            auto pelvis = add("Pelvis", mSkeleton, { 0, 0, 65 });
            auto spine = add("Spine", pelvis, { 0, 0, 8 });
            auto spine1 = add("Spine1", spine, { 0, 0, 12 });
            auto chest = add("Spine2", spine1, { 0, 0, 10 });
            auto neck = add("Neck", chest, { 0, 0, 14 });
            add("Head", neck, { 0, 0, 8 });
            for (const int side : { -1, 1 })
            {
                const std::string prefix = side < 0 ? "L " : "R ";
                auto upper = add(prefix + "UpperArm", chest, { side * 18.f, 0, 5 });
                upper->setMatrix(osg::Matrixf::rotate(side * 0.2f, osg::Vec3f(0, 1, 0)) * upper->getMatrix());
                auto lower = add(prefix + "Forearm", upper, { side * 22.f, 0, 0 });
                add(prefix + "Hand", lower, { side * 20.f, 0, 0 });
                auto thigh = add(prefix + "Thigh", pelvis, { side * 9.f, 0, -5 });
                auto calf = add(prefix + "Calf", thigh, { 0, 0, -30 });
                add(prefix + "Foot", calf, { 0, 5, -30 });
            }
        }

        std::map<std::string, osg::Matrixf> pose() const
        {
            std::map<std::string, osg::Matrixf> result;
            for (const auto& [name, bone] : mBones)
                result.emplace(name, worldMatrix(*bone));
            return result;
        }
    };

    std::vector<osg::Vec3f> floorSnapshot(const osg::Vec3f&, float)
    {
        return { { -1000, -1000, 0 }, { 1000, -1000, 0 }, { 1000, 1000, 0 },
            { -1000, -1000, 0 }, { 1000, 1000, 0 }, { -1000, 1000, 0 } };
    }

    class OverwritePose : public SceneUtil::NodeCallback<OverwritePose, osg::MatrixTransform*>
    {
    public:
        void operator()(osg::MatrixTransform* node, osg::NodeVisitor* visitor)
        {
            node->setMatrix(osg::Matrixf::translate(999, 888, 777));
            traverse(node, visitor);
        }
    };

    void transitionAndCallbacks()
    {
        SkeletonFixture fixture;
        const auto before = fixture.pose();
        auto ragdoll = MWRender::RagdollController::create(*fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, {});
        require(bool(ragdoll), "standard synthetic skeleton must create a ragdoll");
        for (const auto& [name, matrix] : fixture.pose())
            require(near(matrix, before.at(name)), "initial pose must preserve offsets, rotation and root scale");

        osg::ref_ptr<OverwritePose> native = new OverwritePose;
        fixture.mBones.at("Spine2")->addUpdateCallback(native);
        ragdoll->refreshCallbacks();
        osgUtil::UpdateVisitor visitor;
        osg::ref_ptr<osg::FrameStamp> frame = new osg::FrameStamp;
        frame->setFrameNumber(1);
        visitor.setFrameStamp(frame);
        visitor.setTraversalNumber(1);
        fixture.mWorld->accept(visitor);
        for (const auto& [name, matrix] : fixture.pose())
            require(near(matrix, before.at(name)), "native callbacks must not override corpse matrices");

        fixture.mWorld->setMatrix(osg::Matrixf::translate(300, -100, 40));
        ragdoll->update(0);
        for (const auto& [name, matrix] : fixture.pose())
            require(near(matrix, before.at(name)), "moving actor root must not move physical corpse pose");

        ragdoll.reset();
        fixture.mWorld->accept(visitor);
        require(near(fixture.mBones.at("Spine2")->getMatrix(), osg::Matrixf::translate(999, 888, 777)),
            "destroying ragdoll must restore native callback control");
        std::cout << "PASS pose transition, callback rebuild, root movement and teardown\n";
    }

    void collisionAndFreeze()
    {
        SkeletonFixture fixture;
        MWRender::RagdollController::Settings settings;
        settings.mMaxSimulationTime = 3;
        settings.mMaxActive = 1;
        auto ragdoll = MWRender::RagdollController::create(*fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, settings);
        require(bool(ragdoll), "first corpse must start");
        SkeletonFixture second;
        require(!MWRender::RagdollController::create(*second.mSkeleton, *second.mSkeleton, floorSnapshot, settings),
            "active corpse budget must reject extra simulations");
        const float originalHeight = ragdoll->getPosition().z();
        for (unsigned i = 0; i < 240; ++i)
            ragdoll->update(1.f / 60);
        require(ragdoll->getPosition().z() < originalHeight - 15, "corpse must physically fall");
        require(ragdoll->getPosition().z() > 0, "corpse must collide with static floor");
        const auto settled = fixture.pose();
        ragdoll->update(1);
        for (const auto& [name, matrix] : fixture.pose())
            require(near(matrix, settled.at(name)), "frozen pose must remain stable");
        auto next = MWRender::RagdollController::create(*second.mSkeleton, *second.mSkeleton, floorSnapshot, settings);
        require(bool(next), "settled corpse must release simulation budget");
        std::cout << "PASS real Bullet floor collision, freeze and active budget\n";
    }

    void fallbackAndSnapshotLoss()
    {
        SkeletonFixture fixture;
        fixture.mBones.at("L Hand")->setName("Unsupported hand");
        require(!MWRender::RagdollController::create(*fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, {}),
            "missing required bones must preserve native death");
        fixture.mBones.at("L Hand")->setName("Bip01 L Hand");
        bool available = false;
        const auto snapshot = [&](const osg::Vec3f& center, float radius) {
            return available ? floorSnapshot(center, radius) : std::vector<osg::Vec3f>{};
        };
        require(!MWRender::RagdollController::create(*fixture.mSkeleton, *fixture.mSkeleton, snapshot, {}),
            "missing initial collision data must preserve native death");
        available = true;
        auto ragdoll = MWRender::RagdollController::create(*fixture.mSkeleton, *fixture.mSkeleton, snapshot, {});
        require(bool(ragdoll), "valid snapshot must allow ragdoll");
        ragdoll->update(0.1f);
        available = false;
        for (unsigned i = 0; i < 6; ++i)
            ragdoll->update(0.1f);
        const auto frozen = fixture.pose();
        ragdoll->update(0.1f);
        for (const auto& [name, matrix] : fixture.pose())
            require(near(matrix, frozen.at(name)), "lost snapshot must freeze rather than use ghost geometry");
        std::cout << "PASS unsupported skeleton, missing geometry and lost snapshot fallbacks\n";
    }
}

int main()
{
    try
    {
        transitionAndCallbacks();
        collisionAndFreeze();
        fallbackAndSnapshotLoss();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
