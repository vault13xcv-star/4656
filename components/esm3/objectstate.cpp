#include "objectstate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <typeinfo>

#include "esmreader.hpp"
#include "esmwriter.hpp"

namespace ESM
{

    namespace
    {
        template <std::size_t N>
        void readPhysicsFloats(ESMReader& reader, NAME name, float (&values)[N])
        {
            reader.getHNT(values, name);
            if (!std::all_of(std::begin(values), std::end(values), [](float x) { return std::isfinite(x); }))
                throw std::runtime_error("Non-finite saved physics value");
        }
        bool indexValue(float value, int minimum, int maximum)
        {
            return value >= minimum && value <= maximum && value == std::floor(value);
        }
    }

    void RagdollState::load(ESMReader& reader)
    {
        *this = {};
        if (!reader.peekNextSub("RPHY"))
            return;
        float header[25];
        readPhysicsFloats(reader, "RPHY", header);
        if (header[0] != 1 || !indexValue(header[1], 0, 4) || !indexValue(header[2], 0, 2) || header[3] < 0
            || header[4] < 0 || header[4] > 60 || header[5] <= 0 || header[5] > 5000 || !indexValue(header[20], 1, 64)
            || !indexValue(header[21], 0, 127) || !indexValue(header[22], 1, 1024)
            || !indexValue(header[10], -1, int(header[20]) - 1) || header[15] < 0 || header[15] > 10 || header[16] <= 0
            || header[16] > 10)
            throw std::runtime_error("Invalid saved ragdoll header");
        if (header[6] <= 0 || header[6] > 4096 || header[7] < 0 || header[7] > 60 || header[8] < 0 || header[8] > 60
            || header[9] < 0 || header[9] > 60 || header[23] < 0 || header[23] > 5000 || !indexValue(header[24], 0, 1))
            throw std::runtime_error("Invalid saved ragdoll timers or mass");
        mPresent = true;
        mElement = int(header[1]);
        mMode = int(header[2]);
        mElapsed = header[3];
        mTimeLeft = header[4];
        mMass = header[5];
        mTorsoLength = header[6];
        mFireDuration = header[7];
        mPoisonDuration = header[8];
        mShockDuration = header[9];
        mPinnedBody = int(header[10]);
        mArmorMass = std::max(0.f, header[23]);
        mFloating = header[24] != 0;
        for (int axis = 0; axis < 3; ++axis)
        {
            mPinPoint[axis] = header[11 + axis];
            mAttractionTarget[axis] = header[17 + axis];
        }
        mAttractionDistance = header[14];
        mAttractionLeft = header[15];
        mAttractionDuration = header[16];
        for (int i = 0; i < int(header[20]); ++i)
        {
            float data[16];
            readPhysicsFloats(reader, "RBDY", data);
            RagdollBodyState body;
            std::copy_n(data, 7, body.mTransform.begin());
            std::copy_n(data + 7, 3, body.mLinearVelocity.begin());
            std::copy_n(data + 10, 3, body.mAngularVelocity.begin());
            body.mRadius = data[13];
            body.mLength = data[14];
            body.mMass = data[15];
            if (body.mRadius <= 0 || body.mLength < 0 || body.mMass <= 0)
                throw std::runtime_error("Invalid saved ragdoll body");
            mBodies.push_back(std::move(body));
        }
        for (int i = 0; i < int(header[21]); ++i)
        {
            float data[23];
            readPhysicsFloats(reader, "RJNT", data);
            if (!indexValue(data[0], 0, int(mBodies.size()) - 1) || !indexValue(data[1], 0, int(mBodies.size()) - 1)
                || !indexValue(data[2], 0, 1))
                throw std::runtime_error("Invalid saved ragdoll joint");
            RagdollJointState joint;
            joint.mParent = int(data[0]);
            joint.mChild = int(data[1]);
            joint.mType = int(data[2]);
            std::copy_n(data + 3, 7, joint.mParentFrame.begin());
            std::copy_n(data + 10, 7, joint.mChildFrame.begin());
            std::copy_n(data + 17, 3, joint.mLower.begin());
            std::copy_n(data + 20, 3, joint.mUpper.begin());
            mJoints.push_back(std::move(joint));
        }
        for (int i = 0; i < int(header[22]); ++i)
        {
            RagdollBoneState bone;
            bone.mName = reader.getHNString("RBNM");
            float data[33];
            readPhysicsFloats(reader, "RBMT", data);
            if (!indexValue(data[0], -1, int(mBodies.size()) - 1))
                throw std::runtime_error("Invalid saved ragdoll bone");
            bone.mBody = int(data[0]);
            std::copy_n(data + 1, 16, bone.mRest.begin());
            std::copy_n(data + 17, 16, bone.mBoneToBody.begin());
            mBones.push_back(std::move(bone));
        }
        if (reader.peekNextSub("RWND"))
        {
            float wounds[3];
            readPhysicsFloats(reader, "RWND", wounds);
            if (!indexValue(wounds[0], 0, 32767) || !indexValue(wounds[1], 0, 1)
                || (unsigned(wounds[0]) & ~686u) || (wounds[1] != 0 && !(unsigned(wounds[0]) & 4u))
                || wounds[2] < 0 || wounds[2] > 60)
                throw std::runtime_error("Invalid saved ragdoll wounds");
            mCuts = unsigned(wounds[0]);
            mBurstHead = wounds[1] != 0;
            mBleedDuration = wounds[2];
        }
    }

