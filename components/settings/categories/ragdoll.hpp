#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_RAGDOLL_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_RAGDOLL_H

#include <components/settings/sanitizerimpl.hpp>
#include <components/settings/settingvalue.hpp>

namespace Settings
{
    struct RagdollCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<bool> mEnabled{ mIndex, "Ragdoll", "enabled" };
        SettingValue<int> mMaxActive{ mIndex, "Ragdoll", "max active", makeClampSanitizerInt(1, 32) };
        SettingValue<float> mCollisionRadius{
            mIndex, "Ragdoll", "collision radius", makeClampSanitizerFloat(256, 4096) };
        SettingValue<float> mCollisionRefreshInterval{
            mIndex, "Ragdoll", "collision refresh interval", makeClampSanitizerFloat(0.1f, 5.f) };
        SettingValue<float> mMaxSimulationTime{
            mIndex, "Ragdoll", "max simulation time", makeClampSanitizerFloat(1, 60) };
    };
}

#endif
