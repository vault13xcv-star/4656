#ifndef OPENMW_MWRENDER_BLOODSURFACEVISITOR_HPP
#define OPENMW_MWRENDER_BLOODSURFACEVISITOR_HPP

#include <osg/Vec3f>

#include <functional>
#include <vector>

namespace osg
{
    class Node;
    class FrameStamp;
}

namespace MWRender
{
    // Capture nearby visible receiver triangles, never the simplified physics
    // mesh. Hidden collision nodes, actors and transient effects are excluded.
    // A non-colliding NIF child still receives blood when it is part of a world
    // object (for example a rug marked NiCollisionSwitch).
    std::vector<osg::Vec3f> collectBloodSurfaceTriangles(osg::Node& root,
        const osg::Vec3f& center, float radius,
        const std::function<bool(const osg::Node&)>& filter = {},
        const osg::FrameStamp* frameStamp = nullptr);
}

#endif
