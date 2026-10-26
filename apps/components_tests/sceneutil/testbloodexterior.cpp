#include "bloodterrainfixture.hpp"
#include <apps/openmw/mwrender/brutalblood.hpp>
#include <apps/openmw/mwrender/bloodsurfacevisitor.hpp>
#include <apps/openmw/mwrender/bloodterrain.hpp>
#include <apps/openmw/mwrender/vismask.hpp>

#include <components/esm3/loadcell.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/terrain/quadtreeworld.hpp>
#include <components/terrain/storage.hpp>
#include <components/toutf8/toutf8.hpp>
#include <components/vfs/manager.hpp>

#include <gtest/gtest.h>

#include <osg/FrameStamp>
#include <osg/Group>

#include <cmath>
#include <memory>

namespace
{
    using BloodTest::ExteriorBloodScene;

    TEST(BloodExteriorTerrain, directLandReceiversMatchNativeChunkHeightsAcrossWorldCoordinates)
    {
        for (const auto& cell : { osg::Vec2i(-4, -3), osg::Vec2i(0, 0), osg::Vec2i(62, -23) })
        {
            BloodTest::ExteriorBloodStorage storage(cell.x(), cell.y(), .2f, -.1f);
            const auto center = storage.center();
            const auto triangles = MWRender::collectBloodTerrainTriangles(storage,
                ESM::Cell::sDefaultWorldspaceId, center + osg::Vec3f(0, 0, 80), 900);
            ASSERT_FALSE(triangles.empty());
            EXPECT_LE(storage.mFilledChunks, 16u);
            const auto surface = MWRender::BloodTrail::visualSurface(triangles);
            ASSERT_TRUE(bool(surface));
            for (int x = -2; x <= 2; ++x)
                for (int y = -2; y <= 2; ++y)
                {
                    const float px = center.x() + x * 147.25f, py = center.y() + y * 163.5f;
                    const float height = storage.height(px, py);
                    osg::Vec3f point, normal;
                    ASSERT_TRUE(surface.mRaycast({ px, py, height + 100 }, { px, py, height - 100 }, point, normal));
                    EXPECT_NEAR(point.z(), height, .08f);
                    const auto projected = surface.mProjector(point, normal, 48);
                    ASSERT_FALSE(projected.empty());
                    for (const auto& vertex : projected)
                        EXPECT_NEAR((vertex.z() - storage.height(vertex.x(), vertex.y()))
                            / std::sqrt(1 + .2f * .2f + .1f * .1f), .03f, .045f);
                }
            EXPECT_TRUE(MWRender::collectBloodTerrainTriangles(storage,
                ESM::Cell::sDefaultWorldspaceId, center, 0).empty());
        }
    }

    TEST(BloodExteriorTerrain, realNativeTerrainReceiversCoverLargeWorldPositionsAndCellBoundaries)
    {
        for (const auto& cell : { osg::Vec2i(-6, -4), osg::Vec2i(0, 0), osg::Vec2i(62, -23) })
        {
            ExteriorBloodScene scene(cell.x(), cell.y(), .2f, -.1f);
            const auto surface = scene.surface();
            ASSERT_TRUE(bool(surface));
            EXPECT_GT(scene.mStorage.mFilledChunks, 0u);
            // Only nearby chunks should be constructed; the blood snapshot
            // must not cause terrain loading across the full view distance.
            EXPECT_LE(scene.mStorage.mFilledChunks, 16u);
            const auto center = scene.mStorage.center();
            for (int x = -2; x <= 2; ++x)
                for (int y = -2; y <= 2; ++y)
                {
                    const float px = center.x() + x * 147.25f, py = center.y() + y * 163.5f;
                    const float height = scene.mStorage.height(px, py);
                    osg::Vec3f point, normal;
                    ASSERT_TRUE(surface.mRaycast({ px, py, height + 100 }, { px, py, height - 100 }, point, normal));
                    EXPECT_NEAR(point.z(), height, .08f);
                    EXPECT_GT(normal.z(), .9f);
                    const auto projected = surface.mProjector(point, normal, 24);
                    ASSERT_FALSE(projected.empty());
                    EXPECT_LE(projected.size(), ESM::BrutalBloodState::MaxVerticesPerStain);
                    for (const auto& vertex : projected)
                    {
                        const float distance = (vertex.z() - scene.mStorage.height(vertex.x(), vertex.y()))
                            / std::sqrt(1 + .2f * .2f + .1f * .1f);
                        EXPECT_NEAR(distance, .03f, .045f);
                    }
                }
        }
    }

    TEST(BloodExteriorTerrain, lyingTorsoWoundsCanReachSlopedGroundWithoutAnEighteenUnitCutoff)
    {
        ExteriorBloodScene scene(-6, -4, .15f, .1f);
        const auto surface = scene.surface();
        ASSERT_TRUE(bool(surface));
        const auto center = scene.mStorage.center();
        for (int corpse = 0; corpse < 10; ++corpse)
        {
            const float x = center.x() - 250 + corpse * 50.f, y = center.y() + 80;
            const osg::Vec3f source(x, y, scene.mStorage.height(x, y) + 32.f);
            osg::Vec3f point, normal;
            EXPECT_FALSE(surface.mRaycast(source, source - osg::Vec3f(0, 0, 18), point, normal));
            ASSERT_TRUE(surface.mRaycast(source,
                source - osg::Vec3f(0, 0, MWRender::BloodTrail::GroundPoolProbe), point, normal));
            EXPECT_GT(normal.z(), .65f);
            EXPECT_FALSE(surface.mProjector(point, normal, 36).empty());
        }
    }

