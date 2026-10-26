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
#include <list>
#include <map>
#include <memory>
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
        // A deposit may defer projection until it passes the overlap/pool
        // preflight. This callback is consumed immediately and never saved or
        // retained with the stain (it may refer to a short-lived corpse).
        Projector mProjector;
        // Incoming blood volume determines the growth ceiling. This is an
        // emission parameter; saved stains retain their actual fitted radius.
        float mPoolLimit = 100;
        std::uintptr_t mEmitter = 0;
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
        static constexpr std::size_t MaxStains = ESM::BrutalBloodState::MaxStains;
        static constexpr std::size_t MaxVertices = ESM::BrutalBloodState::MaxVertices;
        static constexpr std::size_t ChunkStains = 128;
        static constexpr unsigned MaxOverlappingLayers = 2;
        static constexpr float MaxPoolRadius = 192.f;
        static constexpr float PoolGrowthInterval = .25f;
        static constexpr unsigned MaxNewDepositsPerFrame = 12;
        static constexpr unsigned MaxProjectionAttemptsPerFrame = 24;
        static constexpr unsigned MaxPoolGrowthsPerFrame = 4;
        static constexpr unsigned MaxPoolAbsorptionChecksPerFrame = 8;
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
        // Files are sorted by the caller; an absent group retains the legacy
        // procedural material. All variants share one bounded RGBA atlas.
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
            float mAccumulatedArea = 0;
            double mNextGrowth = 0, mNextWetRefresh = 0;
            mutable std::uint64_t mCoverageVisit = 0;
            std::list<Stain>::iterator mSelf{};
        };
        struct CoverageIndex;
        osg::ref_ptr<osg::Group> mParent;
        std::list<Stain> mStains;
        std::unique_ptr<CoverageIndex> mCoverage;
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
        unsigned mNewDeposits = 0, mProjectionAttempts = 0, mPoolGrowths = 0, mPoolAbsorptionChecks = 0;
        struct EmitterBudget { float mCredit = 1; double mLastSeen = 0; };
        std::map<std::uintptr_t, EmitterBudget> mEmitters;
        void dirty(const Stain& stain);
        void rebuild();
        std::list<Stain>::iterator eraseStain(std::list<Stain>::iterator stain);
        bool feedPool(const std::string& cell, const BloodStamp& stamp);
    };

    class BloodTrail
    {
    public:
        // Linear footprint of newly projected stains; separate from the blood
        // amount slider, which controls emission and the existing pool growth.
        static constexpr float DecalScale = 1.75f;
        static constexpr std::size_t MaxParticles = 192;
        static constexpr std::size_t SpriteVertices = 4;
        static constexpr std::size_t SpriteIndices = 6;
        static constexpr std::size_t MaxSources = 32;
        static constexpr unsigned MaxImpactSplats = 8;
        static constexpr float MaxDropAge = 3.f;
        static constexpr float NormalEmissionRate = 14.f;
        static constexpr float NeckEmissionRate = 60.f;
        static constexpr float MajorEmissionRate = 35.f;
        // Wounds on a resting torso can be several dozen units above sloping
        // terrain; the old eighteen-unit probe missed their growing pools.
        static constexpr float GroundPoolProbe = 64.f;
        using Raycast = std::function<bool(const osg::Vec3f&, const osg::Vec3f&, osg::Vec3f&, osg::Vec3f&)>;
        using Projector = BloodStamp::Projector;
        struct Surface
        {
            Raycast mRaycast;
            Projector mProjector;
            explicit operator bool() const { return bool(mRaycast) && bool(mProjector); }
        };
        using Deposit = std::function<void(const BloodStamp&)>;
        struct Source
        {
            osg::Vec3f mPosition, mDirection;
            float mPressure = 1;
            bool mNeck = false;
            bool mMajorWound = false;
        };
        BloodTrail(osg::Group& root, Raycast raycast, Deposit deposit, Projector projector = {}, bool deferProjection = false);
        static Raycast collisionRaycast(const std::vector<osg::Vec3f>& triangles);
        // Both callbacks share one immutable BVH of visible world triangles.
        static Surface visualSurface(const std::vector<osg::Vec3f>& triangles);
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
        Projector mProjector;
        Deposit mDeposit;
        bool mDeferProjection = false;
        std::vector<Drop> mDrops;
        std::vector<float> mRemainders, mPoolTimes;
        std::vector<unsigned> mPendingDrops;
        std::size_t mNextSource = 0;
        std::size_t mEmitted = 0;
        float mPoolLimit = 100;
        unsigned mRandom = 0x91a37u;
        float random();
        void stamp(const osg::Vec3f& position, const osg::Vec3f& normal, float radius, bool pool);
    };

    osg::ref_ptr<osg::StateSet> createWetGoreState(bool decal = false);
}
#endif
