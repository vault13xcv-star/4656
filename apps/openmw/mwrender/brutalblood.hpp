#ifndef OPENMW_MWRENDER_BRUTALBLOOD_HPP
#define OPENMW_MWRENDER_BRUTALBLOOD_HPP

#include <osg/Group>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/Uniform>
#include <components/esm3/brutalbloodstate.hpp>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace MWRender
{
    struct BloodStamp
    {
        osg::Vec3f mCenter, mNormal;
        std::vector<osg::Vec3f> mTriangles;
        float mRadius = 1;
        bool mPool = false;
    };

    struct BloodStainState
    {
        std::string mCell;
        BloodStamp mStamp;
        float mAge = 0;
    };

    // Batched world-space decals, independent of corpse physics and animation.
    // Cell visibility is explicit: interiors can share world coordinates.
    class BloodDecals
    {
    public:
        explicit BloodDecals(osg::Group& parent);
        ~BloodDecals();
        void add(const std::string& cell, const BloodStamp& stamp);
        void setCellActive(const std::string& cell, bool active);
        void update(float duration, float lifetime);
        void clear();
        std::vector<BloodStainState> saveState() const;
        void restoreState(const std::vector<BloodStainState>& state);
        ESM::BrutalBloodState getSaveRecord() const;
        void loadSaveRecord(const ESM::BrutalBloodState& state);
        std::size_t size() const;

    private:
        struct Chunk
        {
            osg::ref_ptr<osg::Geode> mNode;
            osg::ref_ptr<osg::Geometry> mGeometry;
            bool mDirty = true;
        };
        struct Batch
        {
            std::map<std::uint64_t, Chunk> mChunks;
            bool mActive = false;
        };
        struct Stain : BloodStainState
        {
            std::uint64_t mSequence;
        };
        osg::ref_ptr<osg::Group> mParent;
        std::deque<Stain> mStains;
        std::map<std::string, Batch> mBatches;
        osg::ref_ptr<osg::Uniform> mTimeUniform, mLifetimeUniform;
        std::uint64_t mNextSequence = 0;
        double mTime = 0;
        float mSinceBatch = 1;
        float mLifetime = 600;
        void dirty(const Stain& stain);
        void rebuild();
    };

    class BloodTrail
    {
    public:
        using Raycast = std::function<bool(const osg::Vec3f&, const osg::Vec3f&, osg::Vec3f&, osg::Vec3f&)>;
        using Deposit = std::function<void(const BloodStamp&)>;
        struct Source
        {
            osg::Vec3f mPosition, mDirection;
            float mPressure = 1;
            bool mNeck = false;
        };
        BloodTrail(osg::Group& root, Raycast raycast, Deposit deposit);
        static Raycast collisionRaycast(const std::vector<osg::Vec3f>& triangles);
        ~BloodTrail();
        void update(float duration, const std::vector<Source>& sources, float amount, float age);
        std::size_t particleCount() const { return mDrops.size(); }
        std::size_t emittedCount() const { return mEmitted; }

    private:
        struct Drop { osg::Vec3f mPosition, mVelocity; float mAge = 0, mSize = 1; };
        osg::ref_ptr<osg::Group> mRoot;
        osg::ref_ptr<osg::MatrixTransform> mTransform;
        osg::ref_ptr<osg::Geometry> mGeometry;
        Raycast mRaycast;
        Deposit mDeposit;
        std::vector<Drop> mDrops;
        std::vector<float> mRemainders, mPoolTimes;
        std::size_t mEmitted = 0;
        unsigned mRandom = 0x91a37u;
        float random();
        void stamp(const osg::Vec3f& position, const osg::Vec3f& normal, float radius, bool pool);
    };

    osg::ref_ptr<osg::StateSet> createWetGoreState(bool decal = false);
}
#endif
