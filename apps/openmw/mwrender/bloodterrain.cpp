#include "bloodterrain.hpp"
#include <components/terrain/storage.hpp>
#include <components/terrain/buffercache.hpp>
#include <osg/BoundingBox>
#include <osg/TriangleIndexFunctor>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace MWRender
{
    std::vector<osg::Vec3f> collectBloodTerrainTriangles(Terrain::Storage& storage,
        ESM::RefId worldspace, const osg::Vec3f& center, float radius)
    {
        std::vector<osg::Vec3f> result;
        if (!std::isfinite(center.length2()) || !std::isfinite(radius) || radius <= 0) return result;
        radius = std::min(radius, 2048.f);
        const float cellSize = storage.getCellWorldSize(worldspace), chunkSize = cellSize * .25f;
        const int vertices = storage.getCellVertices(worldspace);
        if (!std::isfinite(chunkSize) || chunkSize <= 0 || vertices < 5 || vertices > 257) return result;
        const auto firstX = std::floor((double(center.x()) - radius) / chunkSize);
        const auto firstY = std::floor((double(center.y()) - radius) / chunkSize);
        const auto lastX = std::floor((double(center.x()) + radius) / chunkSize);
        const auto lastY = std::floor((double(center.y()) + radius) / chunkSize);
        if (firstX < std::numeric_limits<int>::min() || firstY < std::numeric_limits<int>::min()
            || lastX > std::numeric_limits<int>::max() || lastY > std::numeric_limits<int>::max()
            || (lastX - firstX + 1) * (lastY - firstY + 1) > 64) return result;
        constexpr std::size_t MaxVertices = 3 * 32768;
        const osg::BoundingBox query(center - osg::Vec3f(radius, radius, radius),
            center + osg::Vec3f(radius, radius, radius));
        Terrain::BufferCache buffers;
        const auto indices = buffers.getIndexBuffer((vertices - 1) / 4 + 1, 0);
        struct Writer
        {
            const osg::Vec3Array* mPositions;
            osg::Vec3f mOrigin;
            const osg::BoundingBox* mQuery;
            std::vector<osg::Vec3f>* mResult;
            void operator()(unsigned a, unsigned b, unsigned c)
            {
                if (a >= mPositions->size() || b >= mPositions->size() || c >= mPositions->size()
                    || mResult->size() + 3 > MaxVertices) return;
                const auto x = (*mPositions)[a] + mOrigin, y = (*mPositions)[b] + mOrigin, z = (*mPositions)[c] + mOrigin;
                if (!std::isfinite(x.length2()) || !std::isfinite(y.length2()) || !std::isfinite(z.length2())) return;
                osg::BoundingBox bounds; bounds.expandBy(x); bounds.expandBy(y); bounds.expandBy(z);
                if (bounds.intersects(*mQuery)) mResult->insert(mResult->end(), { x, y, z });
            }
        };
        for (std::int64_t x = int(firstX); x <= int(lastX); ++x)
            for (std::int64_t y = int(firstY); y <= int(lastY); ++y)
            {
                const osg::Vec2f chunkCenter((float(x) + .5f) * .25f, (float(y) + .5f) * .25f);
                float minimum, maximum;
                if (!storage.getMinMaxHeights(.25f, chunkCenter, worldspace, minimum, maximum)
                    || maximum < query.zMin() || minimum > query.zMax()) continue;
                osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array, normals = new osg::Vec3Array;
                osg::ref_ptr<osg::Vec4ubArray> colors = new osg::Vec4ubArray;
                storage.fillVertexBuffers(0, .25f, chunkCenter, worldspace, *positions, *normals, *colors);
                osg::TriangleIndexFunctor<Writer> writer;
                writer.mPositions = positions; writer.mOrigin = { chunkCenter.x() * cellSize, chunkCenter.y() * cellSize, 0 };
                writer.mQuery = &query; writer.mResult = &result;
                indices->accept(writer);
            }
        return result;
    }
}
