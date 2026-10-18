#include <apps/openmw/mwrender/ragdoll.hpp>
#include <apps/openmw/mwrender/ragdollappearance.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/skeleton.hpp>

#include <osg/FrameStamp>
#include <osg/Texture2D>
#include <osg/Geometry>
#include <osg/Geode>
#include <osg/MatrixTransform>
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
            && bolt.first.x() > twoHand.first.x() + 3, "weapon distance classes must produce increasing launch motion");
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
        auto corpse = MWRender::RagdollController::create(
            *fixture.mSkeleton, *fixture.mSkeleton, wallSnapshot, {}, impact);
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
        auto corpse = MWRender::RagdollController::create(
            *fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, {}, impact);
        require(bool(corpse), "shock anatomy fixture must start");
        struct Joint
        {
            std::string upper, lower, end;
            osg::Vec3f axis;
        };
        std::array<Joint, 4> hinges{ Joint{ "L Thigh", "L Calf", "L Foot", {} },
            Joint{ "R Thigh", "R Calf", "R Foot", {} },
            Joint{ "L UpperArm", "L Forearm", "L Hand", {} },
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
        bool movedAfterFour = false;
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
            if (frame > 240 && frame < 290)
                movedAfterFour |= !near(worldMatrix(*fixture.mBones.at("L Forearm")), previous.at("L Forearm"), 0.00001f);
            previous = fixture.pose();
        }
        require(movedAfterFour, "electrical death must remain active through the five-second spasm interval");
        require(!corpse->isSimulating(), "electrical corpse must freeze by ten seconds");
        std::cout << "PASS anatomical hinge limits under recoil and five-second electrical spasms\n";
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

    void skinSurfacesAndCleanup()
    {
        for (const auto element : { Ragdoll::DeathElement::Poison, Ragdoll::DeathElement::Fire,
                 Ragdoll::DeathElement::Frost })
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
                require(paintedPixel(*skin->getParent(0)) == original, "fade must start with the original skin texture");
                const float duration = element == Ragdoll::DeathElement::Fire ? 2.f : 1.5f;
                effect.update(duration / 2, { skin }, positions);
                require(std::abs(effect.getBlend() - 0.5f) < 0.001f, "skin transformation must blend over the element's duration");
                const auto halfway = paintedPixel(*skin->getParent(0));
                effect.update(duration, { skin }, positions);
                const auto final = paintedPixel(*skin->getParent(0));
                require((halfway - original).length() > 0.01f && (final - halfway).length() > 0.01f,
                    "surface texture must change gradually, not switch a global colour");
                require(std::abs(final.a() - original.a()) < 0.005f, "skin painting must preserve texture transparency");
                if (element == Ragdoll::DeathElement::Poison)
                    require(final.g() > final.r() * 2 && final.g() > final.b() * 2, "poison must produce green slime texture");
                else if (element == Ragdoll::DeathElement::Fire)
                    require(final.r() < 0.15f && final.g() < 0.15f && final.b() < 0.15f, "fire must produce charred skin");
                else
                    require(final.b() > final.r() && final.g() > original.g(), "frost must produce an icy surface");
                require(armour->getStateSet() == shared && armour->getParent(0) == root,
                    "armour sharing skin assets must retain its original state and attachment");
                require(texture->getImage()->getColor(0, 0) == original,
                    "cached original texture pixels must remain immutable");
                effect.update(5.01f, { skin }, positions);
                require(!effect.hasFire(), "fire and smoke must expire at five seconds");
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
            require(static_cast<osg::Texture2D*>(geometry->getStateSet()->getTextureAttribute(0,
                osg::StateAttribute::TEXTURE)) == texture, "shared source Drawable must remain unchanged");
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
        corpse->update(3.01f);
        require(!corpse->hasFireEffect(), "frozen corpse's fire must stop at five seconds");
        corpse.reset();
        require(skin->getParent(0) == fixture.mSkeleton, "resurrection must remove skin overlay");

        SkeletonFixture frozen;
        impact.mDeathElement = Ragdoll::DeathElement::Frost;
        auto ice = MWRender::RagdollController::create(
            *frozen.mSkeleton, *frozen.mSkeleton, floorSnapshot, {}, impact);
        const auto pelvisBefore = worldMatrix(*frozen.mBones.at("Pelvis"));
        const auto headRelative = worldMatrix(*frozen.mBones.at("Head")) * osg::Matrixf::inverse(pelvisBefore);
        for (int i = 0; i < 120; ++i)
            ice->update(1.f / 60);
        const auto headAfter = worldMatrix(*frozen.mBones.at("Head"))
            * osg::Matrixf::inverse(worldMatrix(*frozen.mBones.at("Pelvis")));
        require(near(headRelative, headAfter, 0.003f), "frost must preserve all relative bone transforms as one rigid object");
        std::cout << "PASS elemental timers after physics freeze and rigid frost skeleton\n";
    }

}

int main()
{
    try
    {
        transitionAndCallbacks();
        collisionAndFreeze();
        fallbackAndSnapshotLoss();
        inertiaAndMass();
        pinAndDeadline();
        anatomicalLimitsAndShock();
        skinSurfacesAndCleanup();
        elementalLifecycle();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
