#ifndef GAME_MWWORLD_CELLVISITORS_H
#define GAME_MWWORLD_CELLVISITORS_H

#include <string>
#include <vector>

#include <components/sceneutil/positionattitudetransform.hpp>

#include "ptr.hpp"

namespace MWWorld
{
    struct ListAndResetObjectsVisitor
    {
        std::vector<MWWorld::Ptr> mObjects;

        bool operator()(const MWWorld::Ptr& ptr)
        {
            // Keep the ordinary reference position, but release transient poses,
            // velocities and coatings when a cell leaves the active world.
            if (ptr.getRefData().getRagdollState().mPresent)
                ptr.getRefData().editRagdollState() = {};
            if (ptr.getRefData().getItemPhysicsState().mPresent)
                ptr.getRefData().setItemPhysicsState({});
            if (ptr.getRefData().getBaseNode())
            {
                ptr.getRefData().setBaseNode(nullptr);
            }
            mObjects.push_back(ptr);

            return true;
        }
    };

}

#endif
