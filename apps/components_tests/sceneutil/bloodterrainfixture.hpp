#ifndef OPENMW_TEST_BLOOD_TERRAIN_FIXTURE_HPP
#define OPENMW_TEST_BLOOD_TERRAIN_FIXTURE_HPP
#include <apps/openmw/mwrender/brutalblood.hpp>
#include <apps/openmw/mwrender/bloodsurfacevisitor.hpp>
#include <apps/openmw/mwrender/vismask.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/terrain/quadtreeworld.hpp>
#include <components/terrain/terraingrid.hpp>
#include <components/terrain/storage.hpp>
#include <components/toutf8/toutf8.hpp>
#include <components/vfs/manager.hpp>
#include <osg/FrameStamp>
#include <osg/Group>
#include <cmath>
#include <memory>
namespace BloodTest
{
    // Exercise the real terrain tree, chunk selection, TerrainDrawable and
    // index buffers. No meshes or Morrowind data are needed for this storage.
    class ExteriorBloodStorage final : public Terrain::Storage
    {
    public:
        int mX, mY;
        float mSlopeX, mSlopeY;
        unsigned mFilledChunks = 0;
        bool mRough = false;

        ExteriorBloodStorage(int x, int y, float slopeX, float slopeY, bool rough = false)
            : mX(x), mY(y), mSlopeX(slopeX), mSlopeY(slopeY), mRough(rough) {}

        osg::Vec3f center() const { return { (mX + 1.f) * 8192.f, (mY + 1.f) * 8192.f, 512.f }; }
        float height(float x, float y) const
        {
            const float dx = x - center().x(), dy = y - center().y();
            return 512.f + dx * mSlopeX + dy * mSlopeY
                + (mRough ? 18.f * std::sin(dx / 96.f) * std::cos(dy / 128.f)
                    + 12.f * std::sin((dx + dy) / 180.f) : 0.f);
        }
        void getBounds(float& minX, float& maxX, float& minY, float& maxY, ESM::RefId) override
        {
            minX = mX; maxX = mX + 2; minY = mY; maxY = mY + 2;
        }
        bool getMinMaxHeights(float size, const osg::Vec2f& chunk, ESM::RefId,
            float& minimum, float& maximum) override
        {
            const float heightAtCenter = height(chunk.x() * 8192.f, chunk.y() * 8192.f);
            const float variation = (std::abs(mSlopeX) + std::abs(mSlopeY)) * size * 4096.f + (mRough ? 64.f : 0.f);
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
        std::unique_ptr<Terrain::World> mTerrain;

        ExteriorBloodScene(int x, int y, float slopeX = 0, float slopeY = 0, bool distant = true, bool rough = false)
            : mStorage(x, y, slopeX, slopeY, rough)
        {
            // Empty RefId means an ESM4 world to OpenMW. This must use the
            // actual TES3 worldspace so both storage and tree use 8192 units.
            if (distant)
            {
                auto terrain = std::make_unique<Terrain::QuadTreeWorld>(mRoot, mCompile, &mResources, &mStorage,
                    MWRender::Mask_Terrain, MWRender::Mask_PreCompile, MWRender::Mask_Debug,
                    128, 8, 1, 0, 1, false, ESM::Cell::sDefaultWorldspaceId, 1);
                terrain->setViewDistance(16384);
                terrain->setActiveGrid({ x, y, x + 2, y + 2 });
                terrain->enable(true);
                mTerrain = std::move(terrain);
            }
            else
            {
                mTerrain = std::make_unique<Terrain::TerrainGrid>(mRoot, mCompile, &mResources, &mStorage,
                    MWRender::Mask_Terrain, ESM::Cell::sDefaultWorldspaceId, 1,
                    MWRender::Mask_PreCompile, MWRender::Mask_Debug);
                for (int dx = 0; dx < 2; ++dx)
                    for (int dy = 0; dy < 2; ++dy) mTerrain->loadCell(x + dx, y + dy);
            }
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

}
#endif
