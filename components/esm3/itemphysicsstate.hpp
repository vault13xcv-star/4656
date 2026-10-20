#ifndef OPENMW_ESM_ITEMPHYSICSSTATE_H
#define OPENMW_ESM_ITEMPHYSICSSTATE_H

#include <array>

namespace ESM
{
    // Optional saved-game extension. Old saves have no state and keep authored
    // placement. Inventory copies never retain an active world physics lease.
    struct ItemPhysicsState
    {
        std::array<float, 3> mLinearVelocity{};
        std::array<float, 3> mAngularVelocity{};
        float mTimeLeft = 0;
        bool mPresent = false;
    };
}

#endif
