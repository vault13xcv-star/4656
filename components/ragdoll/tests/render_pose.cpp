#include <apps/openmw/mwrender/ragdoll.hpp>
#include <apps/openmw/mwrender/ragdollappearance.hpp>
#include <apps/openmw/mwrender/brutalgore.hpp>
#include <apps/openmw/mwrender/brutalblood.hpp>
#include <apps/openmw/mwrender/bloodsurfacevisitor.hpp>
#include <apps/openmw/mwrender/vismask.hpp>
#include <apps/openmw/mwmechanics/ragdollimpact.hpp>
#include <btBulletDynamicsCommon.h>
#include <chrono>
#include <components/esm3/ragdollstate.hpp>
#include <components/ragdoll/items.hpp>
#include <components/ragdoll/blood.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/hitlocation.hpp>

#include <osg/FrameStamp>
#include <osg/Billboard>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/Texture2D>
#include <osg/ShapeDrawable>
#include <osgUtil/UpdateVisitor>
#include <osgUtil/CullVisitor>
#include <osgUtil/LineSegmentIntersector>

#include <array>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <utility>
#include <tuple>

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

    void brutalBloodAndPersistence()
    {
        const auto ray = [](const osg::Vec3f& a, const osg::Vec3f& b, osg::Vec3f& p, osg::Vec3f& n) {
            float best = 2;
            if (a.z() >= 0 && b.z() <= 0 && a.z() != b.z()) { best = a.z() / (a.z() - b.z()); n = { 0, 0, 1 }; }
            if ((a.x() - 35) * (b.x() - 35) <= 0 && a.x() != b.x())
            {
                const float t = (35 - a.x()) / (b.x() - a.x());
                if (t >= 0 && t <= 1 && t < best) { best = t; n = { -1, 0, 0 }; }
            }
            if (best > 1) return false;
            p = a + (b - a) * best; return true;
        };
        std::vector<MWRender::BloodStamp> stamps;
        const auto actualRay = MWRender::BloodTrail::collisionRaycast(floorSnapshot({}, 100));
        osg::Vec3f realPoint, realNormal;
        require(actualRay && actualRay({ 0, 0, 5 }, { 0, 0, -5 }, realPoint, realNormal)
            && std::abs(realPoint.z()) < .001f && realNormal.z() > .9f, "Blood collision BVH missed a real floor");
        osg::ref_ptr<osg::Group> root = new osg::Group;
        MWRender::BloodTrail spray(*root, ray, [&](const auto& stamp) { stamps.push_back(stamp); });
        const std::vector<MWRender::BloodTrail::Source> sources{ { { 0, 0, 20 }, { 1, 0, 1 }, 1, true },
            { { -20, 0, 10 }, { 0, 0, -1 }, 1, false } };
        auto* dropletMesh = root->getChild(0)->asGroup()->getChild(0)->asGeode()->getDrawable(0)->asGeometry();
        osg::ref_ptr<osg::Array> dropletVertices = dropletMesh->getVertexArray();
        osg::ref_ptr<osg::PrimitiveSet> dropletTriangles = dropletMesh->getPrimitiveSet(0);
        for (int frame = 0; frame < 240; ++frame) spray.update(1.f / 60, frame < 120 ? sources : std::vector<MWRender::BloodTrail::Source>{}, 1, frame / 60.f);
        require(dropletMesh->getVertexArray() == dropletVertices && dropletMesh->getPrimitiveSet(0) == dropletTriangles,
            "Ballistic droplets replaced their arrays/primitive every frame");
        require(!stamps.empty(), "Ballistic blood never reached a surface");
        bool wall = false, floor = false;
        for (const auto& stamp : stamps)
        {
            wall |= stamp.mNormal.x() < -.5f; floor |= stamp.mNormal.z() > .5f;
            for (const auto& v : stamp.mTriangles)
                require(std::abs((v - stamp.mCenter) * stamp.mNormal - .06f) < .01f, "Blood projected through a surface/corner");
        }
        require(wall && floor, "Blood did not stain both wall and floor");
        std::size_t low = 0, high = 0;
        for (float amount : { 0.f, .5f, 3.f })
        {
            MWRender::BloodTrail trial(*root, ray, {});
            for (int frame = 0; frame < 60; ++frame) trial.update(1.f / 60, sources, amount, frame / 60.f);
            if (amount == 0) require(trial.emittedCount() == 0, "Blood slider zero still emitted blood");
            else if (amount == .5f) low = trial.emittedCount(); else high = trial.emittedCount();
        }
        require(high >= low * 5 && low > 0, "Blood amount slider did not change actual emission");
        std::array<float, 2> peak{};
        for (int neck = 0; neck < 2; ++neck)
        {
            MWRender::BloodTrail fountain(*root, [&](const auto& a, const auto& b, auto&, auto&) {
                peak[neck] = std::max({ peak[neck], a.z(), b.z() }); return false;
            }, {});
            const std::vector<MWRender::BloodTrail::Source> up{ { { 0, 0, 20 }, { 0, 0, 1 }, 1, neck != 0 } };
            for (int frame = 0; frame < 120; ++frame) fountain.update(1.f / 60, up, 1, frame / 60.f);
        }
        require(peak[1] > peak[0] + 35, "Severed neck lacks a physically higher-pressure fountain");
        MWRender::BloodDecals decals(*root);
        decals.setCellActive("interior:a", true);
        decals.add("interior:a", stamps.front());
        decals.update(599, 600);
        require(decals.size() == 1, "Blood vanished before ten minutes");
        const auto record = decals.getSaveRecord();
        {
            auto overlapping = record;
            overlapping.mStains.front().mPool = true;
            overlapping.mStains.push_back(overlapping.mStains.front());
            overlapping.mStains.back().mAge = 500;
            MWRender::BloodDecals restored(*root);
            restored.loadSaveRecord(overlapping);
            require(restored.size() == 2 && restored.saveState()[0].mAge == 599 && restored.saveState()[1].mAge == 500,
                "Restoring overlapping pools merged them or renewed the ten-minute timer");
        }
        decals.clear(); decals.setCellActive("interior:a", true); decals.loadSaveRecord(record);
        require(decals.size() == 1 && std::abs(decals.saveState().front().mAge - 599) < .01f, "Blood save/load renewed or lost its age");
        decals.setCellActive("interior:a", false);
        decals.setCellActive("interior:b", true);
        decals.update(.5f, 600);
        for (unsigned i = 0; i < root->getNumChildren(); ++i)
            if (root->getChild(i)->getName() == "Brutal Morrowind blood stains")
                require(root->getChild(i)->getNodeMask() == 0, "Blood leaked between overlapping interior coordinates");
        decals.update(.5f, 600);
        require(decals.size() == 0, "Blood persisted past its configured lifetime");
        std::cout << "PASS ballistic neck spray, wall/floor decals, amount 0/50/300%, ten-minute lifetime, cell isolation and saved age\n";
    }

    void bloodReceiversAndSprites()
    {
        using Trail = MWRender::BloodTrail;
        // A collision-free carpet sits above a coarse floor collider. Actual
        // interior receivers must hit the carpet and individual stair treads.
        osg::ref_ptr<osg::Group> world = new osg::Group;
        world->setNodeMask(MWRender::Mask_Scene);
        const auto addMesh = [&](const std::vector<osg::Vec3f>& triangles, unsigned mask, bool carpet = false) {
            osg::ref_ptr<osg::Group> object = new osg::Group;
            object->setNodeMask(mask);
            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            if (carpet) geode->setNodeMask(MWRender::Mask_Effect);
            osg::ref_ptr<osg::Geometry> mesh = new osg::Geometry;
            mesh->setVertexArray(new osg::Vec3Array(triangles.begin(), triangles.end()));
            mesh->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, triangles.size()));
            geode->addDrawable(mesh);
            object->addChild(geode);
            world->addChild(object);
        };
        const auto quad = [](float x0, float x1, float z) {
            return std::vector<osg::Vec3f>{ {x0,-20,z}, {x1,-20,z}, {x1,20,z},
                {x0,-20,z}, {x1,20,z}, {x0,20,z} };
        };
        addMesh(quad(-100, 100, 0), MWRender::Mask_Static);
        addMesh(quad(-10, 10, 2), MWRender::Mask_Object, true);
        addMesh(quad(-10, 10, 18), MWRender::Mask_Actor);
        addMesh(quad(-10, 10, 19), MWRender::Mask_Effect);
        addMesh(quad(20, 30, 5), MWRender::Mask_Static);
        addMesh(quad(30, 40, 10), MWRender::Mask_Static);
        std::vector<osg::Vec3f> cylinder;
        for (int i = 0; i < 16; ++i)
        {
            const float a = i * 6.2831853f / 16, b = (i+1) * 6.2831853f / 16;
            const osg::Vec3f p(60+10*std::cos(a), 10*std::sin(a), 0);
            const osg::Vec3f q(60+10*std::cos(b), 10*std::sin(b), 0);
            const osg::Vec3f height(0,0,20);
            cylinder.insert(cylinder.end(), {p,q,q+height,p,q+height,p+height});
        }
        addMesh(cylinder, MWRender::Mask_Static);
        const auto collected = MWRender::collectBloodSurfaceTriangles(*world, {30,0,5}, 100, {}, nullptr, false);
        const auto surface = Trail::visualSurface(collected);
        require(bool(surface), "Interior visual receiver snapshot is empty");
        osg::Vec3f point, normal;
        require(surface.mRaycast({0,0,25}, {0,0,-5}, point, normal) && std::abs(point.z()-2) < .001f,
            "Carpet was excluded, or actors/transient effects received world blood");
        for (const auto& step : std::array<osg::Vec2f,2>{osg::Vec2f(25,5), osg::Vec2f(35,10)})
        {
            require(surface.mRaycast({step.x(),0,25}, {step.x(),0,-5}, point, normal)
                    && std::abs(point.z()-step.y()) < .001f,
                "Interior blood used a coarse stair collider instead of visible treads");
            const auto projected = surface.mProjector(point, normal, 2);
            require(!projected.empty(), "Interior stair projection left no blood triangles");
            for (const auto& v : projected)
                require(std::abs(v.z()-step.y()-.03f) < .002f, "Blood bridged separate stair treads");
        }
        require(surface.mRaycast({90,0,10}, {60,0,10}, point, normal)
                && std::abs(point.x()-70) < .01f,
            "Visible cylinder receiver was not captured");
        const auto curved = surface.mProjector(point, normal, 7);
        require(curved.size() >= 6, "Blood did not wrap a visible column");
        float minX = 100, maxX = -100;
        for (const auto& v : curved) { minX = std::min(minX,v.x()); maxX = std::max(maxX,v.x()); }
        require(maxX-minX > .5f, "Interior column blood was projected onto a flat invisible box");

        // Pools deliberately keep the original physics probe even when spray
        // splatters are projected onto a higher visual carpet.
        osg::ref_ptr<osg::Group> particles = new osg::Group;
        std::vector<MWRender::BloodStamp> deposits;
        bool ready = true;
        Trail spray(*particles, surface.mRaycast, [&](const auto& stamp) { deposits.push_back(stamp); },
            surface.mProjector, Trail::collisionRaycast(floorSnapshot({},100)), [&] { return ready; });
        const std::vector<Trail::Source> source{ {{0,0,8},{0,0,-1},1,false} };
        for (int i=0; i<60; ++i) spray.update(1.f/60,source,1,i/60.f);
        bool pool = false, splatter = false;
        for (const auto& stamp : deposits)
        {
            if (stamp.mPool)
            {
                pool = true;
                require(std::abs(stamp.mCenter.z()) < .001f && stamp.mRadius <= 8.01f,
                    "Stable pool probe/radius changed with interior visual projection");
            }
            else
            {
                splatter = true;
                require(std::abs(stamp.mCenter.z()-2) < .001f || std::abs(stamp.mCenter.z()) < .001f,
                    "Flying drop did not leave blood at its actual receiver");
            }
        }
        require(pool && splatter, "Pool/splatter receiver separation produced no blood");

        // Snapshot invalidation must not silently let particles fall through.
        deposits.clear(); ready = false;
        spray.update(.05f, source, 1, 1);
        deposits.clear();
        const auto before = spray.particleCount();
        for (int i=0;i<12;++i) spray.update(.05f,{},1,1);
        require(spray.particleCount() == before && deposits.empty(),
            "Invalidated surface allowed blood to pass through an unavailable receiver");
        ready = true;
        for (int i=0;i<100;++i) spray.update(.05f,{},1,1);
        require(spray.particleCount() == 0 && !deposits.empty(), "Refreshed blood receiver lost paused drops");

        std::array<std::size_t,2> counts{};
        for (int i=0;i<2;++i)
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            Trail trial(*root, {}, {});
            for (int frame=0;frame<8;++frame) trial.update(.0625f,source,i ? 1.f : .5f,frame*.0625f);
            counts[i] = trial.emittedCount();
            auto* mesh = root->getChild(0)->asGroup()->getChild(0)->asGeode()->getDrawable(0)->asGeometry();
            require(mesh->getVertexArray()->getNumElements() == trial.particleCount()*4
                    && mesh->getPrimitiveSet(0)->getNumIndices() == trial.particleCount()*6,
                "Flying blood reverted to volumetric geometry instead of one sprite per drop");
        }
        require(counts[0] == 7 && counts[1] == 14, "Default sprite emission is not twice the stable baseline");
        require(Ragdoll::bloodLaunchScale(3) > Ragdoll::bloodLaunchScale(1)
                && Ragdoll::bloodSpreadScale(3) > Ragdoll::bloodSpreadScale(1),
            "Blood quantity no longer increases launch distance and spread");
        std::cout << "PASS visible carpet/stairs/column, actor exclusion, stable collision pools, receiver invalidation, sprite geometry and doubled emission\n";
    }

    void organicBloodRules()
    {
        for (const auto* model : {"rat.nif", "nix_hound.nif", "guar.nif", "scamp.nif", "zombie.nif", "ash_ghoul.nif"})
            require(Ragdoll::creatureHasBlood("generated42", "", model), "Organic creature cannot bleed");
        for (const auto* model : {"steam_centurion.nif", "dwarven_sphere.nif", "skeleton_warrior.nif",
            "ancestor_ghost.nif", "bone_lord.nif", "storm_atronach.nif", "verminous_fabricant.nif"})
            require(!Ragdoll::creatureHasBlood("generated42", "", model), "Nonflesh creature emits blood");
        require(!Ragdoll::creatureHasBlood("generated42", "skeleton2", "custom.nif"),
            "Generated nonflesh creature lost its original-record exclusion");
        Ragdoll::DeathBloodTrigger trigger;
        require(!trigger.update(true) && !trigger.update(true), "Loaded corpse started a new death fountain");
        require(!trigger.update(false) && trigger.update(true) && !trigger.update(true),
            "Fresh death failed to emit exactly one fountain");
        require(!trigger.update(false) && trigger.update(true), "Resurrected creature cannot bleed on its next death");
        std::cout << "PASS organic creature classification and fresh-death fountain lifecycle\n";
    }

    void brutalMeshClipAndPose()
    {
        SkeletonFixture fixture;
        auto* bone = fixture.mSkeleton->getBone("Bip01 Spine2");
        fixture.mSkeleton->updateBoneMatrices(1);
        const auto bind = bone->mMatrixInSkeletonSpace;
        osg::ref_ptr<osg::Geometry> source = new osg::Geometry;
        source->setName("Brutal cut fixture");
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec2Array> uv = new osg::Vec2Array;
        for (const auto& p : { osg::Vec3f(-12, 0, -12), osg::Vec3f(12, 0, -12), osg::Vec3f(12, 0, 12), osg::Vec3f(-12, 0, 12) })
        {
            vertices->push_back(p * bind); uv->push_back({ (p.x() + 12) / 24.f, (p.z() + 12) / 24.f });
        }
        source->setVertexArray(vertices); source->setTexCoordArray(0, uv);
        source->addPrimitiveSet(new osg::DrawArrays(GL_QUADS, 0, 4));
        osg::ref_ptr<SceneUtil::RigGeometry> rig = new SceneUtil::RigGeometry;
        rig->setSourceGeometry(source);
        rig->setBoneInfo({ { "Bip01 Spine2", {}, osg::Matrixf::inverse(bind) } });
        rig->setInfluences({ { { 0, 1 } }, { { 0, 1 } }, { { 0, 1 } }, { { 0, 1 } } });
        fixture.mSkeleton->addChild(rig);
        auto* chest = fixture.mBones.at("Spine2").get();
        chest->setMatrix(chest->getMatrix() * osg::Matrixf::translate(4, 0, 3));
        const auto world = fixture.mWorld->getMatrix();
        const auto center = osg::Vec3f(4, 0, 3) + bind.getTrans();
        const auto normal = osg::Matrixf::transform3x3(osg::Vec3f(1, 0, 0), world);
        const auto halves = MWRender::splitGoreMesh(*rig, center * world, normal);
        struct Inspect : osg::NodeVisitor
        {
            std::vector<osg::ref_ptr<osg::Geometry>> meshes;
            Inspect() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
            void apply(osg::Geometry& g) override { if (g.getName() == "Brutal cut fixture") meshes.push_back(&g); }
        };
        for (unsigned side = 0; side < 2; ++side)
        {
            Inspect inspect; halves[side]->accept(inspect);
            require(!inspect.meshes.empty(), "Longitudinal slice produced no actual mesh");
            bool seam = false;
            for (const auto& mesh : inspect.meshes)
            {
                const auto* p = dynamic_cast<const osg::Vec3Array*>(mesh->getVertexArray());
                const auto* t = dynamic_cast<const osg::Vec2Array*>(mesh->getTexCoordArray(0));
                require(p && t && p->size() == t->size(), "Clipping discarded clothing UVs");
                for (unsigned i = 0; i < p->size(); ++i)
                {
                    const auto local = (*p)[i] * osg::Matrixf::inverse(world);
                    require(side ? local.x() >= 3.999f : local.x() <= 4.001f, "Slice retained triangles on the wrong side");
                    if (std::abs(local.x() - 4) < .001f) { seam = true; require(std::abs((*t)[i].x() - .5f) < .001f, "Cut stretched the texture at the seam"); }
                }
            }
            require(seam, "Cut did not interpolate real triangles at the plane");
        }
        require(std::abs(vertices->front().x() + 12) < .001f && uv->front().x() == 0, "Cut mutated a cached/shared NPC mesh");
        osg::ref_ptr<SceneUtil::MorphGeometry> morph = new SceneUtil::MorphGeometry;
        morph->setSourceGeometry(source);
        morph->addMorphTarget(vertices);
        osg::ref_ptr<osg::Vec3Array> offsets = new osg::Vec3Array;
        offsets->assign(4, osg::Vec3f(2, 0, 0));
        morph->addMorphTarget(offsets, .5f);
        fixture.mSkeleton->addChild(morph);
        const auto faceHalves = MWRender::splitGoreMesh(*morph, (bind.getTrans() + osg::Vec3f(1, 0, 0)) * world, normal);
        for (const auto& half : faceHalves)
        {
            Inspect inspect; half->accept(inspect);
            require(!inspect.meshes.empty(), "Animated NPC head morphs were not sliced");
            bool seam = false;
            for (const auto& mesh : inspect.meshes)
            {
                const auto* p = static_cast<const osg::Vec3Array*>(mesh->getVertexArray());
                const auto* t = static_cast<const osg::Vec2Array*>(mesh->getTexCoordArray(0));
                for (unsigned i = 0; i < p->size(); ++i)
                    if (std::abs(((*p)[i] * osg::Matrixf::inverse(world)).x() - 1) < .001f)
                    {
                        seam = true;
                        require(std::abs((*t)[i].x() - .5f) < .001f, "Head split ignored the current facial morph");
                    }
            }
            require(seam, "Head morph split did not reach the central seam");
        }
        std::cout << "PASS posed skinned mesh clipping, rotated/scaled NPC, actual triangle halves, seam UVs and immutable source\n";
    }

    void brutalPhysicalWounds()
    {
        for (int kind : { 0, 1, 2 })
        {
            const bool longitudinal = kind == 1, crushing = kind == 0;
            SkeletonFixture fixture;
            MWRender::RagdollController::Settings config;
            config.mNpc = true; config.mWoundRoll = 0; config.mWoundChance = 1; config.mMaxSimulationTime = 3;
            int deposits = 0;
            config.mBloodAmount = .5f;
            config.mBloodDeposit = [&](const auto&) { ++deposits; };
            Ragdoll::Impact hit;
            hit.mValid = hit.mHasWeaponContact = true;
            hit.mAttackType = kind == 2 ? Ragdoll::AttackType::Slash : Ragdoll::AttackType::Chop;
            hit.mWoundWeapon = crushing ? Ragdoll::WoundWeapon::Blunt : Ragdoll::WoundWeapon::HeavyBlade;
            hit.mTwoHandedCutting = longitudinal; hit.mTwoHandedHammer = crushing;
            hit.mWeaponHitBody = longitudinal ? 2 : 1; hit.mWeaponDirection.set(0, 1, kind == 2 ? 0.f : -.45f);
            auto corpse = MWRender::RagdollController::create(*fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, config, hit);
            require(bool(corpse), "Brutal wound failed to create real Bullet bodies");
            osg::ref_ptr<osg::Geode> torso = new osg::Geode;
            if (longitudinal)
            {
                osg::ref_ptr<osg::Geometry> mesh = new osg::Geometry;
                const osg::Vec3f v[] = { { -14, 0, -14 }, { 14, 0, -14 }, { 14, 0, 14 }, { -14, 0, 14 } };
                mesh->setVertexArray(new osg::Vec3Array(4, v)); mesh->addPrimitiveSet(new osg::DrawArrays(GL_QUADS, 0, 4));
                torso->addDrawable(mesh); fixture.mBones.at("Spine2")->addChild(torso);
                corpse->splitPart(*torso, 1); corpse->setSkinParts({ torso });
                require(torso->getNodeMask() == 0, "Original whole torso overlays the longitudinal halves");
            }
            ESM::RagdollState saved;
            corpse->writeState(saved);
            require(saved.mLongitudinal == longitudinal && saved.mCrushed == crushing, "Brutal wound type lost");
            require(saved.mGutsCount == 36 && saved.mGutsStart == (longitudinal ? 18 : 15), "Physical intestines missing");
            require(saved.mBodies.size() == (longitudinal ? 54 : 51), "Intestines are visual-only instead of rigid bodies");
            if (longitudinal)
            {
                bool arm = false, leg = false;
                for (const auto& j : saved.mJoints) { arm |= j.mChild == 5 && j.mParent == 16; leg |= j.mChild == 9 && j.mParent == 15; }
                require(arm && leg, "Right limbs remain linked to the left longitudinal half");
            }
            else require(saved.mCuts == (crushing ? 686u : 2u), "Hammer tearing/horizontal waist separation selected incorrect parts");
            for (int frame = 0; frame < 150; ++frame) corpse->update(1.f / 60);
            corpse->writeState(saved);
            if (longitudinal)
            {
                std::vector<osg::Matrixf> transforms;
                for (unsigned i = 0; i < fixture.mSkeleton->getNumChildren(); ++i)
                    if (auto* node = dynamic_cast<osg::MatrixTransform*>(fixture.mSkeleton->getChild(i));
                        node && node->getName() == "Brutal Morrowind longitudinal half") transforms.push_back(node->getMatrix());
                require(transforms.size() == 2 && !near(transforms[0], transforms[1]), "Visible halves do not follow independent physics");
                corpse->setSkinParts({});
                require(torso->getNodeMask() != 0, "Looting/removing a split armour part retained its hidden original");
            }
            for (const auto& body : saved.mBodies)
                for (float v : body.mTransform) require(std::isfinite(v), "Gore physics diverged");
            for (int chain = 0; chain < 2; ++chain)
                for (int i = 1; i < 18; ++i)
                {
                    const auto& a = saved.mBodies[saved.mGutsStart + chain * 18 + i - 1].mTransform;
                    const auto& b = saved.mBodies[saved.mGutsStart + chain * 18 + i].mTransform;
                    require((osg::Vec3f(a[0], a[1], a[2]) - osg::Vec3f(b[0], b[1], b[2])).length() < 14,
                        "Intestinal joints stretched into disconnected segments");
                }
            require(deposits > 0, "Ragdoll bleeding never deposited blood on the actual collision mesh");
            for (int frame = 0; frame < 60; ++frame) corpse->update(1.f / 60);
            require(!corpse->isSimulating(), "Gore physics exceeded the configured corpse lifetime");
            const int frozenDeposits = deposits;
            for (int frame = 0; frame < 30; ++frame) corpse->update(1.f / 60);
            require(deposits > frozenDeposits, "Freezing the corpse stopped blood from reaching world surfaces");
            corpse->writeState(saved);
            corpse.reset();
            corpse = MWRender::RagdollController::restore(*fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, config, saved);
            require(corpse && corpse->getWounds().mLongitudinal == longitudinal, "Brutal wound failed to restore");
            if (longitudinal)
            {
                corpse->splitPart(*torso, 1); corpse->setSkinParts({ torso });
                require(torso->getNodeMask() == 0, "Loaded longitudinal corpse kept a whole torso");
                for (unsigned i = 0; i < fixture.mSkeleton->getNumChildren(); ++i)
                    if (fixture.mSkeleton->getChild(i)->getName() == "Brutal Morrowind longitudinal half")
                        require(fixture.mSkeleton->getChild(i)->getBound().radius() < 80, "Loaded cut mesh stretched across its halves");
            }
            const int beforeReloadedBleed = deposits;
            for (int frame = 0; frame < 60; ++frame) corpse->update(1.f / 60);
            require(deposits > beforeReloadedBleed, "Loading a frozen corpse stopped its remaining surface bleeding");
            corpse.reset();
            require(fixture.mSkeleton->getNumChildren() == 1, "Brutal gore nodes leaked on cleanup");
        }
        std::cout << "PASS real longitudinal halves, right-side articulated limbs, hammer tearing, 36 intestinal segments and pose restoration\n";
    }

    void oneHandedBladeWithoutVisibleMeshIntersection()
    {
        for (float angle : { 0.f, 1.2f })
        for (auto attack : { Ragdoll::AttackType::Chop, Ragdoll::AttackType::Slash })
        for (const auto& [first, second, cut] : {
                 std::tuple("Head", "Head", 2), std::tuple("L Forearm", "L Hand", 3),
                 std::tuple("R Forearm", "R Hand", 5), std::tuple("L Calf", "L Foot", 7),
                 std::tuple("R Calf", "R Foot", 9), std::tuple("Spine1", "Spine2", 0) })
        {
            SkeletonFixture fixture;
            fixture.mWorld->setMatrix(osg::Matrixf::scale(1.2f, 1.2f, 1.2f)
                * osg::Matrixf::rotate(angle, osg::Vec3f(0, 0, 1)) * osg::Matrixf::translate(50, 30, 0));
            const auto point = (worldMatrix(*fixture.mBones.at(first)).getTrans()
                + worldMatrix(*fixture.mBones.at(second)).getTrans()) * .5f;
            const auto direction = osg::Vec3f(0, 1, 0) * osg::Matrixf::rotate(angle, osg::Vec3f(0, 0, 1));
            const auto from = point - direction * 200;
            const auto to = point + direction * 200;
            osg::ref_ptr<osgUtil::LineSegmentIntersector> ray = new osgUtil::LineSegmentIntersector(from, to);
            osgUtil::IntersectionVisitor visitor(ray);
            fixture.mSkeleton->accept(visitor);
            require(!ray->containsIntersections(), "Fallback fixture unexpectedly has visible mesh intersections");
            const auto contact = MWRender::RagdollController::findHitContact(*fixture.mSkeleton, from, to);
            require(contact.mBody >= 0, "A native confirmed hit was lost because rendering had no mesh hit");
            Ragdoll::Impact hit;
            hit.mValid = hit.mHasWeaponContact = true;
            hit.mWoundWeapon = Ragdoll::WoundWeapon::Blade;
            hit.mAttackType = attack;
            hit.mWeaponDirection.set(1, 0, 0);
            hit.mWeaponHitBody = contact.mBody;
            hit.mWeaponPosition = contact.mPosition;
            MWRender::RagdollController::Settings settings;
            settings.mNpc = true;
            settings.mWoundChance = 1;
            settings.mWoundRoll = 0;
            auto corpse = MWRender::RagdollController::create(
                *fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, settings, hit);
            const auto expected = cut ? 1u << cut : 0u;
            if (!corpse || corpse->getWounds().mCuts != expected || corpse->getWounds().mBurstHead)
                throw std::runtime_error("One-hand fallback wound mismatch for " + std::string(first)
                    + ", resolved body " + std::to_string(contact.mBody));
        }
        // The native melee cone can accept an aim just outside visible/skeletal
        // surfaces. Its fallback must still select the adjacent arm, not a leg.
        SkeletonFixture fixture;
        fixture.mWorld->setMatrix(osg::Matrixf::identity());
        const auto arm = (worldMatrix(*fixture.mBones.at("L Forearm")).getTrans()
            + worldMatrix(*fixture.mBones.at("L Hand")).getTrans()) * .5f + osg::Vec3f(0, 0, 10);
        const auto contact = MWRender::RagdollController::findHitContact(*fixture.mSkeleton,
            arm - osg::Vec3f(0, 200, 0), arm + osg::Vec3f(0, 200, 0));
        require(contact.mBody == 3 || contact.mBody == 4 || contact.mBody == 11,
            "A broad-cone arm hit was lost or mapped to a different extremity");
        std::cout << "PASS one-handed chop/slash dismemberment on head/arms/legs without visible ray hits; native cone fallback\n";
    }

    void livingContactAnatomy()
    {
        for (float rotation : { 0.f, 1.4f, 3.14f })
        {
            SkeletonFixture fixture;
            fixture.mWorld->setMatrix(osg::Matrixf::scale(.8f, 1.1f, 1.3f)
                * osg::Matrixf::rotate(rotation, osg::Vec3f(0, 0, 1)) * osg::Matrixf::translate(300, -200, 70));
            for (const auto& [first, second, expected] : {
                     std::tuple("Head", "Head", 2), std::tuple("L UpperArm", "L Forearm", 3),
                     std::tuple("L Forearm", "L Hand", 4), std::tuple("R UpperArm", "R Forearm", 5),
                     std::tuple("R Forearm", "R Hand", 6), std::tuple("L Thigh", "L Calf", 7),
                     std::tuple("L Calf", "L Foot", 8), std::tuple("R Thigh", "R Calf", 9),
                     std::tuple("R Calf", "R Foot", 10), std::tuple("L Hand", "L Hand", 11),
                     std::tuple("R Hand", "R Hand", 12), std::tuple("L Foot", "L Foot", 13),
                     std::tuple("R Foot", "R Foot", 14) })
            {
                const auto point = (worldMatrix(*fixture.mBones.at(first)).getTrans()
                    + worldMatrix(*fixture.mBones.at(second)).getTrans()) * .5f;
                const int actual = MWRender::RagdollController::findHitBody(*fixture.mSkeleton, point);
                if (actual != expected)
                    throw std::runtime_error("Living hit mapped " + std::string(first) + " to " + std::to_string(actual));
            }
            require(MWRender::RagdollController::findHitBody(*fixture.mSkeleton, { -10000, 10000, 0 }) == -1,
                "Far-away contact invented an anatomical hit");
            Ragdoll::Impact hit;
            hit.mValid = hit.mHasWeaponContact = true;
            hit.mWoundWeapon = Ragdoll::WoundWeapon::HeavyBlade;
            hit.mAttackType = Ragdoll::AttackType::Chop;
            hit.mWeaponPosition = worldMatrix(*fixture.mBones.at("Head")).getTrans();
            hit.mWeaponHitBody = MWRender::RagdollController::findHitBody(*fixture.mSkeleton, hit.mWeaponPosition);
            // Deferred damage/death may move the entire skeleton away from the
            // stored world-space contact; the captured anatomical part must win.
            fixture.mWorld->setMatrix(fixture.mWorld->getMatrix() * osg::Matrixf::translate(700, 0, 0));
            MWRender::RagdollController::Settings settings;
            settings.mNpc = true;
            settings.mWoundRoll = 0;
            auto corpse = MWRender::RagdollController::create(
                *fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, settings, hit);
            require(corpse && corpse->getWounds().mCuts == (1u << 2) && !corpse->getWounds().mBurstHead,
                "Vertical sword head hit became a waist cut/burst after the pose changed");
            corpse.reset();
            hit.mWeaponHitBody = -1;
            corpse = MWRender::RagdollController::create(
                *fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, settings, hit);
            require(corpse && !corpse->getWounds().mCuts, "Unverified surface contact guessed a limb");
        }
        // Actual OSG ray intersections distinguish a clothed arm in front of a
        // leg, independent of how close their coarse skeletal capsules are.
        osg::ref_ptr<osg::Group> root = new osg::Group;
        for (const auto [depth, body] : { std::pair(3.f, 7), std::pair(0.f, 4) })
        {
            osg::ref_ptr<osg::Geode> part = new osg::Geode;
            part->setUserValue(SceneUtil::HitBodyTag, body);
            part->addDrawable(new osg::ShapeDrawable(new osg::Box(osg::Vec3f(0, depth, 0), 4, 1, 4)));
            root->addChild(part);
        }
        osg::ref_ptr<osgUtil::LineSegmentIntersector> ray
            = new osgUtil::LineSegmentIntersector(osg::Vec3f(0, -10, 0), osg::Vec3f(0, 10, 0));
        osgUtil::IntersectionVisitor visitor(ray);
        root->accept(visitor);
        require(ray->containsIntersections() && SceneUtil::getHitBody(ray->getFirstIntersection().nodePath) == 4,
            "A visible clothed forearm was attributed to the leg behind it");
        root->removeChild(1);
        ray->reset();
        root->accept(visitor);
        require(ray->containsIntersections() && SceneUtil::getHitBody(ray->getFirstIntersection().nodePath) == 7,
            "Removing the foreground arm did not expose the actual leg hit");
        std::cout << "PASS living-pose anatomy, rotated/scaled rigs, deferred head contact and overlapping clothed parts\n";
    }

    void dotDeathHasNoBlastMomentum()
    {
        for (auto element : { Ragdoll::DeathElement::Fire, Ragdoll::DeathElement::Frost,
                 Ragdoll::DeathElement::Shock, Ragdoll::DeathElement::Poison })
        {
            for (bool initial : { true, false })
            {
                SkeletonFixture fixture;
                MWMechanics::RagdollMagicImpact context;
                context.mHit = std::make_shared<MWMechanics::RagdollHit>();
                context.mExploded = true;
                context.mRadius = 10;
                const auto target = worldMatrix(*fixture.mBones.at("Spine1")).getTrans();
                context.mOrigin = target - osg::Vec3f(2 * Constants::UnitsPerMeter, 0, 0);
                context.mHit->mContactAge = initial ? .5 : 2.;
                const auto impact = MWMechanics::makeRagdollMagicImpact(context, target, 1, element, 50);
                MWRender::RagdollController::Settings settings;
                settings.mNpc = true;
                settings.mWoundRoll = 0;
                auto corpse = MWRender::RagdollController::create(
                    *fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, settings, impact);
                require(bool(corpse), "DOT/blast fixture failed to create");
                ESM::RagdollState state;
                corpse->writeState(state);
                osg::Vec3f momentum;
                for (const auto& body : state.mBodies)
                    momentum += osg::Vec3f(body.mLinearVelocity[0], body.mLinearVelocity[1], body.mLinearVelocity[2])
                        * body.mMass;
                if (!initial)
                {
                    // A rigid frozen corpse has a small native tipping impulse.
                    // Compare to the same elemental death with no contact data,
                    // rather than confusing that existing fall with a blast.
                    SkeletonFixture baseline;
                    Ragdoll::Impact noContact;
                    noContact.mValid = true;
                    noContact.mDeathElement = element;
                    auto reference = MWRender::RagdollController::create(
                        *baseline.mSkeleton, *baseline.mSkeleton, floorSnapshot, settings, noContact);
                    require(bool(reference), "DOT baseline fixture failed to create");
                    ESM::RagdollState expected;
                    reference->writeState(expected);
                    osg::Vec3f baselineMomentum;
                    for (const auto& body : expected.mBodies)
                        baselineMomentum += osg::Vec3f(body.mLinearVelocity[0], body.mLinearVelocity[1], body.mLinearVelocity[2])
                            * body.mMass;
                    require((momentum - baselineMomentum).length() < .01f && state.mElement == int(element) && !state.mCuts,
                        "DOT death replayed blast momentum or lost its elemental appearance");
                }
                else if (element != Ragdoll::DeathElement::Poison)
                    require(momentum.x() > 1000, "Initial lethal explosion lost its outward impulse");
            }
        }
        std::cout << "PASS actual corpse momentum: initial lethal blast versus delayed elemental DOT death\n";
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

    // Capture the actual CPU-skinned vertex arrays without requiring a GL
    // context. A mixed-weight triangle exposes long cross-cut clothing strips.
    class CaptureSkin : public osgUtil::CullVisitor
    {
    public:
        std::map<std::string, std::vector<osg::Vec3f>> mVertices;
        void apply(osg::Group& node) override { traverse(node); }
        void apply(osg::MatrixTransform& node) override { traverse(node); }
        void apply(osg::Geode& node) override { traverse(node); }
        void apply(osg::Billboard& node) override { traverse(node); }
        void apply(osg::Drawable& drawable) override
        {
            if (auto* geometry = drawable.asGeometry())
                apply(*geometry);
        }
        void apply(osg::Geometry& geometry) override
        {
            const auto* vertices = static_cast<const osg::Vec3Array*>(geometry.getVertexArray());
            mVertices[geometry.getName()] = { vertices->begin(), vertices->end() };
        }
    };

    void severedSkin(bool waist)
    {
        SkeletonFixture fixture;
        MWRender::RagdollController::Settings config;
        config.mNpc = true;
        config.mWoundRoll = 0;
        Ragdoll::Impact hit;
        hit.mValid = true;
        if (waist)
        {
            hit.mHasWeaponContact = true;
            hit.mWoundWeapon = Ragdoll::WoundWeapon::HeavyBlade;
            hit.mAttackType = Ragdoll::AttackType::Slash;
            hit.mWeaponDirection.set(1, 0, 0);
            hit.mWeaponPosition = worldMatrix(*fixture.mBones.at("Spine1")).getTrans();
            hit.mWeaponHitBody = MWRender::RagdollController::findHitBody(*fixture.mSkeleton, hit.mWeaponPosition);
        }
        else
        {
            hit.mDirectMagic = true;
            hit.mDeathElement = Ragdoll::DeathElement::Fire;
        }
        auto corpse = MWRender::RagdollController::create(
            *fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, config, hit);
        require(corpse && corpse->getWounds().mCuts, "Skin fixture needs separated limbs");
        require(!waist || corpse->getWounds().cuts(1), "Skin fixture missed waist contact");
        struct Patch
        {
            osg::ref_ptr<SceneUtil::RigGeometry> mRig;
            osg::Matrixf mInvBind;
            std::string mAnchor;
        };
        std::vector<Patch> patches;
        for (const std::size_t body : { 1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 13u })
        {
            const auto pose = corpse->getPartPose(body);
            require(!pose.mAnchor.empty() && !pose.mOffsets.empty(), "Missing cross-cut part pose");
            const std::string foreign = pose.mOffsets.begin()->first;
            auto* owner = fixture.mSkeleton->getBone(pose.mAnchor);
            auto* other = fixture.mSkeleton->getBone(foreign);
            fixture.mSkeleton->updateBoneMatrices(1);
            const auto ownerBind = owner->mMatrixInSkeletonSpace;
            const auto otherBind = other->mMatrixInSkeletonSpace;
            osg::ref_ptr<osg::Geometry> source = new osg::Geometry;
            source->setName("Skin" + std::to_string(body));
            auto* vertices = new osg::Vec3Array;
            vertices->push_back(osg::Vec3f(0, 0, 0) * ownerBind);
            vertices->push_back(osg::Vec3f(1, 0, 0) * ownerBind);
            vertices->push_back(osg::Vec3f(0, 1, 0) * ownerBind);
            source->setVertexArray(vertices);
            source->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
            osg::ref_ptr<SceneUtil::RigGeometry> rig = new SceneUtil::RigGeometry;
            rig->setName(source->getName());
            rig->setSourceGeometry(source);
            rig->setBoneInfo({ { pose.mAnchor, osg::BoundingSpheref({}, 2), osg::Matrixf::inverse(ownerBind) },
                { Misc::StringUtils::lowerCase(foreign), osg::BoundingSpheref({}, 2), osg::Matrixf::inverse(otherBind) } });
            rig->setInfluences({ { { 0, 1.f } }, { { 1, 1.f } }, { { 0, .5f }, { 1, .5f } } });
            rig->setBonePoseOverride(pose.mOffsets, pose.mAnchor);
            fixture.mSkeleton->addChild(rig);
            patches.push_back({ rig, osg::Matrixf::inverse(ownerBind), pose.mAnchor });
        }
        // A coating clone must keep a live relative pose, not the previous
        // frame's absolute matrices or another actor's cached mesh state.
        osg::ref_ptr<SceneUtil::RigGeometry> clone
            = new SceneUtil::RigGeometry(*patches.front().mRig, osg::CopyOp::SHALLOW_COPY);
        auto cloneSource = new osg::Geometry(*clone->getSourceGeometry(), osg::CopyOp::SHALLOW_COPY);
        cloneSource->setName("CoatedSkin");
        clone->setSourceGeometry(cloneSource);
        fixture.mSkeleton->addChild(clone);
        patches.push_back({ clone, patches.front().mInvBind, patches.front().mAnchor });
        for (unsigned frame = 2; frame < 65; ++frame)
        {
            if (frame == 32)
            {
                ESM::RagdollState saved;
                corpse->writeState(saved);
                corpse.reset();
                corpse = MWRender::RagdollController::restore(
                    *fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, config, saved);
                require(bool(corpse), "Severed clothing fixture failed save/restore");
            }
            corpse->update(1.f / 30);
            osg::ref_ptr<osg::FrameStamp> stamp = new osg::FrameStamp;
            stamp->setFrameNumber(frame);
            osgUtil::UpdateVisitor update;
            update.setFrameStamp(stamp);
            update.setTraversalNumber(frame);
            fixture.mSkeleton->accept(update);
            CaptureSkin capture;
            capture.setFrameStamp(stamp);
            capture.setTraversalNumber(frame);
            fixture.mSkeleton->accept(capture);
            for (const auto& patch : patches)
            {
                const auto expected = patch.mInvBind
                    * fixture.mSkeleton->getBone(patch.mAnchor)->mMatrixInSkeletonSpace;
                const auto* source = static_cast<const osg::Vec3Array*>(patch.mRig->getSourceGeometry()->getVertexArray());
                const auto& actual = capture.mVertices.at(patch.mRig->getSourceGeometry()->getName());
                require(actual.size() == 3, "Skinned clothing triangle disappeared");
                for (unsigned vertex = 0; vertex < 3; ++vertex)
                    require((actual[vertex] - (*source)[vertex] * expected).length() < .02f,
                        "Clothing stretched across a severed joint or lagged behind its moving limb");
            }
        }
        const auto sleeve = corpse->getPartPose(3);
        require(!sleeve.mOffsets.count("Bip01 L Forearm") && !sleeve.mOffsets.count("Bip01 L Hand"),
            "Severing froze the intact elbow or wrist");
        const auto robe = corpse->getPartPose(1, true);
        require(robe.mOffsets.count("Bip01 L Forearm") && robe.mOffsets.count("Bip01 L Thigh"),
            "Whole robe must retain all limbs relative to the torso");
        std::cout << "PASS actual mixed-weight skinning, " << (waist ? "waist" : "extremities")
                  << ", moving coating clones, save/restore, intact joints and whole robe\n";
    }

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

    void bloodCacheCapacityAndBuffers()
    {
        osg::ref_ptr<osg::Group> scene = new osg::Group;
        MWRender::BloodDecals decals(*scene);
        decals.setCellActive("arena", true);
        MWRender::BloodStamp stamp;
        stamp.mNormal.set(0, 0, 1);
        for (int i = 0; i < int(MWRender::BloodDecals::MaxStains) + 2048; ++i)
        {
            stamp.mCenter.set(float(i), 0, 0);
            stamp.mTriangles = { stamp.mCenter, stamp.mCenter + osg::Vec3f(1, 0, 0),
                stamp.mCenter + osg::Vec3f(0, 1, 0) };
            decals.add("arena", stamp);
        }
        decals.update(.1f, 600);
        require(decals.size() == MWRender::BloodDecals::MaxStains && decals.saveState().front().mStamp.mCenter.x() == 2048,
            "Full blood cache did not evict exactly the oldest stain");
        require(scene->getNumChildren() <= MWRender::BloodDecals::MaxStains / MWRender::BloodDecals::ChunkStains + 1, "Blood cache produced an unbounded number of drawables");
        struct Buffer
        {
            osg::ref_ptr<osg::Geometry> mMesh;
            osg::ref_ptr<osg::Array> mVertices;
            unsigned mModified;
        };
        std::vector<Buffer> buffers;
        for (unsigned i = 0; i < scene->getNumChildren(); ++i)
        {
            auto* mesh = scene->getChild(i)->asGroup()->getChild(0)->asGeode()->getDrawable(0)->asGeometry();
            buffers.push_back({ mesh, mesh->getVertexArray(), mesh->getVertexArray()->getModifiedCount() });
        }
        for (int frame = 0; frame < 120; ++frame) decals.update(.1f, 600);
        for (const auto& buffer : buffers)
            require(buffer.mMesh->getVertexArray() == buffer.mVertices
                && buffer.mVertices->getModifiedCount() == buffer.mModified,
                "Aging blood reallocated/reuploaded all stain vertices");
        decals.add("arena", stamp);
        decals.update(.1f, 600);
        unsigned changed = 0;
        for (const auto& buffer : buffers)
        {
            require(buffer.mMesh->getVertexArray() == buffer.mVertices, "Blood batch replaced its persistent VBO array");
            changed += buffer.mVertices->getModifiedCount() != buffer.mModified;
        }
        require(changed <= 2, "One new stain rebuilt unrelated blood chunks");
        const auto saved = decals.saveState();
        decals.setCellActive("arena", false);
        decals.restoreState(saved);
        require(decals.size() == MWRender::BloodDecals::MaxStains && scene->getNumChildren() <= MWRender::BloodDecals::MaxStains / MWRender::BloodDecals::ChunkStains + 1, "Full blood save/load grew the render graph");
        for (unsigned i = 0; i < scene->getNumChildren(); ++i)
            require(scene->getChild(i)->getNodeMask() == 0, "Restored blood became visible in an inactive cell");
        decals.update(600, 600);
        require(decals.size() == 0 && scene->getNumChildren() == 0, "Expired full blood cache leaked render chunks");
        std::cout << "PASS full extended stain-cache rollover, stable VBOs while aging, local chunk updates, save/load and cleanup\n";
    }

    void goreLoadMeasurements(bool benchmark)
    {
        osg::ref_ptr<osg::Group> scene = new osg::Group;
        MWRender::BloodDecals decals(*scene);
        decals.setCellActive("arena", true);
        std::vector<std::unique_ptr<SkeletonFixture>> fixtures;
        std::vector<std::unique_ptr<MWRender::RagdollController>> corpses;
        struct Tube
        {
            osg::ref_ptr<osg::Geode> mNode;
            osg::ref_ptr<osg::Geometry> mMesh;
            osg::ref_ptr<osg::Array> mVertices;
            osg::ref_ptr<osg::PrimitiveSet> mIndices;
        };
        std::map<const osg::Geode*, Tube> tubes;
        unsigned sprites = 0;
        double total = 0, maximum = 0;
        for (int frame = 0; frame < 900; ++frame)
        {
            const auto start = std::chrono::steady_clock::now();
            if (frame < 600 && frame % 60 == 0)
            {
                auto fixture = std::make_unique<SkeletonFixture>();
                fixture->mWorld->setMatrix(osg::Matrixf::translate(float(fixtures.size()) * 80 - 360, 0, 0));
                scene->addChild(fixture->mWorld);
                MWRender::RagdollController::Settings config;
                config.mNpc = true; config.mWoundRoll = 0; config.mWoundChance = 1;
                config.mBloodAmount = 3;
                config.mBloodDeposit = [&](const auto& stamp) { decals.add("arena", stamp); };
                config.mBloodEffect = [&](const auto&) { ++sprites; };
                Ragdoll::Impact hit;
                hit.mValid = hit.mHasWeaponContact = true;
                hit.mWoundWeapon = Ragdoll::WoundWeapon::Blunt;
                hit.mAttackType = Ragdoll::AttackType::Chop;
                hit.mTwoHandedHammer = true; hit.mWeaponHitBody = 1;
                if (!benchmark && fixtures.size() % 3 != 0)
                {
                    hit.mWoundWeapon = Ragdoll::WoundWeapon::HeavyBlade;
                    hit.mTwoHandedHammer = false;
                    hit.mTwoHandedCutting = true;
                    hit.mWeaponHitBody = fixtures.size() % 3 == 1 ? 2 : 1;
                    hit.mAttackType = fixtures.size() % 3 == 1 ? Ragdoll::AttackType::Chop : Ragdoll::AttackType::Slash;
                    hit.mWeaponDirection.set(0, 1, hit.mAttackType == Ragdoll::AttackType::Chop ? -.45f : 0.f);
                }
                corpses.push_back(MWRender::RagdollController::create(
                    *fixture->mSkeleton, *fixture->mSkeleton, floorSnapshot, config, hit));
                require(bool(corpses.back()), "Concurrent gore corpse creation failed");
                fixtures.push_back(std::move(fixture));
            }
            const unsigned before = sprites;
            for (auto& corpse : corpses) corpse->update(1.f / 60);
            require(sprites - before <= corpses.size() * 2, "Severing more limbs multiplied whole NIF particle systems");
            decals.update(1.f / 60, 600);
            require(decals.size() <= MWRender::BloodDecals::MaxStains, "Concurrent wounds exceeded the blood memory limit");
            for (auto& fixture : fixtures)
                for (unsigned i = 0; i < fixture->mSkeleton->getNumChildren(); ++i)
                {
                    auto* node = fixture->mSkeleton->getChild(i)->asGeode();
                    if (!node || node->getName() != "Brutal Morrowind physical intestines") continue;
                    auto* mesh = node->getDrawable(0)->asGeometry();
                    const auto [it, inserted] = tubes.try_emplace(node, Tube{ node, mesh,
                        mesh->getVertexArray(), mesh->getPrimitiveSet(0) });
                    require(inserted || (it->second.mMesh == mesh && it->second.mVertices == mesh->getVertexArray()
                        && it->second.mIndices == mesh->getPrimitiveSet(0)), "Intestines reallocated their render buffers each frame");
                    require(mesh->getVertexArray()->getNumElements() == 468, "Intestine rings are not shared by adjacent triangles");
                }
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            total += ms; maximum = std::max(maximum, ms);
            if (frame % 60 == 59)
            {
                if (benchmark)
                    std::cout << "GORE frame=" << frame + 1 << " corpses=" << corpses.size()
                              << " stains=" << decals.size() << " mean_ms=" << total / 60 << " max_ms=" << maximum << std::endl;
                total = maximum = 0;
            }
        }
        require(tubes.size() == 20, "Concurrent waist/longitudinal/hammer deaths lost their physical intestines");
        for (auto& corpse : corpses)
        {
            corpse->update(20);
            require(!corpse->isSimulating(), "Crowded gore bypassed the corpse simulation deadline");
            ESM::RagdollState state; corpse->writeState(state);
            for (const auto& body : state.mBodies)
                for (float value : body.mTransform) require(std::isfinite(value), "Concurrent gore physics became nonfinite");
        }
        corpses.clear(); tubes.clear();
        for (auto& fixture : fixtures)
        {
            require(fixture->mSkeleton->getNumChildren() == 1, "Crowded gore leaked blood/tube/wound nodes after deletion");
            scene->removeChild(fixture->mWorld);
        }
        decals.update(600, 600);
        require(scene->getNumChildren() == 0, "Blood chunks survived expiry after the crowd was removed");
        std::cout << "PASS ten concurrent dismembered corpses, maximum blood, persistent intestinal meshes, bounded sprites and cleanup\n";
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

    void sliderLaunchScaling()
    {
        const auto sample = [](float distance, float scale, bool magic, bool capped, const std::string& contact) {
            SkeletonFixture fixture;
            fixture.mWorld->setMatrix(osg::Matrixf::translate(0, 0, 1000));
            MWRender::RagdollController::Settings settings;
            settings.mPinEnabled = false;
            if (capped) settings.mMaxLaunchSpeed = 100;
            Ragdoll::Impact impact;
            impact.mValid = true;
            if (magic)
            {
                impact.mMagicDistance = distance * scale;
                impact.mMagicImpulseScale = scale;
                impact.mMagicDirection.set(1, 0, 0);
            }
            else
            {
                impact.mWeaponDistance = distance;
                impact.mWeaponImpulseScale = scale;
                impact.mWeaponDirection.set(1, 0, 0);
                impact.mHasWeaponContact = true;
                impact.mWeaponPosition = worldMatrix(*fixture.mBones.at(contact)).getTrans();
            }
            auto corpse = MWRender::RagdollController::create(
                *fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, settings, impact);
            require(bool(corpse), "Slider launch fixture failed to create");
            ESM::RagdollState state;
            corpse->writeState(state);
            const auto centre = [](const ESM::RagdollState& state) {
                osg::Vec3f position;
                for (const auto& part : state.mBodies)
                    position += osg::Vec3f(part.mTransform[0], part.mTransform[1], part.mTransform[2]) * part.mMass;
                return position / state.mMass;
            };
            osg::Vec3f momentum;
            for (const auto& part : state.mBodies)
                momentum += osg::Vec3f(part.mLinearVelocity[0], part.mLinearVelocity[1], part.mLinearVelocity[2]) * part.mMass;
            const auto start = centre(state);
            for (int frame = 0; frame < 30; ++frame) corpse->update(1.f / 120);
            corpse->writeState(state);
            return std::pair(momentum.x(), (centre(state) - start).x());
        };
        for (const auto& [name, distance, magic, capped] : {
                 std::tuple("melee", .25f, false, false), std::tuple("two-handed", 1.f, false, false),
                 std::tuple("crossbow", 2.4f, false, false), std::tuple("bow", .3f, false, false),
                 std::tuple("thrown", .15f, false, false), std::tuple("magic", 2.f, true, false),
                 std::tuple("capped-crossbow", 2.4f, false, true) })
            for (const std::string contact : { "Head", "L Hand", "Spine2" })
            {
                const auto base = sample(distance, 1, magic, capped, contact);
                require(base.first > 0 && base.second > 1, "Baseline slider did not launch a corpse");
                for (const float scale : { .5f, 3.f })
                {
                    const auto actual = sample(distance, scale, magic, capped, contact);
                    require(std::abs(actual.first / base.first - scale) < .001f,
                        "Slider momentum was lost to a per-limb or launch speed cap");
                    require(std::abs(actual.second / base.second - scale) < .02f,
                        "Actual Bullet corpse travel did not follow the 50%-300% slider");
                }
            }
        std::cout << "PASS all launch sliders: 0.5x/1x/3x actual momentum and Bullet travel, including head/hand hits and caps\n";
    }

    void groundedSliderResponse()
    {
        const auto sample = [](float scale) {
            SkeletonFixture fixture;
            fixture.mWorld->setMatrix(osg::Matrixf::identity());
            MWRender::RagdollController::Settings settings;
            settings.mPinEnabled = false;
            Ragdoll::Impact impact;
            impact.mValid = impact.mHasWeaponContact = true;
            impact.mWeaponDistance = 2.4f;
            impact.mWeaponImpulseScale = scale;
            impact.mWeaponDirection.set(1, 0, 0);
            impact.mWeaponPosition = worldMatrix(*fixture.mBones.at("Head")).getTrans();
            const auto broadFloor = [](const osg::Vec3f& center, float radius) {
                auto vertices = floorSnapshot(center, radius);
                for (auto& vertex : vertices) { vertex.x() *= 10; vertex.y() *= 10; }
                return vertices;
            };
            auto corpse = MWRender::RagdollController::create(
                *fixture.mSkeleton, *fixture.mSkeleton, broadFloor, settings, impact);
            require(bool(corpse), "Ground slider fixture failed to create");
            for (int frame = 0; frame < 601; ++frame) corpse->update(1.f / 60);
            ESM::RagdollState state;
            corpse->writeState(state);
            float x = 0;
            for (const auto& body : state.mBodies) x += body.mMass * body.mTransform[0];
            return x / state.mMass;
        };
        const float low = sample(.5f), base = sample(1), high = sample(3);
        std::cout << "Settled crossbow slider response: " << low << '/' << base << '/' << high << std::endl;
        require(low > 0 && base > 0 && high > base * 1.5f && high > low * 2,
            "Ground contact erased the visible difference between slider endpoints");
        std::cout << "PASS ground contact retains a visible slider response; exact impulse ratios checked separately\n";
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

    void pinAndStatusSliderBounds()
    {
        const auto wall = [](const osg::Vec3f& center, float radius) {
            auto vertices = floorSnapshot(center, radius);
            for (const auto& point : { osg::Vec3f(120, -300, 0), osg::Vec3f(120, 300, 0), osg::Vec3f(120, 300, 300),
                     osg::Vec3f(120, -300, 0), osg::Vec3f(120, 300, 300), osg::Vec3f(120, -300, 300) })
                vertices.push_back(point);
            return vertices;
        };
        for (const float pinDistance : { 1.25f, 7.5f })
        {
            SkeletonFixture fixture;
            fixture.mWorld->setMatrix(osg::Matrixf::identity());
            MWRender::RagdollController::Settings settings;
            settings.mPinMaxDistance = pinDistance;
            Ragdoll::Impact impact;
            impact.mValid = impact.mCrossbow = true;
            impact.mWeaponDistance = 5;
            impact.mWeaponDirection.set(1, 0, 0);
            auto corpse = MWRender::RagdollController::create(
                *fixture.mSkeleton, *fixture.mSkeleton, wall, settings, impact);
            require(bool(corpse), "Pin slider fixture failed to create");
            for (int frame = 0; frame < 240; ++frame) corpse->update(1.f / 120);
            require(corpse->isPinned() == (pinDistance == 7.5f), "Pin distance slider failed to change wall eligibility");
        }
        for (const float scale : { .5f, 1.f, 3.f })
        {
            SkeletonFixture fixture;
            MWRender::RagdollController::Settings settings;
            settings.mNpc = true;
            settings.mWoundRoll = 0;
            settings.mMaxSimulationTime = .1f;
            settings.mBleedDuration = 10 * scale;
            settings.mFireEffectDuration = 15 * scale;
            int blood = 0;
            settings.mBloodEffect = [&](const auto&) { ++blood; };
            Ragdoll::Impact impact;
            impact.mValid = impact.mHasWeaponContact = true;
            impact.mWoundWeapon = Ragdoll::WoundWeapon::Blunt;
            impact.mAttackType = Ragdoll::AttackType::Chop;
            impact.mWeaponPosition = worldMatrix(*fixture.mBones.at("Head")).getTrans();
            impact.mWeaponHitBody = MWRender::RagdollController::findHitBody(*fixture.mSkeleton, impact.mWeaponPosition);
            impact.mDeathElement = Ragdoll::DeathElement::Fire;
            auto corpse = MWRender::RagdollController::create(
                *fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, settings, impact);
            require(bool(corpse), "Status slider fixture failed to create");
            corpse->update(10 * scale - .1f);
            require(blood > 1 && corpse->hasFireEffect(), "Scaled blood/fire ended before their deadline");
            const int before = blood;
            corpse->update(.2f);
            require(blood == before, "Bleeding did not end at the scaled deadline");
            corpse->update(5 * scale);
            require(!corpse->hasFireEffect(), "Flames did not end at the scaled deadline");
        }
        std::cout << "PASS min/max wall pin range and 50%/100%/300% blood/flame deadlines after physics freeze\n";
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
        settings.mMaxActive = 300;
        std::vector<std::unique_ptr<SkeletonFixture>> fixtures;
        std::vector<std::unique_ptr<MWRender::RagdollController>> corpses;
        for (int i = 0; i < 300; ++i)
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
        require(active == 300, "UI +200% corpse budget was still clamped at 100");
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
        corpse->setRuntimeSettings(300, 30, 7.5, 45, 30, 30, 30);
        ESM::RagdollState maximum;
        corpse->writeState(maximum);
        require(std::abs(maximum.mTimeLeft - 29.8f) < .01f && maximum.mFireDuration == 45
                && maximum.mPoisonDuration == 30 && maximum.mShockDuration == 30 && maximum.mBleedDuration == 30,
            "300% corpse/status duration was clamped or failed to update");
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
        std::cout << "PASS 300 corpses, live budget reduction and saved runtime timer adjustments\n";
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc > 1 && std::string(argv[1]) == "--gore-benchmark")
        {
            goreLoadMeasurements(true);
            return 0;
        }
        {
            SkeletonFixture fixture;
            MWRender::RagdollController::Settings config;
            config.mNpc = true;
            config.mWoundRoll = 0;
            int blood = 0;
            config.mBloodEffect = [&](const osg::Vec3f& p) {
                require(std::isfinite(p.length2()), "Nonfinite wound position");
                ++blood;
            };
            Ragdoll::Impact hit;
            hit.mValid = hit.mHasWeaponContact = true;
            hit.mWoundWeapon = Ragdoll::WoundWeapon::Blunt;
            hit.mAttackType = Ragdoll::AttackType::Chop;
            hit.mWeaponPosition = worldMatrix(*fixture.mBones.at("Head")).getTrans();
            hit.mWeaponHitBody = MWRender::RagdollController::findHitBody(*fixture.mSkeleton, hit.mWeaponPosition);
            auto corpse = MWRender::RagdollController::create(
                *fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, config, hit);
            require(corpse && corpse->getWounds().mBurstHead, "Blunt fatal blow did not burst head");
            ESM::RagdollState saved;
            for (int frame = 0; frame < 60; ++frame) corpse->update(1.f/60);
            corpse->writeState(saved);
            require(saved.mBodies.size() == 21 && saved.mBurstHead && saved.mCuts == 4,
                "Gibs/cut state missing from saved physical pose");
            require(blood > 1, "Wound did not emit repeated blood effects");
            corpse.reset();
            auto restored = MWRender::RagdollController::restore(
                *fixture.mSkeleton, *fixture.mSkeleton, floorSnapshot, config, saved);
            require(bool(restored) && restored->getWounds().mBurstHead, "Gore pose failed to restore");
            for (int frame = 0; frame < 600; ++frame) restored->update(1.f/60);
            const int expired = blood;
            restored->update(.3f);
            require(blood == expired, "Bleeding continued past ten seconds");
            restored.reset();
            require(fixture.mSkeleton->getNumChildren() == 1, "Generated gore nodes leaked after destruction");
            std::cout << "PASS severed head, dynamic gibs, saved gore, bleeding deadline and cleanup\n";
        }
        if (argc > 1 && std::string(argv[1]) == "--benchmark")
        {
            loadMeasurements();
            return 0;
        }
        oneHandedBladeWithoutVisibleMeshIntersection();
        brutalBloodAndPersistence();
        bloodReceiversAndSprites();
        organicBloodRules();
        brutalMeshClipAndPose();
        brutalPhysicalWounds();
        bloodCacheCapacityAndBuffers();
        goreLoadMeasurements(false);
        livingContactAnatomy();
        dotDeathHasNoBlastMomentum();
        liveSettingsAndExtendedBudget();
        severedSkin(false);
        severedSkin(true);
        transitionAndCallbacks();
        collisionAndFreeze();
        overflowFreezesOldPose();
        savedPoseAndRemainingTime();
        genericCreatureSkeleton();
        fallbackAndSnapshotLoss();
        inertiaAndMass();
        sliderLaunchScaling();
        groundedSliderResponse();
        pinAndDeadline();
        pinAndStatusSliderBounds();
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
