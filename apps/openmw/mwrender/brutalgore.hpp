#ifndef OPENMW_MWRENDER_BRUTALGORE_HPP
#define OPENMW_MWRENDER_BRUTALGORE_HPP
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Matrixf>
#include <array>
#include <vector>
namespace MWRender
{
    // Triangles, normals, UVs and tangent channels are clipped together. Each
    // side is a genuine mesh, including a depth-tested exposed cross-section.
    std::array<osg::ref_ptr<osg::Group>, 2> splitGoreMesh(osg::Node& part,
        const osg::Vec3f& planePoint, const osg::Vec3f& planeNormal);
    osg::ref_ptr<osg::Geometry> makeIntestineTube(const std::vector<osg::Vec3f>& centers, float radius);
    void updateIntestineTube(osg::Geometry& mesh, const std::vector<osg::Vec3f>& centers, float radius);
}
#endif
