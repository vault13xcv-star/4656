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
#include <osg/BoundingBox>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/MatrixTransform>
#include <osg/Texture2D>
#include <osg/ShapeDrawable>
#include <osgUtil/UpdateVisitor>
#include <osgUtil/CullVisitor>
#include <osgUtil/LineSegmentIntersector>
#include <osgDB/ReadFile>

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <set>
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
        // Check unsaturated emission rather than the visible particle cap:
        // 100% must double the former 60/14 droplets per second, and 300%
        // must still multiply that new base by three.
        for (const bool neck : { false, true })
            for (const float amount : { 0.f, -1.f, .5f, 1.f, 3.f, std::numeric_limits<float>::quiet_NaN() })
            {
                MWRender::BloodTrail trial(*root, {}, {});
                const std::vector<MWRender::BloodTrail::Source> source{ { { 0, 0, 100 }, { 0, 0, 1 }, 1, neck } };
                for (int frame = 0; frame < 50; ++frame) trial.update(.01f, source, amount, frame * .01f);
                const float multiplier = !std::isfinite(amount) ? 1.f : std::clamp(amount, 0.f, 3.f);
                const float expected = (neck ? 60.f : 14.f) * multiplier;
                require(std::abs(float(trial.emittedCount()) - expected) <= 1,
                    "Blood 100% did not double the base emission or 300% stopped scaling it");
                require(trial.particleCount() < 192, "Blood emission regression accidentally measured a saturated trail");
            }
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
        {
            std::vector<MWRender::BloodStamp> pools;
            MWRender::BloodTrail larger(*root, actualRay, [&](const auto& stamp) {
                if (stamp.mPool) pools.push_back(stamp);
            });
            const std::vector<MWRender::BloodTrail::Source> source{ { { 0, 0, 10 }, { 0, 0, 1 }, 1, false } };
            for (int frame = 0; frame < 4; ++frame) larger.update(.1f, source, 1, 4);
            require(!pools.empty(), "Grounded wound did not create a pool for decal size regression");
            // At age four preview.3's former 200% default had radius
            // (3 + sqrt(4)*5)*sqrt(2). New 100% keeps that doubled base.
            // The added 1.75 scale and existing +/-12% variation stay in
            // the user's requested 1.5 to 2 times linear size range.
            const float previousDefaultRadius = 13.f * std::sqrt(2.f);
            for (const auto& pool : pools)
            {
                require(pool.mRadius >= previousDefaultRadius * 1.5f && pool.mRadius <= previousDefaultRadius * 2.f,
                    "Projected blood pool was not enlarged by 1.5 to 2 times");
                float extent = 0;
                for (const auto& vertex : pool.mTriangles)
                {
                    extent = std::max({ extent, std::abs(vertex.x()), std::abs(vertex.y()) });
                    require(std::abs(vertex.z() - .06f) < .001f,
                        "Enlarged decal stopped projecting onto the actual floor");
                }
                require(std::abs(extent - pool.mRadius) < .001f,
                    "Enlarged blood UV radius changed without enlarging projected geometry");
            }
        }
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
        std::cout << "PASS ballistic neck spray, doubled base at 100%, scaling to 300%, enlarged projected pools, ten-minute lifetime, cell isolation and saved age\n";
    }

    osg::ref_ptr<osg::Image> bloodMarkerImage(osg::Image::Origin origin)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(16, 16, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        image->setOrigin(origin);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
            {
                const bool top = origin == osg::Image::TOP_LEFT ? y < 8 : y >= 8;
                const auto color = top ? (x < 8 ? osg::Vec4f(1, 0, 0, 1) : osg::Vec4f(0, 1, 0, 1))
                    : (x < 8 ? osg::Vec4f(0, 0, 1, 1) : osg::Vec4f(1, 1, 0, 1));
                image->setColor(x < 2 || x > 13 || y < 2 || y > 13 ? osg::Vec4f() : color, x, y);
            }
        return image;
    }

    MWRender::BloodStamp bloodQuad(bool floor, float x, std::uint32_t seed)
    {
        MWRender::BloodStamp stamp;
        stamp.mCenter = { x, 0, 0 }; stamp.mRadius = 10; stamp.mVisualSeed = seed;
        stamp.mNormal = floor ? osg::Vec3f(0, 0, 1) : osg::Vec3f(0, -1, 0);
        const osg::Vec3f right(10, 0, 0), up = floor ? osg::Vec3f(0, 10, 0) : osg::Vec3f(0, 0, 10);
        stamp.mTriangles = { stamp.mCenter - right - up, stamp.mCenter + right - up,
            stamp.mCenter + right + up, stamp.mCenter - right - up,
            stamp.mCenter + right + up, stamp.mCenter - right + up };
        return stamp;
    }

    void bloodLaunchRangeAndVisibleWounds()
    {
        struct Envelope
        {
            float mForward = 0, mWidth = 0, mSize = 0;
            std::size_t mEmitted = 0, mParticles = 0;
        };
        const auto measure = [](float amount, bool neck, bool major) {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            MWRender::BloodTrail trail(*root, {}, {});
            const std::vector<MWRender::BloodTrail::Source> source{
                { { 0, 0, 1000 }, { 1, 0, 0 }, 1, neck, major }
            };
            Envelope result;
            float minY = 0, maxY = 0;
            auto* mesh = root->getChild(0)->asGroup()->getChild(0)->asGeode()->getDrawable(0)->asGeometry();
            for (int frame = 0; frame < 40; ++frame)
            {
                trail.update(.01f, source, amount, frame * .01f);
                const auto* vertices = static_cast<const osg::Vec3Array*>(mesh->getVertexArray());
                for (const auto& vertex : *vertices)
                {
                    require(std::isfinite(vertex.length2()), "A blood spray emitted nonfinite visible geometry");
                    result.mForward = std::max(result.mForward, vertex.x());
                    minY = std::min(minY, vertex.y()); maxY = std::max(maxY, vertex.y());
                }
                // Each drop has a persistent, visible eight-triangle mesh.
                for (std::size_t i = 0; i + 23 < vertices->size(); i += 24)
                {
                    osg::BoundingBox bounds;
                    for (std::size_t j = i; j < i + 24; ++j) bounds.expandBy((*vertices)[j]);
                    result.mSize = std::max(result.mSize, std::min(bounds.xMax() - bounds.xMin(),
                        std::min(bounds.yMax() - bounds.yMin(), bounds.zMax() - bounds.zMin())));
                }
            }
            result.mWidth = maxY - minY;
            result.mEmitted = trail.emittedCount(); result.mParticles = trail.particleCount();
            require(result.mParticles <= 192, "Stronger blood sprays exceeded their live droplet budget");
            return result;
        };
        const auto normal = measure(1, false, false), high = measure(3, false, false);
        require(high.mEmitted >= normal.mEmitted * 2, "Maximum blood failed to increase actual visible droplet count");
        require(high.mForward > normal.mForward * 1.4f,
            "Blood slider only increased concentration, without increasing launch distance");
        require(high.mWidth > normal.mWidth * 1.5f,
            "Blood slider only increased concentration, without widening the spray");
        const auto neck = measure(1, true, false), major = measure(1, false, true);
        require(neck.mEmitted > major.mEmitted && major.mEmitted > normal.mEmitted,
            "Severed neck and torso cuts did not emit stronger streams than ordinary wounds");
        require(neck.mForward > normal.mForward * 1.4f && major.mForward > normal.mForward * 1.2f,
            "Severed head/torso blood lacked a stronger physical launch");
        require(normal.mSize > .8f && neck.mSize > normal.mSize,
            "Airborne blood remained pin-sized or neck drops lost their visible volume");
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            const auto floor = MWRender::BloodTrail::visualSurface(floorSnapshot({}, 1000));
            unsigned deposits = 0;
            MWRender::BloodTrail fountain(*root, floor.mRaycast, [&](const auto&) { ++deposits; }, floor.mProjector);
            const std::vector<MWRender::BloodTrail::Source> source{ { { 0, 0, 20 }, { 0, 0, 1 }, 1, true } };
            for (int frame = 0; frame < 5; ++frame) fountain.update(.01f, source, 3, .14f + frame * .01f);
            for (int frame = 0; frame < 325; ++frame) fountain.update(.01f, {}, 3, .19f + frame * .01f);
            require(deposits > 0, "Strong maximum-slider neck droplets expired before their ballistic return to the floor");
            require(fountain.particleCount() == 0, "Stopped blood fountain leaked airborne droplets past their lifetime");
        }
        std::cout << "PASS slider-dependent spray distance/spread, stronger visible neck/torso jets and bounded droplets\n";
    }

    void bloodVisibleMeshReceiversAndProjection()
    {
        const auto quad = [](std::vector<osg::Vec3f>& triangles, const osg::Vec3f& a,
            const osg::Vec3f& b, const osg::Vec3f& c, const osg::Vec3f& d) {
            triangles.insert(triangles.end(), { a, b, c, a, c, d });
        };
        const auto node = [](const std::vector<osg::Vec3f>& triangles, unsigned mask) {
            osg::ref_ptr<osg::Geometry> mesh = new osg::Geometry;
            mesh->setVertexArray(new osg::Vec3Array(triangles.begin(), triangles.end()));
            mesh->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, triangles.size()));
            osg::ref_ptr<osg::Geode> result = new osg::Geode;
            result->setNodeMask(mask); result->addDrawable(mesh); return result;
        };
        osg::ref_ptr<osg::Group> scene = new osg::Group;
        std::vector<osg::Vec3f> stairs;
        for (int step = 0; step < 3; ++step)
        {
            const float x = -30.f + step * 20, z = step * 8.f;
            quad(stairs, { x, -30, z }, { x + 20, -30, z }, { x + 20, 30, z }, { x, 30, z });
            if (step)
                quad(stairs, { x, -30, z - 8 }, { x, -30, z }, { x, 30, z }, { x, 30, z - 8 });
        }
        scene->addChild(node(stairs, MWRender::Mask_Static));
        std::vector<osg::Vec3f> flat;
        quad(flat, { -8, -8, 0 }, { 8, -8, 0 }, { 8, 8, 0 }, { -8, 8, 0 });
        const auto placed = [&](const osg::Vec3f& position, unsigned mask, unsigned drawableMask = ~0u) {
            osg::ref_ptr<osg::MatrixTransform> transform = new osg::MatrixTransform;
            transform->setNodeMask(mask);
            transform->setMatrix(osg::Matrixf::translate(position));
            transform->addChild(node(flat, drawableMask)); scene->addChild(transform);
        };
        // A collision-inactive NIF uses Mask_Effect below its world-object
        // ancestor, although its visible carpet triangles still receive blood.
        placed({ 50, 0, 5 }, MWRender::Mask_Object, MWRender::Mask_Effect);
        placed({ -50, 0, 20 }, MWRender::Mask_Object); // A furniture top, above the architectural floor.
        placed({ 80, 0, 40 }, MWRender::Mask_Actor);
        placed({ 100, 0, 50 }, MWRender::Mask_Effect);
        placed({ 65, 0, 10 }, 0);
        const auto visible = MWRender::collectBloodSurfaceTriangles(*scene, { 0, 0, 10 }, 128);
        const auto surface = MWRender::BloodTrail::visualSurface(visible);
        require(bool(surface), "Visible furniture/carpet/stair geometry did not create a blood receiver snapshot");
        osg::Vec3f point, normal;
        require(surface.mRaycast({ -15, 0, 100 }, { -15, 0, -20 }, point, normal)
            && std::abs(point.z()) < .001f, "Blood used a smooth stair collision ramp instead of the rendered tread");
        // The deliberately different collision mesh must never be queried by
        // the visible-surface path. It would report z=4 at this same position.
        std::vector<osg::Vec3f> collisionRamp;
        quad(collisionRamp, { -30, -30, 0 }, { 30, -30, 16 }, { 30, 30, 16 }, { -30, 30, 0 });
        const auto physics = MWRender::BloodTrail::collisionRaycast(collisionRamp);
        require(physics({ -15, 0, 100 }, { -15, 0, -20 }, point, normal) && std::abs(point.z() - 4) < .001f,
            "Stair regression fixture did not differ from the visible model");
        for (const auto& receiver : { osg::Vec3f(50, 0, 5), osg::Vec3f(-50, 0, 20) })
            require(surface.mRaycast(receiver + osg::Vec3f(0, 0, 30), receiver - osg::Vec3f(0, 0, 10), point, normal)
                && (point - receiver).length() < .001f,
                "Render-only carpet or furniture was skipped or its world transform was lost");
        for (const float x : { 65.f, 80.f, 100.f })
            require(!surface.mRaycast({ x, 0, 100 }, { x, 0, -20 }, point, normal),
                "Hidden objects, actors or effects leaked into static blood receivers");

        // Every output triangle must remain on one real source face. Checking
        // all three vertices rejects a centre/rim fan that bridges stairs or
        // chords across a cylindrical column even when the rim itself hits.
        const auto onSourceFace = [](const std::array<osg::Vec3f, 3>& output,
            const std::vector<osg::Vec3f>& original) {
            for (std::size_t i = 0; i + 2 < original.size(); i += 3)
            {
                const auto edge0 = original[i + 1] - original[i], edge1 = original[i + 2] - original[i];
                auto normal = edge0 ^ edge1;
                if (normal.normalize() < .001f) continue;
                const float d00 = edge0 * edge0, d01 = edge0 * edge1, d11 = edge1 * edge1;
                const float denominator = d00 * d11 - d01 * d01;
                bool inside = denominator > .00001f;
                for (const auto& point : output)
                {
                    const auto relative = point - original[i];
                    const float height = relative * normal;
                    const float u = (d11 * (relative * edge0) - d01 * (relative * edge1)) / denominator;
                    const float v = (d00 * (relative * edge1) - d01 * (relative * edge0)) / denominator;
                    inside &= std::abs(height) <= .081f && u >= -.001f && v >= -.001f && u + v <= 1.001f;
                }
                if (inside) return true;
            }
            return false;
        };
        const auto checkProjection = [&](const MWRender::BloodTrail::Surface& receiver,
            const std::vector<osg::Vec3f>& mesh, const osg::Vec3f& center, const osg::Vec3f& normal, float radius) {
            const auto projected = receiver.mProjector(center, normal, radius);
            require(!projected.empty() && projected.size() % 3 == 0
                && projected.size() <= ESM::BrutalBloodState::MaxVerticesPerStain,
                "Visible blood projection was empty, malformed or exceeded its vertex budget");
            for (std::size_t i = 0; i < projected.size(); i += 3)
            {
                for (std::size_t j = i; j < i + 3; ++j)
                    require(std::isfinite(projected[j].length2()), "Visible blood projection emitted nonfinite vertices");
                require(onSourceFace({ projected[i], projected[i + 1], projected[i + 2] }, mesh),
                    "Blood projection bridged source faces instead of clipping directly onto the visible model");
            }
            return projected;
        };
        const auto stairProjection = checkProjection(surface, visible, { 0, 0, 8 }, { 0, 0, 1 }, 28);
        std::set<int> heights;
        for (const auto& p : stairProjection) heights.insert(int(std::round(p.z())));
        require(heights.size() > 1, "Visible stair projection stayed on one invisible flat ramp");
        checkProjection(surface, visible, { 50, 0, 5 }, { 0, 0, 1 }, 6);
        checkProjection(surface, visible, { -50, 0, 20 }, { 0, 0, 1 }, 6);

        std::vector<osg::Vec3f> column;
        for (int slice = 0; slice < 32; ++slice)
        {
            const float a = slice * 6.2831853f / 32, b = (slice + 1) * 6.2831853f / 32;
            const osg::Vec3f bottomA(std::cos(a) * 20, std::sin(a) * 20, -25);
            const osg::Vec3f bottomB(std::cos(b) * 20, std::sin(b) * 20, -25);
            quad(column, bottomA, bottomB, bottomB + osg::Vec3f(0, 0, 50), bottomA + osg::Vec3f(0, 0, 50));
        }
        const auto columnSurface = MWRender::BloodTrail::visualSurface(column);
        const auto cylindrical = checkProjection(columnSurface, column, { 20, 0, 0 }, { 1, 0, 0 }, 12);
        float minimumX = 20;
        for (const auto& p : cylindrical) minimumX = std::min(minimumX, p.x());
        require(minimumX < 19, "Column blood remained a floating plane instead of following curved facets");
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            MWRender::BloodDecals decals(*root);
            MWRender::BloodStamp stamp;
            stamp.mCenter = { 20, 0, 0 }; stamp.mNormal = { 1, 0, 0 }; stamp.mRadius = 12;
            stamp.mTriangles = cylindrical;
            decals.setCellActive("column", true); decals.add("column", stamp); decals.update(.1f, 1200);
            const auto* mesh = root->getChild(0)->asGeode()->getDrawable(0)->asGeometry();
            const auto* normals = static_cast<const osg::Vec3Array*>(mesh->getNormalArray());
            require(normals && normals->size() == cylindrical.size(), "Projected column blood lost its surface normals");
            for (std::size_t i = 0; i < cylindrical.size(); i += 3)
            {
                auto normal = (cylindrical[i + 1] - cylindrical[i]) ^ (cylindrical[i + 2] - cylindrical[i]);
                normal.normalize();
                require(std::abs((*normals)[i] * normal) > .999f,
                    "Blood lighting used one flat normal across a curved visible receiver");
            }
            const auto record = decals.getSaveRecord();
            decals.loadSaveRecord(record);
            require(decals.saveState().front().mStamp.mTriangles == cylindrical,
                "Saving projected blood lost the fitted stair/column geometry");
        }
        std::vector<osg::Vec3f> dense;
        for (int x = -16; x < 16; ++x) for (int y = -16; y < 16; ++y)
            quad(dense, { float(x), float(y), 0 }, { float(x + 1), float(y), 0 },
                { float(x + 1), float(y + 1), 0 }, { float(x), float(y + 1), 0 });
        const auto denseSurface = MWRender::BloodTrail::visualSurface(dense);
        checkProjection(denseSurface, dense, {}, { 0, 0, 1 }, 12);
        std::vector<osg::Vec3f> parallel;
        for (const float z : { 0.f, -2.f, 30.f })
            quad(parallel, { -20, -20, z }, { 20, -20, z }, { 20, 20, z }, { -20, 20, z });
        const auto parallelSurface = MWRender::BloodTrail::visualSurface(parallel);
        const auto frontOnly = checkProjection(parallelSurface, parallel, {}, { 0, 0, 1 }, 10);
        for (const auto& p : frontOnly)
            require(std::abs(p.z()) < .1f,
                "Blood jumped to overhead furniture or projected through a visible surface onto hidden geometry");
        std::vector<osg::Vec3f> carpet;
        for (const float z : { 0.f, .01f })
            quad(carpet, { -20, -20, z }, { 20, -20, z }, { 20, 20, z }, { -20, 20, z });
        const auto carpetSurface = MWRender::BloodTrail::visualSurface(carpet);
        const auto carpetOnly = checkProjection(carpetSurface, carpet, { 0, 0, .01f }, { 0, 0, 1 }, 10);
        for (const auto& p : carpetOnly)
            require(std::abs(p.z() - .04f) < .001f,
                "Blood duplicated its patch on the hidden floor directly below a thin carpet");
        require(columnSurface.mProjector({ 20, 0, 0 }, {}, 12).empty()
            && columnSurface.mProjector({ 20, 0, 0 }, { 1, 0, 0 }, std::numeric_limits<float>::quiet_NaN()).empty(),
            "Invalid visual decal normal/radius was not rejected");
        auto invalid = column; invalid.front().x() = std::numeric_limits<float>::quiet_NaN();
        require(!MWRender::BloodTrail::visualSurface(invalid), "Nonfinite visual receiver mesh was accepted");
        require(!MWRender::BloodTrail::visualSurface(std::vector<osg::Vec3f>(6)),
            "A degenerate visible receiver mesh created an unusable blood surface");
        std::cout << "PASS rendered stairs/cylinders, visual-only carpet/furniture, transform/mask filtering and bounded face-clipped blood\n";
    }

    void bloodTexturesAndVisualIdentity()
    {
        std::vector<osg::ref_ptr<osg::Image>> floors, walls;
        for (const char* name : { "floor06.png", "floor07.png", "wall01.png", "wall02.png", "wall03.png", "wall04.png" })
        {
            const auto image = osgDB::readRefImageFile(std::string(RAGDOLL_TEST_ASSET_DIR) + "/" + name);
            require(image && image->getPixelFormat() == GL_RGBA, "Packaged blood PNG failed to load with RGBA transparency");
            bool transparent = false, visible = false;
            for (int y = 0; y < image->t(); y += 7)
                for (int x = 0; x < image->s(); x += 7)
                {
                    const float alpha = image->getColor(x, y).a();
                    transparent |= alpha < .01f; visible |= alpha > .5f;
                }
            require(transparent && visible, "Blood image lost its alpha silhouette");
            if (image->getOrigin() == osg::Image::BOTTOM_LEFT) image->flipVertical();
            image->setOrigin(osg::Image::TOP_LEFT);
            (name[0] == 'f' ? floors : walls).push_back(image);
        }
        osg::ref_ptr<osg::Group> actual = new osg::Group;
        MWRender::BloodDecals actualDecals(*actual);
        actualDecals.setTextures(floors, walls);
        actualDecals.setCellActive("actual", true);
        actualDecals.add("actual", bloodQuad(true, 0, 8));
        actualDecals.add("actual", bloodQuad(false, 30, 8));
        actualDecals.update(.1f, 1200);
        auto* actualTexture = dynamic_cast<osg::Texture2D*>(actual->getChild(0)->getStateSet()->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
        require(actualTexture && actualTexture->getImage() && actualTexture->getImage()->s() <= 2048
            && actualTexture->getImage()->t() <= 2048, "Blood atlas lost its bounded shared texture");

        osg::ref_ptr<osg::Group> scene = new osg::Group;
        MWRender::BloodDecals decals(*scene);
        const auto bottom = bloodMarkerImage(osg::Image::BOTTOM_LEFT), top = bloodMarkerImage(osg::Image::TOP_LEFT);
        decals.setTextures({ bottom, bottom }, { top, bottom });
        decals.setCellActive("marker", true);
        for (unsigned seed : { 8u, 9u, 10u, 11u, 4u }) decals.add("marker", bloodQuad(false, float(seed) * 30, seed));
        for (unsigned seed : { 8u, 9u, 10u, 11u }) decals.add("marker", bloodQuad(true, float(seed) * 30, seed));
        decals.update(.1f, 1200);
        auto* mesh = scene->getChild(0)->asGeode()->getDrawable(0)->asGeometry();
        auto* uv = static_cast<osg::Vec2Array*>(mesh->getTexCoordArray(0));
        auto* info = static_cast<osg::Vec2Array*>(mesh->getTexCoordArray(1));
        auto* colors = static_cast<osg::Vec4Array*>(mesh->getColorArray());
        auto* texture = dynamic_cast<osg::Texture2D*>(scene->getChild(0)->getStateSet()->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
        require(uv && info && colors && texture && uv->size() == 54 && info->size() == uv->size()
            && colors->size() == uv->size(), "Textured blood vertex attributes are incomplete");
        const auto* atlas = texture->getImage();
        const auto sample = [&](int patch, float u, float v) {
            const auto index = patch * 6;
            const auto coordinate = (*uv)[index] + ((*uv)[index + 1] - (*uv)[index]) * u
                + ((*uv)[index + 5] - (*uv)[index]) * v;
            return atlas->getColor(int(coordinate.x() * atlas->s()), int(coordinate.y() * atlas->t()));
        };
        const std::array expected{ osg::Vec4f(1,0,0,1), osg::Vec4f(0,1,0,1), osg::Vec4f(1,0,0,1),
            osg::Vec4f(0,1,0,1), osg::Vec4f(1,0,0,1), osg::Vec4f(1,0,0,1), osg::Vec4f(0,1,0,1),
            osg::Vec4f(1,1,0,1), osg::Vec4f(0,0,1,1) };
        for (int patch = 0; patch < 9; ++patch)
        {
            require((sample(patch, .25f, .75f) - expected[patch]).length() < .05f,
                "Wall orientation/origin, horizontal mirror or floor 180-degree rotation is incorrect");
            require(sample(patch, .01f, .01f).a() < .01f, "Transparent blood corners became solid rectangles");
            require((*info)[patch * 6].y() > .5f, "Atlas patch bypassed texture sampling");
        }
        for (int patch = 0; patch < 5; ++patch)
            require((*uv)[patch * 6 + 5].y() > (*uv)[patch * 6].y(), "Wall drips were rotated away from world gravity");
        require((*uv)[0].x() > .49f && (*uv)[4 * 6].x() > .74f && (*uv)[5 * 6].x() < .26f,
            "Wall/floor variants were selected from the wrong atlas bank");
        float minimumRed = 1, maximumRed = 0;
        for (int patch = 0; patch < 9; ++patch)
        {
            const auto& color = (*colors)[patch * 6];
            require(color.r() > .4f && color.r() < .8f && color.g() < color.r() && color.b() > color.g(),
                "Textured blood bypassed its darker burgundy tint");
            for (int vertex = 1; vertex < 6; ++vertex)
                require((*colors)[patch * 6 + vertex] == color, "A stain changed shade within its own surface");
            minimumRed = std::min(minimumRed, color.r()); maximumRed = std::max(maximumRed, color.r());
        }
        require(maximumRed - minimumRed > .002f && maximumRed - minimumRed < .15f,
            "Blood stain shades lost their subtle per-decal variation");
        const std::vector<osg::Vec2f> coordinatesBefore(uv->begin(), uv->end());
        const std::vector<osg::Vec4f> colorsBefore(colors->begin(), colors->end());
        const auto* vertices = static_cast<const osg::Vec3Array*>(mesh->getVertexArray());
        const std::vector<osg::Vec3f> verticesBefore(vertices->begin(), vertices->end());
        const auto saved = decals.getSaveRecord();
        decals.loadSaveRecord(saved);
        auto* restored = scene->getChild(0)->asGeode()->getDrawable(0)->asGeometry();
        const auto* after = static_cast<const osg::Vec2Array*>(restored->getTexCoordArray(0));
        require(coordinatesBefore.size() == after->size() && std::equal(coordinatesBefore.begin(), coordinatesBefore.end(), after->begin()),
            "Loading a save rerolled stain variant/orientation/size");
        const auto* colorsAfter = static_cast<const osg::Vec4Array*>(restored->getColorArray());
        const auto* verticesAfter = static_cast<const osg::Vec3Array*>(restored->getVertexArray());
        require(colorsBefore.size() == colorsAfter->size() && std::equal(colorsBefore.begin(), colorsBefore.end(), colorsAfter->begin()),
            "Loading a save rerolled the blood stain's burgundy shade");
        require(verticesBefore.size() == verticesAfter->size() && std::equal(verticesBefore.begin(), verticesBefore.end(), verticesAfter->begin()),
            "Loading a save enlarged a blood stain for a second time");
        require(decals.getSaveRecord().mStains[0].mVisualSeed == 8u, "Saved decal identity was lost");
        std::set<std::uint32_t> identities;
        for (int i = 0; i < 32; ++i)
        {
            auto stamp = bloodQuad(i % 2 == 0, i * 30.f, 0);
            decals.add("marker", stamp);
        }
        for (const auto& state : decals.saveState()) identities.insert(state.mStamp.mVisualSeed);
        require(identities.size() > 20 && !identities.contains(0), "Automatic stain variants repeat a single random identity");
        std::cout << "PASS original RGBA PNGs, atlas banks, wall gravity, image origins, floor 180, mirrors, burgundy shade variation and saved visual identity\n";
    }

    void organicCreatureBloodAndDeathTrigger()
    {
        for (const char* model : { "c\\guar.nif", "c\\nix_hound.nif", "c\\mudcrab.nif", "c\\clannfear.nif",
                 "c\\rat.nif", "c\\bonewalker.nif", "c\\draugr.nif", "c\\ghostly_guar.nif" })
            require(Ragdoll::creatureHasBlood("generated001", "custom_actor", model), "Organic creature lost its blood");
        for (const char* model : { "c\\steam_centurion.nif", "c\\spherecenturion.nif", "c\\dwarven_spider.nif",
                 "c\\skeleton.nif", "c\\bonelord.nif", "c\\ancestor_ghost.nif", "c\\wraith.nif",
                 "c\\atronach_frost.nif", "c\\fabricant.nif" })
            require(!Ragdoll::creatureHasBlood("generated001", "custom_actor", model), "Bloodless model emitted organic blood");
        require(!Ragdoll::creatureHasBlood("generated001", "skeleton_0001", "custom\\actor.nif"),
            "Generated bloodless record ignored its original record ID");
        require(!Ragdoll::creatureHasBlood("Dwemer_Spider03", "", "custom\\actor.nif"), "Case or numeric suffix bypassed blood exclusion");
        Ragdoll::DeathBloodTrigger trigger;
        require(!trigger.update(true) && !trigger.update(true), "Loading an existing corpse started a new fountain");
        require(!trigger.update(false) && trigger.update(true) && !trigger.update(true), "A new death did not emit exactly one fountain");
        require(!trigger.update(false) && trigger.update(true) && !trigger.update(true), "Resurrection did not rearm death blood");
        std::cout << "PASS organic creatures, bloodless models/original IDs, loaded corpses and once-per-life fountains\n";
    }

    void bloodGeometryBudget()
    {
        osg::ref_ptr<osg::Group> scene = new osg::Group;
        MWRender::BloodDecals decals(*scene);
        decals.setCellActive("arena", true);
        MWRender::BloodStamp stamp;
        stamp.mNormal = { 0, 0, 1 };
        constexpr std::size_t verticesPerStain = ESM::BrutalBloodState::MaxVerticesPerStain;
        const std::size_t fitting = MWRender::BloodDecals::MaxVertices / verticesPerStain;
        for (std::size_t i = 0; i < fitting + 256; ++i)
        {
            stamp.mCenter = { float(i), 0, 0 };
            stamp.mTriangles.clear();
            for (std::size_t j = 0; j < verticesPerStain / 3; ++j)
                stamp.mTriangles.insert(stamp.mTriangles.end(), { stamp.mCenter,
                    stamp.mCenter + osg::Vec3f(1, 0, 0), stamp.mCenter + osg::Vec3f(0, 1, 0) });
            decals.add("arena", stamp);
        }
        decals.update(.1f, 1200);
        require(decals.size() == fitting && decals.saveState().front().mStamp.mCenter.x() == 256,
            "The extended retention cache exceeded the baseline geometry memory budget");
        decals.clear();
        require(scene->getNumChildren() == 0, "Geometry-budget eviction left render nodes alive");
        std::cout << "PASS doubled-blood retention and bounded maximum-detail geometry allocation\n";
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
        for (int i = 0; i < int(MWRender::BloodDecals::MaxStains + 2048); ++i)
        {
            stamp.mCenter.set(float(i), 0, 0);
            stamp.mTriangles.clear();
            for (int edge = 0; edge < 8; ++edge)
            {
                const float a = edge * 6.2831853f / 8, b = (edge + 1) * 6.2831853f / 8;
                stamp.mTriangles.insert(stamp.mTriangles.end(), { stamp.mCenter,
                    stamp.mCenter + osg::Vec3f(std::cos(a), std::sin(a), 0),
                    stamp.mCenter + osg::Vec3f(std::cos(b), std::sin(b), 0) });
            }
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
            auto* mesh = scene->getChild(i)->asGeode()->getDrawable(0)->asGeometry();
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
        std::cout << "PASS full bounded stain-cache rollover, stable VBOs while aging, local chunk updates, save/load and cleanup\n";
    }

    void goreLoadMeasurements(bool benchmark)
    {
        osg::ref_ptr<osg::Group> scene = new osg::Group;
        MWRender::BloodDecals decals(*scene);
        decals.setCellActive("arena", true);
        // This is deliberately separate from the flat Bullet floor used by
        // corpse physics. Blood exercises the actual visual collector, shared
        // BVH and face clipping under the same ten-corpse load as the runtime.
        osg::ref_ptr<osg::Geode> receiver = new osg::Geode;
        receiver->setNodeMask(MWRender::Mask_Static);
        osg::ref_ptr<osg::Vec3Array> receiverVertices = new osg::Vec3Array;
        const auto receiverQuad = [&](const osg::Vec3f& a, const osg::Vec3f& b,
            const osg::Vec3f& c, const osg::Vec3f& d) {
            for (const auto& vertex : { a, b, c, a, c, d }) receiverVertices->push_back(vertex);
        };
        for (int x = -1000; x < 1000; x += 40) for (int y = -1000; y < 1000; y += 40)
            receiverQuad({ float(x), float(y), 0 }, { float(x + 40), float(y), 0 },
                { float(x + 40), float(y + 40), 0 }, { float(x), float(y + 40), 0 });
        for (int step = 0; step < 5; ++step)
        {
            const float y = 100.f + step * 20, z = 8.f + step * 8;
            receiverQuad({ -600, y, z }, { 600, y, z }, { 600, y + 20, z }, { -600, y + 20, z });
        }
        osg::ref_ptr<osg::Geometry> receiverMesh = new osg::Geometry;
        receiverMesh->setVertexArray(receiverVertices);
        receiverMesh->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, receiverVertices->size()));
        receiver->addDrawable(receiverMesh);
        const auto receiverSurface = MWRender::BloodTrail::visualSurface(
            MWRender::collectBloodSurfaceTriangles(*receiver, {}, 1500));
        require(bool(receiverSurface), "Concurrent gore failed to build its real visible-surface blood snapshot");
        unsigned surfaceRequests = 0;
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
                config.mBloodSurface = [&, receiverSurface](const auto&, float) {
                    ++surfaceRequests; return receiverSurface;
                };
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
                              << " stains=" << decals.size() << " surface_requests=" << surfaceRequests
                              << " mean_ms=" << total / 60 << " max_ms=" << maximum << std::endl;
                total = maximum = 0;
            }
        }
        require(tubes.size() == 20, "Concurrent waist/longitudinal/hammer deaths lost their physical intestines");
        require(surfaceRequests >= corpses.size(), "Concurrent gore stress bypassed the visual blood-surface callbacks");
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
        bloodLaunchRangeAndVisibleWounds();
        bloodVisibleMeshReceiversAndProjection();
        bloodTexturesAndVisualIdentity();
        organicCreatureBloodAndDeathTrigger();
        bloodGeometryBudget();
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
