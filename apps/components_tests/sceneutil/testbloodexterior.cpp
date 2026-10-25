#include <apps/openmw/mwrender/brutalblood.hpp>
#include <apps/openmw/mwrender/bloodsurfacevisitor.hpp>
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
    // Exercise the real terrain tree, chunk selection, TerrainDrawable and
    // index buffers. No meshes or Morrowind data are needed for this storage.
    class ExteriorBloodStorage final : public Terrain::Storage
    {
    public:
        int mX, mY;
        float mSlopeX, mSlopeY;
        unsigned mFilledChunks = 0;

        ExteriorBloodStorage(int x, int y, float slopeX, float slopeY)
            : mX(x), mY(y), mSlopeX(slopeX), mSlopeY(slopeY) {}

        osg::Vec3f center() const { return { (mX + 1.f) * 8192.f, (mY + 1.f) * 8192.f, 512.f }; }
        float height(float x, float y) const
        {
            return 512.f + (x - center().x()) * mSlopeX + (y - center().y()) * mSlopeY;
        }
        void getBounds(float& minX, float& maxX, float& minY, float& maxY, ESM::RefId) override
        {
            minX = mX; maxX = mX + 2; minY = mY; maxY = mY + 2;
        }
        bool getMinMaxHeights(float size, const osg::Vec2f& chunk, ESM::RefId,
            float& minimum, float& maximum) override
        {
            const float heightAtCenter = height(chunk.x() * 8192.f, chunk.y() * 8192.f);
            const float variation = (std::abs(mSlopeX) + std::abs(mSlopeY)) * size * 4096.f;
            minimum = heightAtCenter - variation; maximum = heightAtCenter + variation;
            return true;
        }
        void fillVertexBuffers(int lod, float size, const osg::Vec2f& chunk, ESM::RefId,
            osg::Vec3Array& positions, osg::Vec3Array& normals, osg::Vec4ubArray& colors) override
        {
            ++mFilledChunks;
            const int count = static_cast<int>(64 * size / (1 << lod)) + 1;
            osg::Vec3f normal(-mSlopeX, -mSlopeY, 1);
            normal.normalize();
            for (int col = 0; col < count; ++col)
                for (int row = 0; row < count; ++row)
                {
                    const float x = (col / float(count - 1) - .5f) * size * 8192.f;
                    const float y = (row / float(count - 1) - .5f) * size * 8192.f;
                    positions.push_back({ x, y, height(chunk.x() * 8192.f + x, chunk.y() * 8192.f + y) });
                    normals.push_back(normal);
                    colors.push_back({ 255, 255, 255, 255 });
                }
        }
        void getBlendmaps(float, const osg::Vec2f&, ImageVector&,
            std::vector<Terrain::LayerInfo>&, ESM::RefId) override {}
        float getHeightAt(const osg::Vec3f& position, ESM::RefId) override { return height(position.x(), position.y()); }
        float getCellWorldSize(ESM::RefId) override { return 8192; }
        int getCellVertices(ESM::RefId) override { return 65; }
        int getTextureTileCount(float, ESM::RefId) override { return 16; }
    };

    struct ExteriorBloodScene
    {
        const VFS::Manager mVfs;
        const ToUTF8::Utf8Encoder mEncoder{ ToUTF8::WINDOWS_1252 };
        Resource::ResourceSystem mResources{ &mVfs, 1, &mEncoder.getStatelessEncoder() };
        osg::ref_ptr<osg::Group> mRoot = new osg::Group;
        osg::ref_ptr<osg::Group> mCompile = new osg::Group;
        ExteriorBloodStorage mStorage;
        std::unique_ptr<Terrain::QuadTreeWorld> mTerrain;

        ExteriorBloodScene(int x, int y, float slopeX = 0, float slopeY = 0)
            : mStorage(x, y, slopeX, slopeY)
        {
            // Empty RefId means an ESM4 world to OpenMW. This must use the
            // actual TES3 worldspace so both storage and tree use 8192 units.
            mTerrain = std::make_unique<Terrain::QuadTreeWorld>(mRoot, mCompile, &mResources, &mStorage,
                MWRender::Mask_Terrain, MWRender::Mask_PreCompile, MWRender::Mask_Debug,
                128, 8, 1, 0, 1, false, ESM::Cell::sDefaultWorldspaceId, 1);
            mTerrain->setViewDistance(16384);
            mTerrain->setActiveGrid({ x, y, x + 2, y + 2 });
            mTerrain->enable(true);
        }

        MWRender::BloodTrail::Surface surface()
        {
            const auto center = mStorage.center() + osg::Vec3f(0, 0, 80);
            osg::ref_ptr<osg::FrameStamp> frame = new osg::FrameStamp;
            frame->setFrameNumber(1);
            frame->setReferenceTime(1);
            return MWRender::BloodTrail::visualSurface(
                MWRender::collectBloodSurfaceTriangles(*mRoot, center, 900, {}, frame));
        }
    };

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
    }
}
