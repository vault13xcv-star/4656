#ifndef OPENMW_COMPONENTS_SCENEUTIL_HITLOCATION_HPP
#define OPENMW_COMPONENTS_SCENEUTIL_HITLOCATION_HPP

#include <osg/Node>

namespace SceneUtil
{
    // Per-instance NPC attachment metadata, also retained by clothing/armour
    // clones. -1 requests skeletal fallback (e.g. a whole robe); -2 excludes
    // carried weapons/shields from anatomical dismemberment contacts.
    inline constexpr char HitBodyTag[] = "openmw.ragdoll.hit-body";

    inline int getHitBody(const osg::NodePath& path)
    {
        for (auto node = path.rbegin(); node != path.rend(); ++node)
        {
            int body = -1;
            if ((*node)->getUserValue(HitBodyTag, body) && body >= -2 && body < 15)
                return body;
        }
        return -1;
    }
}

#endif
