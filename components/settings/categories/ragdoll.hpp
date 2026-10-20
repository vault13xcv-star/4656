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
        SettingValue<bool> mItemPhysics{ mIndex, "Ragdoll", "item physics" };
        SettingValue<float> mDismembermentChance{ mIndex, "Ragdoll", "dismemberment chance",
            makeClampSanitizerFloat(.15f, .6f) };
        SettingValue<float> mMeleeImpulseScale{ mIndex, "Ragdoll", "melee impulse scale",
            makeClampSanitizerFloat(.5f, 2.f) };
        SettingValue<float> mCrossbowImpulseScale{ mIndex, "Ragdoll", "crossbow impulse scale",
            makeClampSanitizerFloat(.5f, 2.f) };
        SettingValue<float> mBowImpulseScale{ mIndex, "Ragdoll", "bow impulse scale",
            makeClampSanitizerFloat(.5f, 2.f) };
        SettingValue<float> mThrownImpulseScale{ mIndex, "Ragdoll", "thrown impulse scale",
            makeClampSanitizerFloat(.5f, 2.f) };
        SettingValue<float> mStatusDurationScale{ mIndex, "Ragdoll", "status duration scale",
            makeClampSanitizerFloat(.5f, 2.f) };
        SettingValue<float> mBowDistance{ mIndex, "Ragdoll", "bow distance", makeClampSanitizerFloat(0, 10) };
        SettingValue<float> mThrownDistance{ mIndex, "Ragdoll", "thrown distance", makeClampSanitizerFloat(0, 10) };
        SettingValue<float> mSpearDistance{ mIndex, "Ragdoll", "spear distance", makeClampSanitizerFloat(0, 10) };
        SettingValue<float> mItemMassScale{
            mIndex, "Ragdoll", "item mass scale", makeClampSanitizerFloat(0.05f, 20.f) };
        SettingValue<float> mItemImpulseScale{
            mIndex, "Ragdoll", "item impulse scale", makeClampSanitizerFloat(0.f, 10.f) };
        SettingValue<int> mMaxActive{ mIndex, "Ragdoll", "max active", makeClampSanitizerInt(1, 200) };
        SettingValue<int> mMaxActiveItems{ mIndex, "Ragdoll", "max active items", makeClampSanitizerInt(1, 200) };
        SettingValue<float> mItemSimulationTime{ mIndex, "Ragdoll", "item simulation time",
            makeClampSanitizerFloat(1, 60) };
        SettingValue<float> mCollisionRadius{
            mIndex, "Ragdoll", "collision radius", makeClampSanitizerFloat(256, 4096) };
        SettingValue<float> mCollisionRefreshInterval{
            mIndex, "Ragdoll", "collision refresh interval", makeClampSanitizerFloat(0.1f, 5.f) };
        SettingValue<float> mMaxSimulationTime{
            mIndex, "Ragdoll", "max simulation time", makeClampSanitizerFloat(1, 60) };
        SettingValue<float> mBaseMass{ mIndex, "Ragdoll", "base mass", makeClampSanitizerFloat(20, 300) };
        SettingValue<float> mReferenceHeight{
            mIndex, "Ragdoll", "reference height", makeClampSanitizerFloat(16, 512) };
        SettingValue<std::vector<std::string>> mCreatureProfiles{ mIndex, "Ragdoll", "creature profiles" };
        SettingValue<float> mLightArmorMassFactor{
            mIndex, "Ragdoll", "light armor mass factor", makeClampSanitizerFloat(0, 5) };
        SettingValue<float> mMediumArmorMassFactor{
            mIndex, "Ragdoll", "medium armor mass factor", makeClampSanitizerFloat(0, 5) };
        SettingValue<float> mHeavyArmorMassFactor{
            mIndex, "Ragdoll", "heavy armor mass factor", makeClampSanitizerFloat(0, 5) };
        SettingValue<float> mImpulseMultiplier{
            mIndex, "Ragdoll", "impulse multiplier", makeClampSanitizerFloat(0, 5) };
        SettingValue<float> mMaxLaunchSpeed{
            mIndex, "Ragdoll", "max launch speed", makeClampSanitizerFloat(100, 3000) };
        SettingValue<float> mDaggerMaxWeight{
            mIndex, "Ragdoll", "dagger max weight", makeClampSanitizerFloat(0, 50) };
        SettingValue<float> mWeaponMinWeight{ mIndex, "Ragdoll", "weapon min weight",
            makeClampSanitizerFloat(-1, 100000) };
        SettingValue<float> mWeaponMaxWeight{ mIndex, "Ragdoll", "weapon max weight",
            makeClampSanitizerFloat(-1, 100000) };
        SettingValue<float> mOneHandDistance{
            mIndex, "Ragdoll", "one hand distance", makeClampSanitizerFloat(0, 10) };
        SettingValue<float> mTwoHandDistance{
            mIndex, "Ragdoll", "two hand distance", makeClampSanitizerFloat(0, 20) };
        SettingValue<float> mCrossbowDistance{ mIndex, "Ragdoll", "crossbow distance", makeClampSanitizerFloat(0, 30) };
        SettingValue<float> mMagicDistanceScale{
            mIndex, "Ragdoll", "magic distance scale", makeClampSanitizerFloat(0, 5) };
        SettingValue<float> mShockDuration{ mIndex, "Ragdoll", "shock duration", makeClampSanitizerFloat(0, 60) };
        SettingValue<float> mShockStrength{
            mIndex, "Ragdoll", "shock strength", makeClampSanitizerFloat(0, 5) };
        SettingValue<float> mPoisonTransitionTime{
            mIndex, "Ragdoll", "poison transition time", makeClampSanitizerFloat(0, 10) };
        SettingValue<float> mFireTransitionTime{
            mIndex, "Ragdoll", "fire transition time", makeClampSanitizerFloat(0, 10) };
        SettingValue<float> mFrostTransitionTime{
            mIndex, "Ragdoll", "frost transition time", makeClampSanitizerFloat(0, 10) };
        SettingValue<float> mFireEffectDuration{ mIndex, "Ragdoll", "fire effect duration",
            makeClampSanitizerFloat(0, 60) };
        SettingValue<float> mPoisonEffectDuration{ mIndex, "Ragdoll", "poison effect duration",
            makeClampSanitizerFloat(0, 60) };
        SettingValue<bool> mPinEnabled{ mIndex, "Ragdoll", "pin enabled" };
        SettingValue<float> mPinMaxDistance{
            mIndex, "Ragdoll", "pin max distance", makeClampSanitizerFloat(0, 20) };
        SettingValue<float> mPinContactDistance{
            mIndex, "Ragdoll", "pin contact distance", makeClampSanitizerFloat(1, 32) };
    };
}

#endif
