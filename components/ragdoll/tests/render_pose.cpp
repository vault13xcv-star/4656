#include <apps/openmw/mwrender/ragdoll.hpp>
#include <apps/openmw/mwrender/ragdollappearance.hpp>
#include <btBulletDynamicsCommon.h>
#include <chrono>
#include <components/esm3/ragdollstate.hpp>
#include <components/ragdoll/items.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/skeleton.hpp>

#include <osg/FrameStamp>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/Texture2D>
#include <osgUtil/UpdateVisitor>

#include <array>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <utility>

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
            mWorld->setMatrix(osg::Matrixf::scale(1.2f, 1.2f, 1.2f) * osg::Matrixf::rotate(0.4f, osg::Vec3f(0, 0, 1))
                * osg::Matrixf::translate(50, 30, 0));
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
        return { { -1000, -1000, 0 }, { 1000, -1000, 0 }, { 1000, 1000, 0 }, { -1000, -1000, 0 }, { 1000, 1000, 0 },
            { -1000, 1000, 0 } };
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
        auto ragdoll
            = MWRender::RagdollController::create(*fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, settings);
        require(bool(ragdoll), "first corpse must start");
        SkeletonFixture second;
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

    void overflowFreezesOldPose()
    {
        MWRender::RagdollController::Settings settings;
        settings.mMaxActive = 2;
        SkeletonFixture far, nearPlayer, incoming;
        far.mWorld->setMatrix(osg::Matrixf::translate(1000, 0, 0));
        nearPlayer.mWorld->setMatrix(osg::Matrixf::identity());
        auto oldFar = MWRender::RagdollController::create(*far.mSkeleton, *far.mSkeleton, floorSnapshot, settings);
        auto oldNear = MWRender::RagdollController::create(
            *nearPlayer.mSkeleton, *nearPlayer.mSkeleton, floorSnapshot, settings);
        const auto finalFar = far.pose();
        auto next
            = MWRender::RagdollController::create(*incoming.mSkeleton, *incoming.mSkeleton, floorSnapshot, settings);
        require(next && next->isSimulating() && oldNear->isSimulating() && !oldFar->isSimulating(),
            "new death must evict the distant old corpse rather than fall back to animation");
        oldFar->update(0.2f);
        for (const auto& [name, pose] : far.pose())
            require(near(pose, finalFar.at(name)), "budget eviction lost a visible final pose");
        std::cout << "PASS deterministic corpse overflow and retained frozen pose\n";
    }

    void savedPoseAndRemainingTime()
    {
        SkeletonFixture fixture;
        Ragdoll::Impact impact;
        impact.mValid = true;
        impact.mDeathElement = Ragdoll::DeathElement::Fire;
        auto corpse
            = MWRender::RagdollController::create(*fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, {}, impact);
        for (int i = 0; i < 180; ++i)
            corpse->update(1.f / 60);
        ESM::RagdollState saved;
        corpse->writeState(saved);
        const auto pose = fixture.pose();
        require(saved.mPresent && std::abs(saved.mTimeLeft - 7) < 0.01f, "saved corpse restarted its physics lifetime");
        corpse.reset();
        auto loaded
            = MWRender::RagdollController::restore(*fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, {}, saved);
        require(loaded && loaded->isSimulating(), "active corpse failed to restore");
        for (const auto& [name, matrix] : fixture.pose())
            require(near(matrix, pose.at(name), 0.003f), "restore changed the saved pose");
        for (int i = 0; i < 426; ++i)
            loaded->update(1.f / 60);
        require(!loaded->isSimulating() && loaded->hasFireEffect(),
            "physics remainder or independent fire duration was lost");
        loaded->writeState(saved);
        const auto frozen = fixture.pose();
        loaded.reset();
        auto rest
            = MWRender::RagdollController::restore(*fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, {}, saved);
        require(rest && !rest->isSimulating(), "frozen corpse restarted its dynamics after loading");
        for (const auto& [name, matrix] : fixture.pose())
            require(near(matrix, frozen.at(name), 0.003f), "frozen pose was not restored");
        rest->update(5);
        require(!rest->hasFireEffect(), "loading restarted the full fire effect");
        std::cout << "PASS active/frozen corpse pose restore and remaining physics/effect timers\n";
    }

    void genericCreatureSkeleton()
    {
        SkeletonFixture creature;
        creature.mSkeleton->removeChildren(0, creature.mSkeleton->getNumChildren());
        creature.mBones.clear();
        const auto spine = creature.add("BoneSpine", creature.mSkeleton, { 0, 0, 30 });
        creature.add("BoneHead", spine, { 0, 20, 4 });
        creature.add("BoneTail", spine, { 0, -20, -2 });
        for (const int side : { -1, 1 })
            for (const int front : { -1, 1 })
            {
                const auto name = std::string("BoneLeg") + std::to_string(side) + std::to_string(front);
                const auto upper = creature.add(name, spine, { side * 8.f, front * 12.f, -5 });
                creature.add(name + "Lower", upper, { 0, 0, -20 });
            }
        MWRender::RagdollController::Settings settings;
        settings.mAllowGenericSkeleton = true;
        settings.mBaseMass = 3;
        settings.mReferenceHeight = 50;
        Ragdoll::Impact impact;
        impact.mValid = true;
        impact.mDeathElement = Ragdoll::DeathElement::Frost;
        auto body = MWRender::RagdollController::create(
            *creature.mSkeleton, *creature.mSkeleton, floorSnapshot, settings, impact);
        require(body && body->getMass() < 20, "non-humanoid creature failed to obtain its own mass and skeleton");
        for (int i = 0; i < 120; ++i)
            body->update(1.f / 60);
        ESM::RagdollState saved;
        body->writeState(saved);
        require(saved.mBodies.size() > 4 && saved.mMode == 2, "creature frost lost its compound body");
        body.reset();
        auto restored = MWRender::RagdollController::restore(
            *creature.mSkeleton, *creature.mSkeleton, floorSnapshot, settings, saved);
        require(bool(restored), "non-humanoid pose failed to restore");
        std::cout << "PASS generic creature anatomy, mass, rigid frost and saved pose\n";
    }

    void loadMeasurements()
    {
        for (const auto [count, itemsCount] : { std::pair(0, 0), std::pair(10, 0), std::pair(50, 0), std::pair(100, 0),
                 std::pair(0, 10), std::pair(0, 50), std::pair(0, 100), std::pair(100, 100) })
        {
            std::vector<std::unique_ptr<SkeletonFixture>> fixtures;
            std::vector<std::unique_ptr<MWRender::RagdollController>> bodies;
            for (int i = 0; i < count; ++i)
            {
                fixtures.push_back(std::make_unique<SkeletonFixture>());
                bodies.push_back(MWRender::RagdollController::create(
                    *fixtures.back()->mSkeleton, *fixtures.back()->mSkeleton, floorSnapshot, {}));
                require(bodies.back() && bodies.back()->isSimulating(), "load test could not admit all 100 corpses");
            }
            Ragdoll::ItemSimulation items;
            Ragdoll::StaticCollider floor;
            floor.mShape = std::make_shared<btStaticPlaneShape>(btVector3(0, 0, 1), 0);
            items.replaceStaticColliders({ floor });
            btBoxShape box(btVector3(2, 2, 2));
            for (int i = 1; i <= itemsCount; ++i)
            {
                auto t = btTransform::getIdentity();
                t.setOrigin(btVector3(i * 20, 0, 100));
                items.add(i, box, t, 1);
                items.wake(i);
            }
            double total = 0, maximum = 0;
            for (int frame = 0; frame < 60; ++frame)
            {
                const auto start = std::chrono::steady_clock::now();
                for (auto& body : bodies)
                    body->update(1.f / 60);
                items.advance(1.0 / 60);
                items.takeStateChanges();
                const auto time
                    = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                total += time;
                maximum = std::max(maximum, time);
            }
            std::cout << "LOAD corpses=" << count << " items=" << itemsCount << " mean_ms=" << total / 60
                      << " max_ms=" << maximum << '\n';
        }
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
    void inertiaAndMass()
    {
        const auto displacement = [](float weapon, float magic, float armour, float scale = 1.f) {
            SkeletonFixture fixture;
            fixture.mWorld->setMatrix(osg::Matrixf::scale(scale, scale, scale));
            MWRender::RagdollController::Settings settings;
            settings.mArmorMass = armour;
            Ragdoll::Impact impact;
            impact.mValid = true;
            impact.mWeaponDistance = weapon;
            impact.mMagicDistance = magic;
            impact.mWeaponDirection.set(1, 0, 0);
            impact.mMagicDirection.set(0, 1, 0);
            auto corpse = MWRender::RagdollController::create(
                *fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, settings, impact);
            require(bool(corpse), "impact fixture must start");
            const auto initial = corpse->getPosition();
            for (int frame = 0; frame < 12; ++frame)
                corpse->update(1.f / 120);
            return std::pair(corpse->getPosition() - initial, corpse->getMass());
        };
        const auto dagger = displacement(0, 0, 0);
        const auto oneHand = displacement(0.25f, 0, 0);
        const auto twoHand = displacement(1, 0, 0);
        const auto bolt = displacement(2, 0, 0);
        const auto heavy = displacement(2, 0, 100);
        const auto tall = displacement(2, 0, 0, 1.2f);
        const auto enchanted = displacement(1, 5, 0);
        require(std::abs(dagger.first.x()) < 1, "dagger/arrow must not launch the corpse");
        require(oneHand.first.x() > dagger.first.x() + 1 && twoHand.first.x() > oneHand.first.x() + 3
                && bolt.first.x() > twoHand.first.x() + 3,
            "weapon distance classes must produce increasing launch motion");
        require(heavy.first.x() < bolt.first.x() * 0.6f, "equipped armour mass must reduce recoil");
        require(std::abs(tall.second / dagger.second - 1.728f) < 0.01f,
            "body mass must scale with the cube of skeletal stature");
        require(tall.first.x() < bolt.first.x(), "taller bodies must resist the same impact momentum");
        require(enchanted.first.x() > 2 && enchanted.first.y() > 10,
            "weapon and magical impacts must add along independent world directions");
        std::cout << "PASS weapon recoil, additive magic, cubic stature and equipped mass\n";
    }

    void pinAndDeadline()
    {
        SkeletonFixture fixture;
        fixture.mWorld->setMatrix(osg::Matrixf::identity());
        auto wallSnapshot = [](const osg::Vec3f& center, float radius) {
            auto vertices = floorSnapshot(center, radius);
            const std::array<osg::Vec3f, 6> wall{ osg::Vec3f(80, -300, 0), osg::Vec3f(80, 300, 0),
                osg::Vec3f(80, 300, 300), osg::Vec3f(80, -300, 0), osg::Vec3f(80, 300, 300),
                osg::Vec3f(80, -300, 300) };
            vertices.insert(vertices.end(), wall.begin(), wall.end());
            return vertices;
        };
        Ragdoll::Impact impact;
        impact.mValid = true;
        impact.mCrossbow = true;
        impact.mWeaponDistance = 2;
        impact.mWeaponDirection.set(1, 0, 0);
        auto corpse
            = MWRender::RagdollController::create(*fixture.mSkeleton, *fixture.mSkeleton, wallSnapshot, {}, impact);
        require(bool(corpse), "crossbow wall fixture must start");
        for (int frame = 0; frame < 600; ++frame)
            corpse->update(1.f / 60);
        require(corpse->isPinned(), "crossbow corpse must anchor upon reaching a nearby wall");
        require(worldMatrix(*fixture.mBones.at("Spine2")).getTrans().z() > 30,
            "pinned torso must remain suspended after the physical deadline");
        require(!corpse->isSimulating(), "corpse simulation must stop at ten elapsed seconds");
        const auto frozen = fixture.pose();
        corpse->update(1);
        for (const auto& [name, matrix] : fixture.pose())
            require(near(matrix, frozen.at(name)), "pinned frozen body must remain displayed");

        SkeletonFixture floorOnly;
        floorOnly.mWorld->setMatrix(osg::Matrixf::identity());
        impact.mWeaponDirection.set(0, 0, -1);
        auto falling = MWRender::RagdollController::create(
            *floorOnly.mSkeleton, *floorOnly.mSkeleton, floorSnapshot, {}, impact);
        for (int frame = 0; frame < 120; ++frame)
            falling->update(1.f / 60);
        require(!falling->isPinned(), "a bolt into the floor must not suspend a corpse");

        SkeletonFixture enchanted;
        enchanted.mWorld->setMatrix(osg::Matrixf::identity());
        const auto distantWall = [&](const osg::Vec3f& center, float radius) {
            auto vertices = wallSnapshot(center, radius);
            for (std::size_t i = 6; i < vertices.size(); ++i)
                vertices[i].x() = 120;
            return vertices;
        };
        MWRender::RagdollController::Settings armoured;
        armoured.mArmorMass = 100;
        impact.mWeaponDirection.set(1, 0, 0);
        impact.mMagicDirection.set(1, 0, 0);
        impact.mMagicDistance = 5;
        auto magicalBolt = MWRender::RagdollController::create(
            *enchanted.mSkeleton, *enchanted.mSkeleton, distantWall, armoured, impact);
        for (int frame = 0; frame < 120; ++frame)
            magicalBolt->update(1.f / 60);
        require(magicalBolt->isPinned(),
            "enchantment momentum must allow a heavy bolt victim to reach and pin beyond weapon-only range");

        SkeletonFixture stalled;
        impact.mCrossbow = false;
        impact.mShock = true;
        impact.mDeathElement = Ragdoll::DeathElement::Shock;
        MWRender::RagdollController::Settings settings;
        settings.mShockDuration = 10;
        auto timed = MWRender::RagdollController::create(
            *stalled.mSkeleton, *stalled.mSkeleton, floorSnapshot, settings, impact);
        timed->update(9.9f);
        require(timed->isSimulating(), "solver time cap must not advance the deadline early");
        timed->update(0.11f);
        require(!timed->isSimulating(), "deadline must use elapsed game time, not capped solver time");
        std::cout << "PASS bolt wall pin, floor exclusion, suspended freeze and elapsed deadline\n";
    }

    void anatomicalLimitsAndShock()
    {
        SkeletonFixture fixture;
        fixture.mWorld->setMatrix(osg::Matrixf::identity());
        Ragdoll::Impact impact;
        impact.mValid = true;
        impact.mShock = true;
        impact.mDeathElement = Ragdoll::DeathElement::Shock;
        impact.mMagicDistance = 5;
        impact.mMagicDirection.set(1, 1, 0);
        auto corpse
            = MWRender::RagdollController::create(*fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, {}, impact);
        require(bool(corpse), "shock anatomy fixture must start");
        struct Joint
        {
            std::string upper, lower, end;
            osg::Vec3f axis;
        };
        std::array<Joint, 4> hinges{ Joint{ "L Thigh", "L Calf", "L Foot", {} },
            Joint{ "R Thigh", "R Calf", "R Foot", {} }, Joint{ "L UpperArm", "L Forearm", "L Hand", {} },
            Joint{ "R UpperArm", "R Forearm", "R Hand", {} } };
        for (std::size_t i = 0; i < hinges.size(); ++i)
        {
            auto& joint = hinges[i];
            const auto parent = worldMatrix(*fixture.mBones.at(joint.upper));
            const auto proximal = worldMatrix(*fixture.mBones.at(joint.lower)).getTrans() - parent.getTrans();
            auto axis = proximal ^ osg::Vec3f(0, i < 2 ? -1.f : 1.f, 0);
            axis.normalize();
            joint.axis = osg::Matrixf::transform3x3(axis, osg::Matrixf::inverse(parent));
        }
        bool movedAfterNine = false;
        auto previous = fixture.pose();
        for (int frame = 0; frame < 600; ++frame)
        {
            corpse->update(1.f / 60);
            for (const auto& joint : hinges)
            {
                const auto parent = worldMatrix(*fixture.mBones.at(joint.upper));
                const auto knee = worldMatrix(*fixture.mBones.at(joint.lower)).getTrans();
                auto proximal = knee - parent.getTrans();
                auto distal = worldMatrix(*fixture.mBones.at(joint.end)).getTrans() - knee;
                proximal.normalize();
                distal.normalize();
                auto axis = osg::Matrixf::transform3x3(joint.axis, parent);
                axis.normalize();
                const float angle = std::atan2(axis * (proximal ^ distal), proximal * distal);
                require(angle > -0.16f && angle < 2.7f,
                    "knee/elbow must remain within anatomical flexion, including shock impacts");
            }
            if (frame > 540 && frame < 570)
                movedAfterNine
                    |= !near(worldMatrix(*fixture.mBones.at("L Forearm")), previous.at("L Forearm"), 0.00001f);
            previous = fixture.pose();
        }
        require(
            movedAfterNine, "electrical death must retain movement during the final second of the ten-second interval");
        require(!corpse->isSimulating(), "electrical corpse must freeze by ten seconds");
        std::cout << "PASS anatomical hinge limits under recoil and ten-second electrical spasms\n";
    }

    osg::ref_ptr<osg::Texture2D> testSkinTexture()
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(8, 8, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                image->setColor(osg::Vec4f(0.7f, 0.5f, 0.3f, 0.6f), x, y);
        return new osg::Texture2D(image);
    }

    osg::Vec4f paintedPixel(osg::Node& node)
    {
        const auto* texture = static_cast<const osg::Texture2D*>(
            node.getStateSet()->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
        return texture->getImage()->getColor(0, 0);
    }

    void sharedWornDrawableIsPaintedOnlyOnSelectedActor()
    {
        osg::ref_ptr<osg::Group> root = new osg::Group;
        osg::ref_ptr<osg::Geode> selected = new osg::Geode, other = new osg::Geode;
        osg::ref_ptr<osg::Geometry> shared = new osg::Geometry;
        auto texture = testSkinTexture();
        shared->getOrCreateStateSet()->setTextureAttributeAndModes(0, texture, osg::StateAttribute::ON);
        selected->addDrawable(shared);
        other->addDrawable(shared);
        root->addChild(selected);
        root->addChild(other);
        {
            MWRender::RagdollAppearance coating(*root, Ragdoll::DeathElement::Poison, {});
            coating.update(2, { selected }, {});
            require(selected->getDrawable(0) != shared && other->getDrawable(0) == shared,
                "shared armour/hair Drawable was skipped or another NPC was painted");
        }
        require(selected->getDrawable(0) == shared && other->getDrawable(0) == shared,
            "unload did not restore shared worn mesh attachments");
        std::cout << "PASS shared armour/hair drawables and per-actor cleanup\n";
    }

    void wornSurfaceCoverageAndDetail()
    {
        for (auto element :
            { Ragdoll::DeathElement::Fire, Ragdoll::DeathElement::Poison, Ragdoll::DeathElement::Frost })
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            std::array<osg::ref_ptr<osg::Group>, 3> parts{ new osg::Group, new osg::Group, new osg::Group };
            const std::array<osg::Vec4f, 3> colors{ osg::Vec4f(.8, .2, .1, .6), osg::Vec4f(.2, .4, .8, .6),
                osg::Vec4f(.5, .5, .5, .6) };
            std::array<osg::ref_ptr<osg::StateSet>, 3> originals;
            for (int part = 0; part < 3; ++part)
            {
                osg::ref_ptr<osg::Image> image = new osg::Image;
                image->allocateImage(256, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE);
                for (int y = 0; y < 256; ++y)
                    for (int x = 0; x < 256; ++x)
                        image->setColor(colors[part], x, y);
                originals[part] = new osg::StateSet;
                originals[part]->setTextureAttributeAndModes(0, new osg::Texture2D(image), osg::StateAttribute::ON);
                parts[part]->setStateSet(originals[part]);
                root->addChild(parts[part]);
            }
            {
                MWRender::RagdollAppearance effect(*root, element, {});
                effect.update(3, { parts[0], parts[1], parts[2] }, {});
                std::array<const osg::Image*, 3> images;
                for (int part = 0; part < 3; ++part)
                {
                    auto* wrapper = parts[part]->getParent(0);
                    require(wrapper != root, "worn armour/hair was omitted from coating");
                    images[part] = static_cast<const osg::Texture2D*>(
                        wrapper->getStateSet()->getTextureAttribute(0, osg::StateAttribute::TEXTURE))
                                       ->getImage();
                }
                int covered = 0;
                for (int y = 0; y < 256; ++y)
                    for (int x = 0; x < 256; ++x)
                    {
                        const auto a = images[0]->getColor(x, y), b = images[1]->getColor(x, y);
                        require(std::abs(a.a() - .6f) < .005f, "coating filled transparent hair cutouts");
                        const float transmission = (a.r() - b.r()) / .6f;
                        if (element == Ragdoll::DeathElement::Fire)
                        {
                            if (std::abs(transmission - 1.f) > .02f)
                                ++covered;
                            require(transmission > .11f, "soot erased original texture detail");
                        }
                        else if (element == Ragdoll::DeathElement::Frost)
                            require(transmission > .19f && transmission < .31f,
                                "frost opacity left the requested 70–80 percent interval");
                        else
                            require(std::abs(a.r() - b.r()) < .005f && std::abs(a.g() - b.g()) < .005f,
                                "slime failed to cover the whole worn surface");
                    }
                if (element == Ragdoll::DeathElement::Fire)
                    require(std::abs(covered / (256.f * 256) - .9f) < .005f, "soot coverage must be 90 percent");
            }
            for (int part = 0; part < 3; ++part)
                require(parts[part]->getParent(0) == root && parts[part]->getStateSet() == originals[part],
                    "cell unload left coating overrides attached");
        }
        std::cout << "PASS whole-actor coatings, 90% soot, 70–80% frost and original-detail preservation\n";
    }

    void skinSurfacesAndCleanup()
    {
        for (const auto element :
            { Ragdoll::DeathElement::Poison, Ragdoll::DeathElement::Fire, Ragdoll::DeathElement::Frost })
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            osg::ref_ptr<osg::Group> skin = new osg::Group;
            osg::ref_ptr<osg::Group> armour = new osg::Group;
            auto texture = testSkinTexture();
            osg::ref_ptr<osg::StateSet> shared = new osg::StateSet;
            shared->setTextureAttributeAndModes(0, texture, osg::StateAttribute::ON);
            skin->setStateSet(shared);
            armour->setStateSet(shared);
            root->addChild(skin);
            root->addChild(armour);
            const auto original = texture->getImage()->getColor(0, 0);
            {
                MWRender::RagdollAppearance effect(*root, element, {});
                const std::array<osg::Vec3f, 2> positions{ osg::Vec3f(0, 0, 10), osg::Vec3f(0, 10, 10) };
                effect.update(0, { skin }, positions);
                require(
                    paintedPixel(*skin->getParent(0)) == original, "fade must start with the original skin texture");
                const float duration = element == Ragdoll::DeathElement::Fire ? 2.f : 1.5f;
                effect.update(duration / 2, { skin }, positions);
                require(std::abs(effect.getBlend() - 0.5f) < 0.001f,
                    "skin transformation must blend over the element's duration");
                const auto halfway = paintedPixel(*skin->getParent(0));
                effect.update(duration, { skin }, positions);
                const auto final = paintedPixel(*skin->getParent(0));
                require((halfway - original).length() > 0.01f && (final - halfway).length() > 0.01f,
                    "surface texture must change gradually, not switch a global colour");
                require(
                    std::abs(final.a() - original.a()) < 0.005f, "skin painting must preserve texture transparency");
                if (element == Ragdoll::DeathElement::Poison)
                    require(final.g() > final.r() * 2 && final.g() > final.b() * 2,
                        "poison must produce green slime texture");
                else if (element == Ragdoll::DeathElement::Fire)
                    require(final.r() > .06f && final.r() < original.r() * .65f,
                        "soot must darken but retain original texture");
                else
                    require(final.g() > original.g() && std::abs(final.r() - final.b()) < .2f,
                        "frost must whiten the original surface without painting it solid blue");
                require(armour->getStateSet() == shared && armour->getParent(0) == root,
                    "armour sharing skin assets must retain its original state and attachment");
                require(texture->getImage()->getColor(0, 0) == original,
                    "cached original texture pixels must remain immutable");
                effect.update(15.01f, { skin }, positions);
                require(!effect.hasFire(), "fire and smoke must expire at fifteen seconds");
            }
            require(skin->getParent(0) == root && skin->getStateSet() == shared && root->getNumChildren() == 2,
                "removal/resurrection must restore original attachments and states");
        }

        osg::ref_ptr<osg::Group> root = new osg::Group;
        osg::ref_ptr<osg::Geode> skin = new osg::Geode;
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        auto texture = testSkinTexture();
        geometry->getOrCreateStateSet()->setTextureAttributeAndModes(0, texture, osg::StateAttribute::ON);
        skin->addDrawable(geometry);
        root->addChild(skin);
        {
            MWRender::RagdollAppearance effect(*root, Ragdoll::DeathElement::Poison, {});
            effect.update(1.5f, { skin }, {});
            require(skin->getNumDrawables() == 1 && skin->getDrawable(0) && skin->getDrawable(0) != geometry,
                "a Geode must keep a per-instance Drawable, never a Group child");
            require(paintedPixel(*skin->getDrawable(0)).g() > paintedPixel(*skin->getDrawable(0)).r(),
                "direct Drawable materials must receive slime texture");
            require(static_cast<osg::Texture2D*>(
                        geometry->getStateSet()->getTextureAttribute(0, osg::StateAttribute::TEXTURE))
                    == texture,
                "shared source Drawable must remain unchanged");
        }
        require(skin->getDrawable(0) == geometry, "Geode drawable must restore after effect destruction");
        {
            MWRender::RagdollAppearance directPart(*root, Ragdoll::DeathElement::Poison, {});
            directPart.update(0.5f, { geometry }, {});
            directPart.update(1.5f, {}, {});
            require(skin->getNumDrawables() == 0 && geometry->getNumParents() == 0,
                "removing a direct Drawable skin part must remove its clone without resurrecting the source");
        }

        osg::ref_ptr<osg::Group> removed = new osg::Group;
        removed->getOrCreateStateSet()->setTextureAttributeAndModes(0, texture, osg::StateAttribute::ON);
        root->addChild(removed);
        MWRender::RagdollAppearance changing(*root, Ragdoll::DeathElement::Poison, {});
        changing.update(0.5f, { removed }, {});
        removed->getParent(0)->removeChild(removed); // Same operation as PartHolder destruction.
        changing.update(1.5f, {}, {});
        require(removed->getNumParents() == 0 && root->getNumChildren() == 1,
            "equipment replacement must not reattach removed skin or leave an empty overlay");
        std::cout << "PASS skin-only texture transitions, shared-asset isolation, Geode support and cleanup\n";
    }

    void elementalLifecycle()
    {
        SkeletonFixture fixture;
        osg::ref_ptr<osg::Group> skin = new osg::Group;
        skin->getOrCreateStateSet()->setTextureAttributeAndModes(0, testSkinTexture(), osg::StateAttribute::ON);
        fixture.mSkeleton->addChild(skin);
        Ragdoll::Impact impact;
        impact.mValid = true;
        impact.mDeathElement = Ragdoll::DeathElement::Fire;
        MWRender::RagdollController::Settings settings;
        settings.mMaxSimulationTime = 0.1f;
        auto corpse = MWRender::RagdollController::create(
            *fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, settings, impact);
        corpse->setSkinParts({ skin });
        corpse->update(0.2f);
        require(!corpse->isSimulating() && corpse->hasFireEffect() && corpse->getAppearanceBlend() < 1,
            "early physics freeze must retain unfinished skin fade and fire");
        corpse->update(1.8f);
        require(corpse->getAppearanceBlend() == 1 && corpse->hasFireEffect(),
            "char transition must finish at two seconds after physics has frozen");
        corpse->update(8.f);
        require(corpse->hasFireEffect(), "fire must remain visible after the normal ten-second physics deadline");
        corpse->update(4.9f);
        require(corpse->hasFireEffect(), "fire must last until fifteen seconds");
        corpse->update(0.11f);
        require(!corpse->hasFireEffect() && corpse->getAppearanceBlend() == 1,
            "fire must stop at fifteen seconds while the charred covering remains");
        corpse.reset();
        require(skin->getParent(0) == fixture.mSkeleton, "resurrection must remove skin overlay");

        SkeletonFixture frozen;
        impact.mDeathElement = Ragdoll::DeathElement::Frost;
        auto ice = MWRender::RagdollController::create(*frozen.mSkeleton, *frozen.mSkeleton, floorSnapshot, {}, impact);
        const auto pelvisBefore = worldMatrix(*frozen.mBones.at("Pelvis"));
        const auto headRelative = worldMatrix(*frozen.mBones.at("Head")) * osg::Matrixf::inverse(pelvisBefore);
        for (int i = 0; i < 120; ++i)
            ice->update(1.f / 60);
        const auto headAfter
            = worldMatrix(*frozen.mBones.at("Head")) * osg::Matrixf::inverse(worldMatrix(*frozen.mBones.at("Pelvis")));
        require(near(headRelative, headAfter, 0.003f),
            "frost must preserve all relative bone transforms as one rigid object");
        std::cout << "PASS elemental timers after physics freeze and rigid frost skeleton\n";
    }

    void visibleShockAndFrozenFall()
    {
        for (int facing = 0; facing < 8; ++facing)
        {
            SkeletonFixture radial;
            radial.mWorld->setMatrix(
                osg::Matrixf::rotate(facing * 0.785398f, osg::Vec3f(0, 0, 1)) * osg::Matrixf::translate(0, 0, 200));
            MWRender::RagdollController::Settings settings;
            settings.mArmorMass = 1000;
            Ragdoll::Impact wave;
            wave.mValid = true;
            wave.mDeathElement = Ragdoll::DeathElement::Frost;
            wave.mMagicDirection.set(1, 0, 0);
            wave.mMagicDistance = 0.5f;
            auto body = MWRender::RagdollController::create(
                *radial.mSkeleton, *radial.mSkeleton, floorSnapshot, settings, wave);
            const auto start = body->getPosition();
            for (int i = 0; i < 6; ++i)
                body->update(1.f / 120);
            const auto motion = body->getPosition() - start;
            require(motion.x() > 0.1 && std::abs(motion.y()) < 0.001,
                "A facing-dependent toppling kick redirected the explosion of a heavy corpse");
        }
        SkeletonFixture frozen;
        frozen.mWorld->setMatrix(osg::Matrixf::identity());
        Ragdoll::Impact impact;
        impact.mValid = true;
        impact.mDeathElement = Ragdoll::DeathElement::Frost;
        auto statue
            = MWRender::RagdollController::create(*frozen.mSkeleton, *frozen.mSkeleton, floorSnapshot, {}, impact);
        const float height = worldMatrix(*frozen.mBones.at("Head")).getTrans().z();
        for (int i = 0; i < 240; ++i)
            statue->update(1.f / 60);
        require(statue->isSimulating(), "Frost ragdoll was destroyed before the ten second deadline");
        require(worldMatrix(*frozen.mBones.at("Head")).getTrans().z() < height - 30,
            "An upright frozen death did not topple under gravity");
        statue.reset();

        SkeletonFixture electric;
        electric.mWorld->setMatrix(osg::Matrixf::identity());
        impact.mDeathElement = Ragdoll::DeathElement::Shock;
        auto corpse
            = MWRender::RagdollController::create(*electric.mSkeleton, *electric.mSkeleton, floorSnapshot, {}, impact);
        float torsoTravel = 0, legTravel = 0;
        auto previous = electric.pose();
        for (int i = 0; i < 300; ++i)
        {
            corpse->update(1.f / 60);
            const auto current = electric.pose();
            if (i > 180)
            {
                torsoTravel += (current.at("Spine2").getTrans() - previous.at("Spine2").getTrans()).length();
                legTravel += (current.at("L Foot").getTrans() - previous.at("L Foot").getTrans()).length();
            }
            previous = current;
        }
        std::cout << "Shock travel in seconds 3–5: torso=" << torsoTravel << " leg=" << legTravel << '\n';
        require(torsoTravel > 8 && legTravel > 20, "Shock only produced subpixel hand tremors");
        std::cout << "PASS visible whole-body shock and topple of upright frozen corpse\n";
    }

    void liveSettingsAndExtendedBudget()
    {
        MWRender::RagdollController::Settings settings;
        settings.mMaxActive = 200;
        std::vector<std::unique_ptr<SkeletonFixture>> fixtures;
        std::vector<std::unique_ptr<MWRender::RagdollController>> corpses;
        for (int i = 0; i < 200; ++i)
        {
            auto fixture = std::make_unique<SkeletonFixture>();
            auto corpse = MWRender::RagdollController::create(
                *fixture->mSkeleton, *fixture->mSkeleton, floorSnapshot, settings);
            require(bool(corpse), "extended corpse budget failed to create a supported skeleton");
            fixtures.push_back(std::move(fixture));
            corpses.push_back(std::move(corpse));
        }
        int active = 0;
        for (const auto& corpse : corpses)
            active += corpse->isSimulating();
        require(active == 200, "UI +100% corpse budget was still clamped at 100");
        corpses.back()->setRuntimeSettings(50, 10, 2.5, 30, 20, 20);
        active = 0;
        for (const auto& corpse : corpses)
            active += corpse->isSimulating();
        require(active == 50, "live corpse budget reduction did not freeze excess bodies");
        corpses.clear();
        fixtures.clear();
        SkeletonFixture fixture;
        auto corpse = MWRender::RagdollController::create(*fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, {});
        corpse->update(.2f);
        corpse->setRuntimeSettings(100, 5, 2.5, 30, 20, 20);
        ESM::RagdollState state;
        corpse->writeState(state);
        require(std::abs(state.mTimeLeft - 4.8f) < .01f, "live corpse duration restarted the physical window");
        corpse.reset();
        MWRender::RagdollController::Settings restoredSettings;
        restoredSettings.mMaxSimulationTime = 5;
        auto restored = MWRender::RagdollController::restore(
            *fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, restoredSettings, state);
        require(bool(restored), "runtime-settings save failed to restore");
        restored->setRuntimeSettings(100, 5, 2.5, 30, 20, 20);
        ESM::RagdollState again;
        restored->writeState(again);
        require(std::abs(again.mTimeLeft - state.mTimeLeft) < .01f, "polling settings renewed a restored corpse timer");
        restored->setRuntimeSettings(100, 10, 2.5, 15, 10, 10);
        restored->writeState(again);
        require(std::abs(again.mTimeLeft - 9.8f) < .01f, "restored duration adjustment lost the elapsed window");
        require(again.mFireDuration == 15 && again.mPoisonDuration == 10 && again.mShockDuration == 10,
            "status duration settings failed to update saved corpse effects");
        std::cout << "PASS 200 corpses, live budget reduction and saved runtime timer adjustments\n";
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc > 1 && std::string(argv[1]) == "--benchmark")
        {
            loadMeasurements();
            return 0;
        }
        liveSettingsAndExtendedBudget();
        transitionAndCallbacks();
        collisionAndFreeze();
        overflowFreezesOldPose();
        savedPoseAndRemainingTime();
        genericCreatureSkeleton();
        fallbackAndSnapshotLoss();
        inertiaAndMass();
        pinAndDeadline();
        anatomicalLimitsAndShock();
        skinSurfacesAndCleanup();
        wornSurfaceCoverageAndDetail();
        sharedWornDrawableIsPaintedOnlyOnSelectedActor();
        elementalLifecycle();
        visibleShockAndFrozenFall();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