    TEST(BloodExteriorTerrain, theDefaultTerrainGridReceivesBloodAcrossCellEdges)
    {
        ExteriorBloodScene scene(-4, -3, .2f, .1f, false);
        const auto surface = scene.surface();
        ASSERT_TRUE(bool(surface));
        const auto center = scene.mStorage.center();
        for (int x = -2; x <= 2; ++x)
            for (int y = -2; y <= 2; ++y)
            {
                const auto position = center + osg::Vec3f(x * 90.f, y * 90.f, 0);
                const float z = scene.mStorage.height(position.x(), position.y());
                osg::Vec3f point, normal;
                ASSERT_TRUE(surface.mRaycast({ position.x(), position.y(), z + 40 },
                    { position.x(), position.y(), z - 40 }, point, normal));
                EXPECT_NEAR(point.z(), z, .03f);
                EXPECT_FALSE(surface.mProjector(point, normal, 30).empty());
            }
    }

    TEST(BloodExteriorTerrain, aLargePagedDecorationMeshCannotExhaustTheGroundScanBudget)
    {
        ExteriorBloodScene scene(-6, -4);
        const auto center = scene.mStorage.center();
        osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array;
        // Paged exteriors merge many remote objects into a large drawable.
        // Its bounds contain the corpse, while its early triangles are far
        // outside the receiver query. It must not starve the later terrain.
        constexpr unsigned triangles = 131100;
        positions->reserve(triangles * 3);
        for (unsigned i = 0; i < triangles; ++i)
        {
            const float side = i % 2 ? 1.f : -1.f;
            const auto corner = center + osg::Vec3f(side * 6000, side * 6000, side * 6000);
            positions->push_back(corner);
            positions->push_back(corner + osg::Vec3f(100, 0, 0));
            positions->push_back(corner + osg::Vec3f(0, 100, 0));
        }
        osg::ref_ptr<osg::Geometry> decoration = new osg::Geometry;
        decoration->setVertexArray(positions);
        decoration->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, positions->size()));
        for (int chunk = 0; chunk < 5; ++chunk)
        {
            osg::ref_ptr<osg::Geode> paged = new osg::Geode;
            paged->setNodeMask(MWRender::Mask_Static);
            paged->addDrawable(decoration);
            scene.mRoot->addChild(paged);
        }

        osg::ref_ptr<osg::Vec3Array> pavingPositions = new osg::Vec3Array;
        for (const auto& corner : { osg::Vec3f(-200, -200, 64), osg::Vec3f(200, -200, 64),
                 osg::Vec3f(200, 200, 64), osg::Vec3f(-200, -200, 64),
                 osg::Vec3f(200, 200, 64), osg::Vec3f(-200, 200, 64) })
            pavingPositions->push_back(center + corner);
        osg::ref_ptr<osg::Geometry> paving = new osg::Geometry;
        paving->setVertexArray(pavingPositions);
        paving->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 6));
        osg::ref_ptr<osg::Geode> pavedFloor = new osg::Geode;
        pavedFloor->setNodeMask(MWRender::Mask_Static);
        pavedFloor->addDrawable(paving);
        scene.mRoot->addChild(pavedFloor);

        const auto surface = scene.surface();
        ASSERT_TRUE(bool(surface));
        osg::Vec3f point, normal;
        ASSERT_TRUE(surface.mRaycast(center + osg::Vec3f(0, 0, 80), center - osg::Vec3f(0, 0, 80), point, normal));
        EXPECT_NEAR(point.z(), center.z() + 64, .001f);
        EXPECT_FALSE(surface.mProjector(point, normal, 36).empty());

        // Production combines direct LAND with visible objects. Paved streets
        // and bridges must remain above LAND when duplicate terrain is skipped.
        auto receivers = MWRender::collectBloodTerrainTriangles(scene.mStorage,
            ESM::Cell::sDefaultWorldspaceId, center + osg::Vec3f(0, 0, 80), 900);
        const auto objects = MWRender::collectBloodSurfaceTriangles(*scene.mRoot,
            center + osg::Vec3f(0, 0, 80), 900, {}, nullptr, false);
        receivers.insert(receivers.end(), objects.begin(), objects.end());
        const auto directSurface = MWRender::BloodTrail::visualSurface(receivers);
        ASSERT_TRUE(bool(directSurface));
        ASSERT_TRUE(directSurface.mRaycast(center + osg::Vec3f(0, 0, 80),
            center - osg::Vec3f(0, 0, 80), point, normal));
        EXPECT_NEAR(point.z(), center.z() + 64, .001f);
        EXPECT_FALSE(directSurface.mProjector(point, normal, 36).empty());
    }
}