    void RagdollState::save(ESMWriter& writer) const
    {
        if (!mPresent)
            return;
        const float header[25] = { 1.f, float(mElement), float(mMode), mElapsed, mTimeLeft, mMass, mTorsoLength,
            mFireDuration, mPoisonDuration, mShockDuration, float(mPinnedBody), mPinPoint[0], mPinPoint[1],
            mPinPoint[2], mAttractionDistance, mAttractionLeft, mAttractionDuration, mAttractionTarget[0],
            mAttractionTarget[1], mAttractionTarget[2], float(mBodies.size()), float(mJoints.size()),
            float(mBones.size()), mArmorMass, mFloating ? 1.f : 0.f };
        writer.writeHNT("RPHY", header);
        for (const auto& body : mBodies)
        {
            float data[16];
            std::copy(body.mTransform.begin(), body.mTransform.end(), data);
            std::copy(body.mLinearVelocity.begin(), body.mLinearVelocity.end(), data + 7);
            std::copy(body.mAngularVelocity.begin(), body.mAngularVelocity.end(), data + 10);
            data[13] = body.mRadius;
            data[14] = body.mLength;
            data[15] = body.mMass;
            writer.writeHNT("RBDY", data);
        }
        for (const auto& joint : mJoints)
        {
            float data[23];
            data[0] = float(joint.mParent);
            data[1] = float(joint.mChild);
            data[2] = float(joint.mType);
            std::copy(joint.mParentFrame.begin(), joint.mParentFrame.end(), data + 3);
            std::copy(joint.mChildFrame.begin(), joint.mChildFrame.end(), data + 10);
            std::copy(joint.mLower.begin(), joint.mLower.end(), data + 17);
            std::copy(joint.mUpper.begin(), joint.mUpper.end(), data + 20);
            writer.writeHNT("RJNT", data);
        }
        for (const auto& bone : mBones)
        {
            writer.writeHNCString("RBNM", bone.mName);
            float data[33];
            data[0] = float(bone.mBody);
            std::copy(bone.mRest.begin(), bone.mRest.end(), data + 1);
            std::copy(bone.mBoneToBody.begin(), bone.mBoneToBody.end(), data + 17);
            writer.writeHNT("RBMT", data);
        }
        const float wounds[3] = { float(mCuts), mBurstHead ? 1.f : 0.f, mBleedDuration };
        writer.writeHNT("RWND", wounds);
    }

