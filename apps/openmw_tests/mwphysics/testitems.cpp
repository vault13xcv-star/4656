#include "apps/openmw/mwclass/classes.hpp"
#include "apps/openmw/mwmechanics/combat.hpp"
#include "apps/openmw/mwmechanics/ragdollimpact.hpp"
#include "apps/openmw/mwphysics/itemhit.hpp"
#include "apps/openmw/mwphysics/object.hpp"
#include "apps/openmw/mwphysics/physicssystem.hpp"
#include "apps/openmw/mwphysics/projectile.hpp"
#include "apps/openmw/mwphysics/projectileconvexcallback.hpp"
#include "apps/openmw/mwworld/cellvisitors.hpp"
#include "apps/openmw/mwworld/class.hpp"
#include "apps/openmw/mwworld/livecellref.hpp"

#include <components/esm3/loadalch.hpp>
#include <components/esm3/loadappa.hpp>
#include <components/esm3/loadarmo.hpp>
#include <components/esm3/loadbook.hpp>
#include <components/esm3/loadclot.hpp>
#include <components/esm3/loadingr.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/esm3/loadlock.hpp>
#include <components/esm3/loadmisc.hpp>
#include <components/esm3/loadprob.hpp>
#include <components/esm3/loadrepa.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/esm3/loadweap.hpp>
#include <components/misc/mathutil.hpp>
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
            option.set(2.f);
            const auto high = MWMechanics::makeRagdollHit({}, {}, weapon.ptr(),
                type == ESM::Weapon::SpearTwoWide ? MWWorld::Ptr() : ammo.ptr(), 0, {}, { 0, 1, 0 });
            EXPECT_GT(high->mWeapon.mWeaponDistance, 0);
            EXPECT_FLOAT_EQ(high->mWeapon.mWeaponDistance, low->mWeapon.mWeaponDistance);
            EXPECT_FLOAT_EQ(high->mWeapon.mWeaponMultiplier, low->mWeapon.mWeaponMultiplier * 4);
            EXPECT_EQ(high->mWeapon.mCrossbow, type == ESM::Weapon::Bolt);
            option.set(old);
        }
        EXPECT_FLOAT_EQ(settings.mCrossbowDistance, 2.4f);
        EXPECT_FLOAT_EQ(settings.mBowDistance, .3f);
        EXPECT_FLOAT_EQ(settings.mSpearDistance, .3f);
        EXPECT_FLOAT_EQ(settings.mPinMaxDistance, 2.5f);
    }
}
