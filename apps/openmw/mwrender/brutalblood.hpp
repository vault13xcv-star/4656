#ifndef OPENMW_MWRENDER_BRUTALBLOOD_HPP
#define OPENMW_MWRENDER_BRUTALBLOOD_HPP

#include <osg/Group>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/Uniform>
#include <osg/Image>
#include <osg/Texture2D>
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
        using Projector = std::function<std::vector<osg::Vec3f>(const osg::Vec3f&, const osg::Vec3f&, float)>;
        osg::Vec3f mCenter, mNormal;
        std::vector<osg::Vec3f> mTriangles;
        float mRadius = 1;
        bool mPool = false;
        std::uint32_t mVisualSeed = 0;
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
        static constexpr std::size_t MaxStains = 32768;
        static constexpr std::size_t MaxVertices = 8192 * 192;
        static constexpr std::size_t ChunkStains = 128;
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
        void setTextures(const std::vector<osg::ref_ptr<osg::Image>>& floor,
            const std::vector<osg::ref_ptr<osg::Image>>& wall);

    private:
        struct Chunk
        {
            osg::ref_ptr<osg::Geode> mNode;
            osg::ref_ptr<osg::MatrixTransform> mTransform;
            osg::Vec3f mOrigin;
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
        osg::ref_ptr<osg::Texture2D> mAtlas;
        std::vector<osg::Vec4f> mFloorTiles, mWallTiles;
        std::size_t mVertexCount = 0;
        std::uint32_t mRandom = 0x9f673bbdu;
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
        static constexpr std::size_t MaxParticles = 192;
        static constexpr std::size_t SpriteVertices = 4;
        static constexpr std::size_t SpriteIndices = 6;
        static constexpr std::size_t MaxSources = 32;
        static constexpr float MaxDropAge = 4.f;
        static constexpr unsigned MaxImpactSplats = 8;
        using Raycast = std::function<bool(const osg::Vec3f&, const osg::Vec3f&, osg::Vec3f&, osg::Vec3f&)>;
        using Deposit = std::function<void(const BloodStamp&)>;
        using Projector = BloodStamp::Projector;
        struct Surface
        {
            Raycast mRaycast;
            Projector mProjector;
            Raycast mPoolRaycast;
            std::function<bool()> mValid;
            explicit operator bool() const { return bool(mRaycast) && (!mValid || mValid()); }
        };
        struct Source
        {
            osg::Vec3f mPosition, mDirection;
            float mPressure = 1;
            bool mNeck = false;
            bool mMajorWound = false;
        };
        BloodTrail(osg::Group& root, Raycast raycast, Deposit deposit, Projector projector = {}, Raycast poolRaycast = {},
            std::function<bool()> ready = {});
        static Raycast collisionRaycast(const std::vector<osg::Vec3f>& triangles);
        static Surface visualSurface(const std::vector<osg::Vec3f>& triangles);
        static Surface collisionSurface(const std::vector<osg::Vec3f>& triangles);
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
        Raycast mPoolRaycast;
        Projector mProjector;
        std::function<bool()> mReady;
        Deposit mDeposit;
        std::vector<Drop> mDrops;
        std::vector<float> mRemainders, mPoolTimes, mPoolRemainders;
        std::vector<unsigned> mPendingDrops;
        std::size_t mNextSource = 0;
        std::size_t mEmitted = 0;
        unsigned mRandom = 0x91a37u;
        float random();
        void stamp(const osg::Vec3f& position, const osg::Vec3f& normal, float radius, bool pool);
    };

    osg::ref_ptr<osg::StateSet> createWetGoreState(bool decal = false);
}
#endif
