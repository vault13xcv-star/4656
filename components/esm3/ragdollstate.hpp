#ifndef OPENMW_ESM_RAGDOLLSTATE_H
#define OPENMW_ESM_RAGDOLLSTATE_H

#include <array>
#include <string>
#include <vector>

namespace ESM
{
    class ESMReader;
    class ESMWriter;

    struct RagdollBodyState
    {
        std::array<float, 7> mTransform{}; // position xyz, quaternion xyzw
        std::array<float, 3> mLinearVelocity{}, mAngularVelocity{};
        float mRadius = 1, mLength = 0, mMass = 1;
    };
    struct RagdollJointState
    {
        int mParent = 0, mChild = 0, mType = 0;
        std::array<float, 7> mParentFrame{}, mChildFrame{};
        std::array<float, 3> mLower{}, mUpper{};
    };
    struct RagdollBoneState
    {
        std::string mName;
        int mBody = -1;
        std::array<float, 16> mRest{}, mBoneToBody{};
    };
    struct RagdollState
    {
        bool mPresent = false;
        bool mFloating = false;
        int mElement = 0, mMode = 0, mPinnedBody = -1;
        float mElapsed = 0, mTimeLeft = 0, mMass = 70, mTorsoLength = 40;
        float mArmorMass = 0;
        float mFireDuration = 15, mPoisonDuration = 10, mShockDuration = 10;
        std::array<float, 3> mPinPoint{}, mAttractionTarget{};
        float mAttractionDistance = 0, mAttractionLeft = 0, mAttractionDuration = 2;
        std::vector<RagdollBodyState> mBodies;
        std::vector<RagdollJointState> mJoints;
        std::vector<RagdollBoneState> mBones;

        void load(ESMReader& reader);
        void save(ESMWriter& writer) const;
    };
}

#endif
