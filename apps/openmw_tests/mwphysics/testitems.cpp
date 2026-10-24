#include "apps/openmw/mwclass/classes.hpp"
#include "apps/openmw/mwbase/environment.hpp"
#include "apps/openmw/mwmechanics/combat.hpp"
#include "apps/openmw/mwmechanics/creaturestats.hpp"
#include "apps/openmw/mwmechanics/ragdollimpact.hpp"
#include "apps/openmw/mwphysics/itemhit.hpp"
#include "apps/openmw/mwphysics/object.hpp"
#include "apps/openmw/mwphysics/physicssystem.hpp"
#include "apps/openmw/mwphysics/projectile.hpp"
#include "apps/openmw/mwphysics/projectileconvexcallback.hpp"
#include "apps/openmw/mwworld/cellvisitors.hpp"
#include "apps/openmw/mwworld/class.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"
#include "apps/openmw/mwworld/livecellref.hpp"

#include <components/esm3/loadalch.hpp>
#include <components/esm3/loadappa.hpp>
#include <components/esm3/loadarmo.hpp>
#include <components/esm3/loadbook.hpp>
#include <components/esm3/loadclot.hpp>
#include <components/esm3/creaturestats.hpp>
#include <components/esm3/loadingr.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/esm3/loadlock.hpp>
#include <components/esm3/loadmisc.hpp>
#include <components/esm3/loadprob.hpp>
#include <components/esm3/loadrepa.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/esm3/loadweap.hpp>
#include <components/misc/mathutil.hpp>
#include <components/ragdoll/dismemberment.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/settings/values.hpp>
#include <components/testing/util.hpp>
#include <components/toutf8/toutf8.hpp>

#include <osg/Geode>
#include <osg/ShapeDrawable>
#include <osgDB/Registry>

#include <gtest/gtest.h>

namespace
{
    // Real record classes, native scene insertion, mesh loading, contact rays,
    // collision snapshots and pose updates. No Morrowind assets are needed.
    std::string boxMesh(float x, float y, float z)
    {
        osg::ref_ptr<osg::Geode> node = new osg::Geode;
        node->addDrawable(new osg::ShapeDrawable(new osg::Box(osg::Vec3f(), x, y, z)));
        auto* writer = osgDB::Registry::instance()->getReaderWriterForExtension("osgt");
        if (!writer)
            throw std::runtime_error("OSG serialization plugin unavailable");
        std::ostringstream stream;
        if (!writer->writeNode(*node, stream).success())
            throw std::runtime_error("Unable to serialize physics test mesh");
        return stream.str();
    }

    template <class Record>
    struct Reference
    {
        Record mBase;
        std::unique_ptr<MWWorld::LiveCellRef<Record>> mRef;

        Reference(float x, float y, float z, float mass = 1.f)
        {
            mBase.blank();
            mBase.mId = ESM::RefId::stringRefId("physics-test");
            mBase.mModel = "box.osgt";
            if constexpr (!std::is_same_v<Record, ESM::Static>)
                mBase.mData.mWeight = mass;
            if constexpr (std::is_same_v<Record, ESM::Light>)
                mBase.mData.mFlags |= ESM::Light::Carry;
            ESM::CellRef ref;
            ref.blank();
            ref.mRefID = mBase.mId;
            ref.mPos = {};
            ref.mPos.pos[0] = x;
            ref.mPos.pos[1] = y;
            ref.mPos.pos[2] = z;
            mRef = std::make_unique<MWWorld::LiveCellRef<Record>>(ref, &mBase);
        }
        MWWorld::Ptr ptr() { return MWWorld::Ptr(mRef.get()); }
    };

    class ItemPhysicsIntegration : public testing::Test
    {
    protected:
        const bool mOldEnabled = Settings::ragdoll().mItemPhysics;
        const int mOldThreads = Settings::physics().mAsyncNumThreads;
        ToUTF8::Utf8Encoder mEncoder{ ToUTF8::WINDOWS_1252 };
        std::unique_ptr<TestingOpenMW::VFSTestFile> mBox, mFloor, mWall;
        std::unique_ptr<VFS::Manager> mVfs;
        std::unique_ptr<Resource::ResourceSystem> mResources;
        std::unique_ptr<MWPhysics::PhysicsSystem> mPhysics;

