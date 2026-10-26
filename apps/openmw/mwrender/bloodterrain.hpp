#ifndef OPENMW_MWRENDER_BLOODTERRAIN_HPP
#define OPENMW_MWRENDER_BLOODTERRAIN_HPP
#include <components/esm/refid.hpp>
#include <osg/Vec3f>
#include <vector>
namespace Terrain { class Storage; }
namespace MWRender
{
    // Read the same LAND vertex/index data as native terrain rendering. This
    // receiver does not depend on a render traversal finding the terrain node.
    std::vector<osg::Vec3f> collectBloodTerrainTriangles(Terrain::Storage& storage,
        ESM::RefId worldspace, const osg::Vec3f& center, float radius);
}
#endif