    void ObjectState::load(ESMReader& esm)
    {
        mVersion = esm.getFormatVersion();
        mActorIdConverter = esm.mActorIdConverter;

        bool isDeleted;
        mRef.loadData(esm, isDeleted);

        mHasLocals = 0;
        esm.getHNOT(mHasLocals, "HLOC");

        if (mHasLocals)
            mLocals.load(esm);

        mLuaScripts.load(esm);

        mEnabled = 1;
        esm.getHNOT(mEnabled, "ENAB");

        if (mVersion <= MaxOldCountFormatVersion)
        {
            if (mVersion <= MaxOldGoldValueFormatVersion)
                mRef.mCount = std::max(1, mRef.mCount);
            esm.getHNOT("COUN", mRef.mCount);
        }

        mPosition = mRef.mPos;
        esm.getOptionalComposite("POS_", mPosition);

        mFlags = 0;
        esm.getHNOT(mFlags, "FLAG");

        mAnimationState.load(esm);

        mItemPhysics = {};
        if (esm.peekNextSub("IPHY"))
        {
            float values[8];
            esm.getHNT(values, "IPHY");
            if (values[0] != 1.f
                || !std::all_of(std::begin(values), std::end(values), [](float value) { return std::isfinite(value); })
                || values[1] < 0.f || values[1] > 60.f)
                throw std::runtime_error("Invalid saved item physics state");
            mItemPhysics.mPresent = true;
            mItemPhysics.mTimeLeft = values[1];
            for (int axis = 0; axis < 3; ++axis)
            {
                mItemPhysics.mLinearVelocity[axis] = values[2 + axis];
                mItemPhysics.mAngularVelocity[axis] = values[5 + axis];
            }
        }
        mRagdoll.load(esm);

        // FIXME: assuming "false" as default would make more sense, but also break compatibility with older save files
        mHasCustomState = true;
        esm.getHNOT(mHasCustomState, "HCUS");
    }

    void ObjectState::save(ESMWriter& esm, bool inInventory) const
    {
        mRef.save(esm, true, inInventory);

        if (mHasLocals)
        {
            esm.writeHNT("HLOC", mHasLocals);
            mLocals.save(esm);
        }

        mLuaScripts.save(esm);

        if (!mEnabled && !inInventory)
            esm.writeHNT("ENAB", mEnabled);

        if (!inInventory && mPosition != mRef.mPos)
        {
            esm.writeNamedComposite("POS_", mPosition);
        }

        if (mFlags != 0)
            esm.writeHNT("FLAG", mFlags);

        mAnimationState.save(esm);

        if (!inInventory && mItemPhysics.mPresent)
        {
            const float values[8] = { 1.f, mItemPhysics.mTimeLeft, mItemPhysics.mLinearVelocity[0],
                mItemPhysics.mLinearVelocity[1], mItemPhysics.mLinearVelocity[2], mItemPhysics.mAngularVelocity[0],
                mItemPhysics.mAngularVelocity[1], mItemPhysics.mAngularVelocity[2] };
            esm.writeHNT("IPHY", values);
        }
        if (!inInventory)
            mRagdoll.save(esm);

        if (!mHasCustomState)
            esm.writeHNT("HCUS", false);
    }

    void ObjectState::blank()
    {
        mRef.blank();
        mHasLocals = 0;
        mEnabled = false;
        for (int i = 0; i < 3; ++i)
        {
            mPosition.pos[i] = 0;
            mPosition.rot[i] = 0;
        }
        mFlags = 0;
        mHasCustomState = true;
        mItemPhysics = {};
        mRagdoll = {};
    }

    const NpcState& ObjectState::asNpcState() const
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to NpcState";
        throw std::logic_error(error.str());
    }

    NpcState& ObjectState::asNpcState()
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to NpcState";
        throw std::logic_error(error.str());
    }

    const CreatureState& ObjectState::asCreatureState() const
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to CreatureState";
        throw std::logic_error(error.str());
    }

    CreatureState& ObjectState::asCreatureState()
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to CreatureState";
        throw std::logic_error(error.str());
    }

    const ContainerState& ObjectState::asContainerState() const
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to ContainerState";
        throw std::logic_error(error.str());
    }

    ContainerState& ObjectState::asContainerState()
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to ContainerState";
        throw std::logic_error(error.str());
    }

    const DoorState& ObjectState::asDoorState() const
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to DoorState";
        throw std::logic_error(error.str());
    }

    DoorState& ObjectState::asDoorState()
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to DoorState";
        throw std::logic_error(error.str());
    }

    const CreatureLevListState& ObjectState::asCreatureLevListState() const
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to CreatureLevListState";
        throw std::logic_error(error.str());
    }

    CreatureLevListState& ObjectState::asCreatureLevListState()
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to CreatureLevListState";
        throw std::logic_error(error.str());
    }

    ObjectState::~ObjectState() = default;

}