        void SetUp() override
        {
            MWClass::registerClasses();
            Settings::ragdoll().mItemPhysics.set(true);
            Settings::physics().mAsyncNumThreads.set(0);
            mBox = std::make_unique<TestingOpenMW::VFSTestFile>(boxMesh(4, 4, 4));
            mFloor = std::make_unique<TestingOpenMW::VFSTestFile>(boxMesh(800, 800, 2));
            mWall = std::make_unique<TestingOpenMW::VFSTestFile>(boxMesh(2, 800, 800));
            mVfs = TestingOpenMW::createTestVFS({
                { VFS::Path::NormalizedView("meshes/box.osgt"), mBox.get() },
                { VFS::Path::NormalizedView("meshes/floor.osgt"), mFloor.get() },
                { VFS::Path::NormalizedView("meshes/wall.osgt"), mWall.get() },
            });
            mResources = std::make_unique<Resource::ResourceSystem>(mVfs.get(), 60, &mEncoder.getStatelessEncoder());
            auto* scene = mResources->getSceneManager();
            scene->setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
            auto defines = Shader::getDefaultDefines();
            for (const auto& [key, value] : SceneUtil::ShadowManager::getShadowsDisabledDefines())
                defines[key] = value;
            osg::ref_ptr<SceneUtil::LightManager> lights = new SceneUtil::LightManager(SceneUtil::LightSettings{});
            for (const auto& [key, value] : lights->getLightDefines())
                defines[key] = value;
            scene->getShaderManager().setGlobalDefines(defines);
            mPhysics = std::make_unique<MWPhysics::PhysicsSystem>(mResources.get(), new osg::Group);
        }
        void TearDown() override
        {
            mPhysics.reset();
            Settings::ragdoll().mItemPhysics.set(mOldEnabled);
            Settings::physics().mAsyncNumThreads.set(mOldThreads);
        }
        void insert(const MWWorld::Ptr& ptr, const std::string& mesh = "meshes/box.osgt")
        {
            ptr.getClass().insertObject(ptr, mesh, osg::Quat(), *mPhysics);
            mPhysics->updateScale(ptr); // Scene applies scale after insertion.
        }
        void advance(float seconds)
        {
            for (int i = 0; i < int(seconds * 120); ++i)
                mPhysics->updateItems(1.f / 120, [&](const MWWorld::Ptr& ptr, const osg::Vec3f& p, const osg::Quat& q) {
                    // The native World consumer writes the reference and sends
                    // these same transform notifications back to PhysicsSystem.
                    ESM::Position pos = ptr.getRefData().getPosition();
                    const auto angles = Misc::toEulerAnglesZYX(q);
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        pos.pos[axis] = p[axis];
                        pos.rot[axis] = angles[axis];
                    }
                    ptr.getRefData().setPosition(pos);
                    mPhysics->updateRotation(ptr, q);
                    mPhysics->updatePosition(ptr);
                });
        }
    };

    template <class Record>
    class ItemPhysicsRecords : public ItemPhysicsIntegration
    {
    };
    using ItemRecords = testing::Types<ESM::Apparatus, ESM::Armor, ESM::Book, ESM::Clothing, ESM::Ingredient,
        ESM::Light, ESM::Lockpick, ESM::Miscellaneous, ESM::Potion, ESM::Probe, ESM::Repair, ESM::Weapon>;
    TYPED_TEST_SUITE(ItemPhysicsRecords, ItemRecords);

    TYPED_TEST(ItemPhysicsRecords, SceneInsertionAndPostponedInsertionCreateHittableBodies)
    {
        Reference<TypeParam> item(0, 0, 20);
        const auto ptr = item.ptr();
        ASSERT_TRUE(ptr.getClass().isItem(ptr));
        this->insert(ptr);
        ASSERT_NE(this->mPhysics->getObject(ptr), nullptr);
        const auto hit = this->mPhysics->castRay(osg::Vec3f(-20, 0, 20), osg::Vec3f(20, 0, 20));
        EXPECT_TRUE(hit.mHit);
        EXPECT_EQ(hit.mHitObject, ptr);
        this->mPhysics->remove(ptr);

        ptr.mRef->mData.mPhysicsPostponed = true;
        this->insert(ptr);
        EXPECT_EQ(this->mPhysics->getObject(ptr), nullptr);
        ptr.mRef->mData.mPhysicsPostponed = false;
        ptr.getClass().insertObjectPhysics(ptr, "meshes/box.osgt", osg::Quat(), *this->mPhysics);
        EXPECT_NE(this->mPhysics->getObject(ptr), nullptr);
        this->mPhysics->remove(ptr);

        Settings::ragdoll().mItemPhysics.set(false);
        this->insert(ptr);
        EXPECT_EQ(this->mPhysics->getObject(ptr), nullptr);
    }

    TEST_F(ItemPhysicsIntegration, NativeImpulseMovesItemAndPreservesVelocityAcrossPoseUpdates)
    {
        Reference<ESM::Static> floor(0, 0, -1);
        Reference<ESM::Potion> item(0, 0, 40);
        insert(floor.ptr(), "meshes/floor.osgt");
        insert(item.ptr());
        advance(0.2f);
        EXPECT_FLOAT_EQ(item.ptr().getRefData().getPosition().pos[2], 40.f);
        mPhysics->applyItemImpulse(item.ptr(), osg::Vec3f(100, 0, 10), osg::Vec3f(0, 0, 41));
        advance(0.2f);
        EXPECT_GT(item.ptr().getRefData().getPosition().pos[0], 15.f);
        EXPECT_LT(item.ptr().getRefData().getPosition().pos[2], 35.f);
        advance(3.f);
        EXPECT_GT(item.ptr().getRefData().getPosition().pos[2], 1.f);
        EXPECT_LT(item.ptr().getRefData().getPosition().pos[2], 4.f);
        mPhysics->remove(item.ptr());
        mPhysics->remove(floor.ptr());
    }

    TEST_F(ItemPhysicsIntegration, AllImpulseSlidersReachTheNativeSolverAfterBaselineClamping)
    {
        Reference<ESM::Static> floor(0, 0, -1);
        insert(floor.ptr(), "meshes/floor.osgt");
        auto& settings = Settings::ragdoll();
        for (auto* slider : { &settings.mMeleeImpulseScale, &settings.mCrossbowImpulseScale,
                 &settings.mBowImpulseScale, &settings.mThrownImpulseScale, &settings.mMagicDistanceScale })
        {
            const float original = slider->get();
            for (const float scale : { .5f, 1.f, 3.f })
            {
                slider->set(scale);
                Reference<ESM::Potion> item(0, 0, 80, .05f);
                insert(item.ptr());
                mPhysics->applyItemImpulse(item.ptr(), osg::Vec3f(5000, 0, 0), osg::Vec3f(0, 0, 80), slider->get());
                EXPECT_NEAR(item.ptr().getRefData().getItemPhysicsState().mLinearVelocity[0], 1200 * scale, .01f);
                advance(1.f / 120);
                EXPECT_NEAR(item.ptr().getRefData().getPosition().pos[0], 10 * scale, .1f);
                mPhysics->remove(item.ptr());
            }
            slider->set(original);
        }
        mPhysics->remove(floor.ptr());
    }

    TEST_F(ItemPhysicsIntegration, NativeExplosionPushesRegisteredItemsOutwardAccordingToMass)
    {
        Reference<ESM::Static> floor(0, 0, -1);
        Reference<ESM::Potion> light(-20, 0, 40, 1);
        Reference<ESM::Armor> heavy(20, 0, 40, 20);
        insert(floor.ptr(), "meshes/floor.osgt");
        insert(light.ptr());
        insert(heavy.ptr());
        mPhysics->applyItemExplosion(osg::Vec3f(0, 0, 40), 100, 200);
        advance(0.1f);
        const float left = -20 - light.ptr().getRefData().getPosition().pos[0];
        const float right = heavy.ptr().getRefData().getPosition().pos[0] - 20;
        EXPECT_GT(left, 10.f);
        EXPECT_GT(right, 0.5f);
        EXPECT_NEAR(left / right, 20.f, 0.1f);
        mPhysics->remove(light.ptr());
        mPhysics->remove(heavy.ptr());
        mPhysics->remove(floor.ptr());
    }

    TEST_F(ItemPhysicsIntegration, NativeDropWakesItemAndPickupRemovesBothColliders)
    {
        Reference<ESM::Static> floor(0, 0, -1);
        Reference<ESM::Miscellaneous> item(0, 0, 80);
        insert(floor.ptr(), "meshes/floor.osgt");
        insert(item.ptr());
        mPhysics->applyItemImpulse(item.ptr(), osg::Vec3f(), osg::Vec3f(0, 0, 80));
        advance(0.2f);
        EXPECT_LT(item.ptr().getRefData().getPosition().pos[2], 70.f);
        mPhysics->remove(item.ptr());
        EXPECT_EQ(mPhysics->getObject(item.ptr()), nullptr);
        mPhysics->updateItems(0.1f, [](const auto&, const auto&, const auto&) { ADD_FAILURE() << "Ghost item pose"; });
        mPhysics->remove(floor.ptr());
    }

    TEST_F(ItemPhysicsIntegration, UnloadAndReloadPreserveTheRemainingWindowAndFinalPose)
    {
        Reference<ESM::Static> floor(0, 0, -1);
        Reference<ESM::Potion> item(0, 0, 100);
        insert(floor.ptr(), "meshes/floor.osgt");
        ESM::ItemPhysicsState saved;
        saved.mPresent = true;
        saved.mTimeLeft = 0.35f;
        saved.mLinearVelocity = { 100, 0, 50 };
        item.ptr().getRefData().setItemPhysicsState(saved);
        insert(item.ptr());
        advance(0.125f);
        const auto beforeUnload = item.ptr().getRefData().getItemPhysicsState();
        EXPECT_NEAR(beforeUnload.mTimeLeft, 0.225f, 0.001f);
        EXPECT_GT(item.ptr().getRefData().getPosition().pos[0], 10.f);
        mPhysics->remove(item.ptr());
        insert(item.ptr());
        EXPECT_EQ(item.ptr().getRefData().getItemPhysicsState().mTimeLeft, beforeUnload.mTimeLeft);
        advance(0.3f);
        EXPECT_EQ(item.ptr().getRefData().getItemPhysicsState().mTimeLeft, 0.f);
        const auto frozen = item.ptr().getRefData().getPosition();
        advance(0.3f);
        EXPECT_EQ(item.ptr().getRefData().getPosition(), frozen);
        mPhysics->remove(item.ptr());
        mPhysics->remove(floor.ptr());
    }

    TEST_F(ItemPhysicsIntegration, ProjectileChoosesBottleBeforeWallRegardlessOfCandidateOrder)
    {
        Reference<ESM::Static> wall(30, 0, 20);
        Reference<ESM::Potion> bottle(0, 0, 20);
        insert(wall.ptr());
        insert(bottle.ptr());
        for (bool farFirst : { false, true })
        {
            const auto id = mPhysics->addProjectile(
                {}, osg::Vec3f(-40, 0, 20), VFS::Path::NormalizedView("meshes/box.osgt"), false);
            auto* projectile = mPhysics->getProjectile(id);
            MWPhysics::ProjectileConvexCallback sweep(
                nullptr, nullptr, btVector3(-40, 0, 20), btVector3(40, 0, 20), *projectile);
            btCollisionWorld::LocalConvexResult far(mPhysics->getObject(wall.ptr())->getCollisionObject(), nullptr,
                btVector3(-1, 0, 0), btVector3(28, 0, 20), .85);
            btCollisionWorld::LocalConvexResult near(mPhysics->getObject(bottle.ptr())->getCollisionObject(), nullptr,
                btVector3(-1, 0, 0), btVector3(-2, 0, 20), .475);
            sweep.addSingleResult(farFirst ? far : near, true);
            EXPECT_TRUE(projectile->isActive());
            sweep.addSingleResult(farFirst ? near : far, true);
            sweep.commitHit();
            ASSERT_FALSE(projectile->isActive());
            EXPECT_EQ(projectile->getTarget(), bottle.ptr());
            EXPECT_NEAR(projectile->getHitPosition().x(), -2, .001);
            mPhysics->applyItemImpulse(projectile->getTarget(), osg::Vec3f(1000, 0, 0), osg::Vec3f(-2, 0, 20));
            EXPECT_GT(bottle.ptr().getRefData().getItemPhysicsState().mTimeLeft, 0);
            mPhysics->removeProjectile(id);
        }
        mPhysics->remove(bottle.ptr());
        mPhysics->remove(wall.ptr());
    }

    TEST_F(ItemPhysicsIntegration, MeleeFindsSmallOffAxisClutterButDoesNotHitThroughFurniture)
    {
        Reference<ESM::Potion> bottle(0, 3, 20);
        Reference<ESM::Static> wall(-10, 0, 20);
        insert(bottle.ptr());
        const osg::Vec3f from(-40, 0, 20), direction(1, 0, 0);
        EXPECT_FALSE(mPhysics->castRay(from, from + direction * 80).mHit);
        const auto hit = MWPhysics::itemMeleeContact(*mPhysics, from, direction, 80);
        ASSERT_EQ(hit.mHitObject, bottle.ptr());
        insert(wall.ptr(), "meshes/wall.osgt");
        EXPECT_FALSE(MWPhysics::itemMeleeContact(*mPhysics, from, direction, 80).mHit);
        mPhysics->remove(wall.ptr());
        mPhysics->remove(bottle.ptr());
    }

    TEST_F(ItemPhysicsIntegration, DownwardMeleeUsesBodyReachRatherThanEyeDistance)
    {
        Reference<ESM::Static> floor(0, 0, -1);
        Reference<ESM::Potion> bottle(50, 0, 4);
        Reference<ESM::Static> wall(25, 0, 60);
        insert(floor.ptr(), "meshes/floor.osgt");
        insert(bottle.ptr());
        const osg::Vec3f eye(0, 0, 120), center(0, 0, 60);
        const osg::Vec3f direction = osg::Vec3f(50, 0, 4) - eye;
        EXPECT_FALSE(MWPhysics::itemMeleeContact(*mPhysics, eye, direction, 100).mHit);
        const auto hit = MWPhysics::itemMeleeContact(*mPhysics, eye, direction, 100, {}, center);
        ASSERT_EQ(hit.mHitObject, bottle.ptr());
        insert(wall.ptr(), "meshes/wall.osgt");
        EXPECT_FALSE(MWPhysics::itemMeleeContact(*mPhysics, eye, direction, 100, {}, center).mHit);
        mPhysics->remove(wall.ptr());
        mPhysics->applyItemImpulse(hit.mHitObject, osg::Vec3f(500, 0, 100), hit.mHitPos);
        advance(.1f);
        EXPECT_GT(bottle.ptr().getRefData().getPosition().pos[0], 55);
        mPhysics->remove(bottle.ptr());
        mPhysics->remove(floor.ptr());
    }

    TEST_F(ItemPhysicsIntegration, CellUnloadDropsPoseMemoryAndTimersButRetainsReferencePlacement)
    {
        Reference<ESM::Potion> item(15, 25, 45);
        auto ptr = item.ptr();
        auto& ragdoll = ptr.getRefData().editRagdollState();
        ragdoll.mPresent = true;
        ragdoll.mBodies.resize(15);
        ragdoll.mBones.resize(60);
        ESM::ItemPhysicsState physics;
        physics.mPresent = true;
        physics.mTimeLeft = 7;
        ptr.getRefData().setItemPhysicsState(physics);
        const auto position = ptr.getRefData().getPosition();
        MWWorld::ListAndResetObjectsVisitor unload;
        unload(ptr);
        const auto& cleared = ptr.getRefData().getRagdollState();
        EXPECT_FALSE(cleared.mPresent);
        EXPECT_EQ(cleared.mBodies.capacity(), 0);
        EXPECT_EQ(cleared.mBones.capacity(), 0);
        EXPECT_FALSE(ptr.getRefData().getItemPhysicsState().mPresent);
        EXPECT_EQ(ptr.getRefData().getPosition(), position);
    }

    TEST_F(ItemPhysicsIntegration, RemovingLastItemReleasesWorldAndNextItemCanStillMove)
    {
        Reference<ESM::Static> floor(0, 0, -1);
        insert(floor.ptr(), "meshes/floor.osgt");
        for (int pass = 0; pass < 2; ++pass)
        {
            Reference<ESM::Potion> item(0, 0, 40);
            insert(item.ptr());
            mPhysics->applyItemImpulse(item.ptr(), osg::Vec3f(100, 0, 10), osg::Vec3f(0, 0, 40));
            advance(.1f);
            EXPECT_GT(item.ptr().getRefData().getPosition().pos[0], 5);
            mPhysics->remove(item.ptr());
        }
        mPhysics->remove(floor.ptr());
    }
    TYPED_TEST(ItemPhysicsRecords, DownwardMeleeHitsAtAllHeadingsAndMovesTheItem)
    {
        Reference<ESM::Static> floor(0, 0, -1);
        this->insert(floor.ptr(), "meshes/floor.osgt");
        // Actual spatial targets below an eye. The former ZYX model quaternion
        // looked horizontally east and upwards south despite the downward pitch.
        const osg::Vec3f eye(0, 0, 100);
        for (const auto target : { osg::Vec3f(0, 60, 20), osg::Vec3f(60, 0, 20), osg::Vec3f(0, -60, 20),
                 osg::Vec3f(-60, 0, 20), osg::Vec3f(42, 42, 20) })
        {
            Reference<TypeParam> item(target.x(), target.y(), target.z());
            this->insert(item.ptr());
            const float pitch = std::atan2(80.f, std::hypot(target.x(), target.y()));
            const float yaw = std::atan2(target.x(), target.y());
            const auto direction = MWPhysics::itemAimDirection(pitch, yaw);
            const auto hit = MWPhysics::itemMeleeContact(*this->mPhysics, eye, direction, 120);
            EXPECT_EQ(hit.mHitObject, item.ptr());
            if (hit.mHitObject == item.ptr())
            {
                this->mPhysics->applyItemImpulse(item.ptr(), osg::Vec3f(target.x(), target.y(), 10), hit.mHitPos);
                this->advance(.1f);
                EXPECT_GT(
                    (item.ptr().getRefData().getPosition().asVec3() - target) * osg::Vec3f(target.x(), target.y(), 0),
                    5);
            }
            this->mPhysics->remove(item.ptr());
        }
        this->mPhysics->remove(floor.ptr());
    }

    TEST_F(ItemPhysicsIntegration, FirstPersonMeleeUsesTheRenderedEyeNotTheAnimatedHead)
    {
        Reference<ESM::Potion> bottle(0, 60, 20);
        insert(bottle.ptr());
        const osg::Vec3f eye(30, 0, 100), center(0, 60, 20), head(0, 0, 120);
        const auto view = osg::Matrixf::lookAt(eye, center, osg::Vec3f(0, 0, 1));
        const auto [from, direction] = MWPhysics::itemViewRay(view);
        EXPECT_LT((from - eye).length(), .001f);
        EXPECT_EQ(MWPhysics::itemMeleeContact(*mPhysics, from, direction, 120).mHitObject, bottle.ptr());
        EXPECT_FALSE(MWPhysics::itemMeleeContact(*mPhysics, head, direction, 120).mHit);
        mPhysics->remove(bottle.ptr());
    }

    TEST_F(ItemPhysicsIntegration, LiveItemDurationChangesPreserveAge)
    {
        const float previous = Settings::ragdoll().mItemSimulationTime;
        Reference<ESM::Potion> item(0, 0, 1000);
        Settings::ragdoll().mItemSimulationTime.set(10);
        insert(item.ptr());
        mPhysics->applyItemImpulse(item.ptr(), osg::Vec3f(10, 0, 10), osg::Vec3f(0, 0, 1000));
        advance(.1f);
        Settings::ragdoll().mItemSimulationTime.set(20);
        advance(.1f);
        EXPECT_NEAR(item.ptr().getRefData().getItemPhysicsState().mTimeLeft, 19.8f, .02f);
        Settings::ragdoll().mItemSimulationTime.set(5);
        advance(.1f);
        EXPECT_NEAR(item.ptr().getRefData().getItemPhysicsState().mTimeLeft, 4.7f, .02f);
        mPhysics->remove(item.ptr());
        Settings::ragdoll().mItemSimulationTime.set(previous);
    }

    TEST_F(ItemPhysicsIntegration, WeaponGroupsHaveIndependentTunableImpulses)
    {
        auto& settings = Settings::ragdoll();
        Reference<ESM::Weapon> weapon(0, 0, 0), ammo(0, 0, 0);
        for (const auto type :
            { ESM::Weapon::Bolt, ESM::Weapon::Arrow, ESM::Weapon::MarksmanThrown, ESM::Weapon::SpearTwoWide })
        {
            weapon.mBase.mData.mType = type;
            ammo.mBase.mData.mType = type;
            auto& option = type == ESM::Weapon::Bolt  ? settings.mCrossbowImpulseScale
                : type == ESM::Weapon::Arrow          ? settings.mBowImpulseScale
                : type == ESM::Weapon::MarksmanThrown ? settings.mThrownImpulseScale
                                                      : settings.mMeleeImpulseScale;
            const float old = option;
            option.set(.5f);
            const auto low = MWMechanics::makeRagdollHit({}, {}, weapon.ptr(),
                type == ESM::Weapon::SpearTwoWide ? MWWorld::Ptr() : ammo.ptr(), 0, {}, { 0, 1, 0 });
            option.set(3.f);
            const auto high = MWMechanics::makeRagdollHit({}, {}, weapon.ptr(),
                type == ESM::Weapon::SpearTwoWide ? MWWorld::Ptr() : ammo.ptr(), 0, {}, { 0, 1, 0 });
            EXPECT_GT(high->mWeapon.mWeaponDistance, 0);
            EXPECT_FLOAT_EQ(high->mWeapon.mWeaponDistance, low->mWeapon.mWeaponDistance);
            EXPECT_FLOAT_EQ(low->mWeapon.mWeaponImpulseScale, .5f);
            EXPECT_FLOAT_EQ(high->mWeapon.mWeaponImpulseScale, 3.f);
            EXPECT_FLOAT_EQ(high->mWeapon.mWeaponImpulseScale, low->mWeapon.mWeaponImpulseScale * 6);
            EXPECT_FLOAT_EQ(high->mWeapon.mWeaponMultiplier, low->mWeapon.mWeaponMultiplier);
            EXPECT_EQ(high->mWeapon.mCrossbow, type == ESM::Weapon::Bolt);
            option.set(old);
        }
        EXPECT_FLOAT_EQ(settings.mCrossbowDistance, 2.4f);
        EXPECT_FLOAT_EQ(settings.mBowDistance, .3f);
        EXPECT_FLOAT_EQ(settings.mSpearDistance, .3f);
        EXPECT_FLOAT_EQ(settings.mPinMaxDistance, 2.5f);
    }

    TEST_F(ItemPhysicsIntegration, NativeWeaponClassesAndAnimationKeysSelectOnlyEligibleWounds)
    {
        using Attack = Ragdoll::AttackType;
        using Weapon = Ragdoll::WoundWeapon;
        Reference<ESM::Weapon> weapon(0, 0, 0);
        for (const auto& [type, group] : {
                 std::pair(ESM::Weapon::LongBladeOneHand, Weapon::Blade),
                 std::pair(ESM::Weapon::LongBladeTwoHand, Weapon::HeavyBlade),
                 std::pair(ESM::Weapon::AxeOneHand, Weapon::HeavyBlade),
                 std::pair(ESM::Weapon::AxeTwoHand, Weapon::HeavyBlade),
                 std::pair(ESM::Weapon::ShortBladeOneHand, Weapon::None),
                 std::pair(ESM::Weapon::SpearTwoWide, Weapon::None),
                 std::pair(ESM::Weapon::BluntOneHand, Weapon::Blunt),
                 std::pair(ESM::Weapon::BluntTwoClose, Weapon::Blunt),
                 std::pair(ESM::Weapon::BluntTwoWide, Weapon::Blunt) })
        {
            weapon.mBase.mData.mType = type;
            for (const auto& [key, attack] : { std::pair(ESM::Weapon::AT_Chop, Attack::Chop),
                     std::pair(ESM::Weapon::AT_Slash, Attack::Slash), std::pair(ESM::Weapon::AT_Thrust, Attack::Thrust) })
            {
                auto contact = MWMechanics::makeRagdollHit({}, {}, weapon.ptr(), {}, key, {}, { 0, 1, 0 });
                EXPECT_EQ(contact->mWeapon.mWoundWeapon, group);
                EXPECT_EQ(contact->mWeapon.mAttackType, attack);
                contact->mWeapon.mWeaponHitBody = 2;
                contact->applyWeapon();
                const auto& hit = contact->mApplied;
                EXPECT_EQ(hit.mWeaponHitBody, 2);
                EXPECT_EQ(hit.mAttackType, attack);
                const auto wound = Ragdoll::selectWounds(hit, hit.mWeaponHitBody, 0, 1);
                const bool cutting = group == Weapon::Blade || group == Weapon::HeavyBlade;
                const bool eligible = (cutting || group == Weapon::Blunt) && attack != Attack::Thrust;
                const bool longitudinal = hit.mTwoHandedCutting && attack == Attack::Chop;
                EXPECT_EQ(wound.mLongitudinal, longitudinal);
                EXPECT_EQ(wound.mCuts, eligible && !longitudinal ? 1u << 2 : 0);
                EXPECT_EQ(wound.mBurstHead, eligible && group == Weapon::Blunt);
                EXPECT_FALSE(wound.cuts(1));
                contact->mWeapon.mWeaponHitBody = 1;
                contact->applyWeapon();
                const auto torso = Ragdoll::selectWounds(contact->mApplied, 1, 0, 1);
                EXPECT_EQ(torso.cuts(1), (group == Weapon::HeavyBlade && attack == Attack::Slash)
                    || (hit.mTwoHandedHammer && attack != Attack::Thrust));
                EXPECT_EQ(torso.mLongitudinal, longitudinal);
                EXPECT_EQ(torso.mCrushed, hit.mTwoHandedHammer && attack != Attack::Thrust);
                EXPECT_FALSE(torso.mBurstHead);
            }
        }
    }

    TEST_F(ItemPhysicsIntegration, PrimedEnchantmentsMergeWithTheNativeDeathHitInBothOrders)
    {
        MWBase::Environment environment;
        MWWorld::ESMStore store;
        environment.setESMStore(store);
        ESM::NPC base;
        base.blank();
        const auto* npc = store.insert(base);
        Reference<ESM::Weapon> blade(0, 0, 0), bow(0, 0, 0), bolt(0, 0, 0);
        blade.mBase.mData.mType = ESM::Weapon::LongBladeOneHand;
        bow.mBase.mData.mType = ESM::Weapon::MarksmanCrossbow;
        bolt.mBase.mData.mType = ESM::Weapon::Bolt;
        for (bool ranged : { false, true })
        for (bool weaponFirst : { false, true })
        for (auto element : { Ragdoll::DeathElement::Fire, Ragdoll::DeathElement::Frost,
                 Ragdoll::DeathElement::Shock, Ragdoll::DeathElement::Poison })
        {
            MWMechanics::CreatureStats stats;
            stats.getSpells().setSpells(npc->mId);
            ESM::CreatureStats saved{};
            saved.blank();
            saved.mDead = true;
            stats.readState(saved); // Native dead stats without assets or a World timestamp mock.
            auto hit = MWMechanics::makeRagdollHit({}, {}, ranged ? bow.ptr() : blade.ptr(),
                ranged ? bolt.ptr() : MWWorld::Ptr(), ESM::Weapon::AT_Chop, {}, { 0, 1, 0 });
            hit->mWeapon.mWeaponHitBody = 4;
            hit->mAwaitingMagic = true;
            const auto id = stats.queueRagdollHit(hit);
            MWMechanics::RagdollMagicImpact magic;
            magic.mHit = hit;
            magic.mRadius = 4;
            magic.mDirection.set(1, 0, 0);
            ESM::ActiveEffect effect{};
            effect.mDuration = effect.mTimeLeft = 2;
            effect.mFlags = ESM::ActiveEffect::Flag_Applied | ESM::ActiveEffect::Flag_Remove;
            hit->beginMagicTick(0); // updateMagicEffects(victim) primes before Lua hit action.
            const auto physical = [&] {
                auto queued = stats.takeRagdollHit(id);
                ASSERT_EQ(queued, hit);
                queued->mAwaitingWeapon = false;
                queued->applyWeapon();
                stats.recordRagdollDeath(queued);
            };
            if (weaponFirst)
            {
                physical();
                ASSERT_TRUE(stats.isRagdollImpactPending()) << "Renderer would create a plain corpse before enchantment damage";
            }
            hit->beginMagicTick(1.f / 60);
            hit->applyMagic(1, MWMechanics::makeRagdollMagicImpact(magic,
                osg::Vec3f(Constants::UnitsPerMeter, 0, 0), 1, element, 5));
            stats.recordRagdollDeath(hit);
            if (!weaponFirst)
            {
                ASSERT_TRUE(stats.isRagdollImpactPending());
                physical();
            }
            const auto& death = stats.getRagdollImpact();
            EXPECT_FALSE(stats.isRagdollImpactPending());
            EXPECT_EQ(death.mDeathElement, element);
            EXPECT_EQ(death.mCrossbow, ranged);
            EXPECT_TRUE(death.mDirectMagic);
            EXPECT_GT(death.mMagicDistance, 0);
            const auto wound = Ragdoll::selectWounds(death, death.mWeaponHitBody, 0, 1);
            const unsigned expected = element == Ragdoll::DeathElement::Fire || element == Ragdoll::DeathElement::Frost ? 684
                : element == Ragdoll::DeathElement::Shock ? 4 : ranged ? 0 : 8;
            EXPECT_EQ(wound.mCuts, expected);
            EXPECT_EQ(wound.mBurstHead, element == Ragdoll::DeathElement::Shock);
        }
    }
}
