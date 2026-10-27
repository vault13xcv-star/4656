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
#include <components/ragdoll/bloodflow.hpp>
#include <cstdint>
#include <array>
#include <list>
#include <deque>
#include <memory>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace MWRender
{
    // Shared by every bleeding corpse and hit burst. Work is served in round
    // robin order; one detailed piece of furniture must not monopolize a frame.
    class BloodProjectionBudget
    {
    public:
        static constexpr unsigned MaxJobs = 4;
        static constexpr unsigned MaxFlowQueries = 16;
        static constexpr double MaxMilliseconds = 1.5;
        struct Client { bool mPending=false, mFlowPending=false; };
        using Frame = std::function<std::uint64_t()>;
        explicit BloodProjectionBudget(Frame frame = {}) : mFrame(std::move(frame)) {}
        std::shared_ptr<Client> addClient();
        void setFrame(std::uint64_t frame) { mManualFrame=frame; }
        bool take(const std::shared_ptr<Client>& client);
        bool takeFlow(const std::shared_ptr<Client>& client);
        void complete(double milliseconds) { mSpent+=milliseconds; }
        unsigned jobs() const { return mJobs; }
    private:
        Frame mFrame;
        std::vector<std::weak_ptr<Client>> mClients;
        std::uint64_t mLastFrame=~std::uint64_t(0), mManualFrame=0;
        std::size_t mCursor=0, mFlowCursor=0;
        unsigned mJobs=0, mFlowJobs=0;
        double mSpent=0;
        void refreshFrame();
    };

    struct BloodStamp
    {
        static constexpr std::uint32_t ProceduralPoolSeed = 0x42504f4fu;
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
        osg::ref_ptr<osg::Node> createPrecompileNode();

    private:
        struct Chunk
        {
            osg::ref_ptr<osg::Geode> mNode;
            osg::ref_ptr<osg::MatrixTransform> mTransform;
            osg::Vec3f mOrigin;
            osg::ref_ptr<osg::Geometry> mGeometry;
            bool mDirty = true;
        };
        struct Stain;
        using SpatialKey = std::array<int, 3>;
        struct Batch
        {
            std::map<std::uint64_t, Chunk> mChunks;
            std::map<SpatialKey, std::vector<Stain*>> mSpatial;
            float mMaxRadius = 0;
            bool mActive = false;
        };
        struct Stain : BloodStainState
        {
            std::uint64_t mSequence;
            osg::Vec3f mHorizontal, mVertical;
            float mPlaneDepth=0;
            bool mFullSquare=false;
        };
        osg::ref_ptr<osg::Group> mParent;
        std::list<Stain> mStains;
        std::map<std::string, Batch> mBatches;
        osg::ref_ptr<osg::Uniform> mTimeUniform, mLifetimeUniform;
        osg::ref_ptr<osg::Texture2D> mAtlas;
        std::vector<osg::Vec4f> mFloorTiles, mWallTiles;
        std::vector<std::vector<osg::Vec2f>> mFloorInk, mWallInk;
        std::size_t mVertexCount = 0;
        std::uint32_t mRandom = 0x9f673bbdu;
        std::uint64_t mNextSequence = 0;
        double mTime = 0;
        float mSinceBatch = 1;
        float mLifetime = 600;
        void dirty(const Stain& stain);
        void rebuild();
        std::vector<Stain*> nearby(const std::string& cell, const BloodStamp& stamp) const;
        bool redundant(const BloodStamp& stamp, const std::vector<Stain*>& nearby) const;
        void index(Stain& stain);
        void unindex(const Stain& stain);
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
        static constexpr unsigned MaxQueuedImpacts = 32;
        static constexpr unsigned MaxProjectionsPerUpdate = 2;
        static constexpr float MaxPendingAge = .5f;
        using Raycast = std::function<bool(const osg::Vec3f&, const osg::Vec3f&, osg::Vec3f&, osg::Vec3f&)>;
        using Deposit = std::function<void(const BloodStamp&)>;
        using Projector = BloodStamp::Projector;
        struct Surface
        {
            Raycast mRaycast;
            Projector mProjector;
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
        BloodTrail(osg::Group& root, Raycast raycast, Deposit deposit, Projector projector = {},
            std::function<bool()> ready = {}, std::shared_ptr<BloodProjectionBudget> budget = {});
        static Raycast collisionRaycast(const std::vector<osg::Vec3f>& triangles);
        static Surface visualSurface(const std::vector<osg::Vec3f>& triangles);
        static Surface collisionSurface(const std::vector<osg::Vec3f>& triangles);
        static Surface combineSurfaces(Surface visual, Surface collision);
        ~BloodTrail();
        void update(float duration, const std::vector<Source>& sources, float amount, float age);
        std::size_t particleCount() const { return mDrops.size(); }
        bool hasPendingSurfaceFlow() const { return !mFlows.empty() || !mPools.empty(); }
        std::size_t emittedCount() const { return mEmitted; }
        std::size_t queuedImpacts() const { return mImpacts.size(); }

    private:
        struct Drop
        {
            osg::Vec3f mPosition, mVelocity;
            float mAge = 0, mSize = 1;
            // Flight never waits for a background receiver build. Keep a
            // separate ballistic cursor to recover contacts without tracing
            // a long straight chord or replaying every missed frame at once.
            osg::Vec3f mTracePosition, mTraceVelocity;
            float mUntracedTime = 0;
            Drop(const osg::Vec3f& position, const osg::Vec3f& velocity, float size)
                : mPosition(position), mVelocity(velocity), mSize(size)
                , mTracePosition(position), mTraceVelocity(velocity) {}
        };
        osg::ref_ptr<osg::Group> mRoot;
        osg::ref_ptr<osg::MatrixTransform> mTransform;
        osg::ref_ptr<osg::Geometry> mGeometry;
        Raycast mRaycast;
        Projector mProjector;
        std::function<bool()> mReady;
        Deposit mDeposit;
        std::vector<Drop> mDrops;
        struct Flow
        {
            Ragdoll::BloodFlow mState;
            osg::Vec3f mLastMark;
            float mAccumulator = 0, mLifetime = 0, mSize = 1;
        };
        std::vector<Flow> mFlows;
        struct Pool
        {
            osg::Vec3f mCenter, mNormal;
            float mVolume = 0, mQueuedRadius = 0, mSinceQueue = 0, mIdle = 0;
        };
        std::vector<Pool> mPools;
        std::vector<float> mRemainders;
        struct Impact { osg::Vec3f mPosition,mNormal; float mRadius=1,mAge=0; bool mPool=false; };
        std::deque<Impact> mImpacts;
        std::shared_ptr<BloodProjectionBudget> mBudget;
        std::shared_ptr<BloodProjectionBudget::Client> mBudgetClient;
        std::vector<unsigned> mPendingDrops;
        std::size_t mNextSource = 0;
        std::size_t mEmitted = 0;
        unsigned mRandom = 0x91a37u;
        float random();
        void stamp(const osg::Vec3f& position, const osg::Vec3f& normal, float radius, bool pool=false);
    };

    osg::ref_ptr<osg::StateSet> createWetGoreState(bool decal = false);
    osg::ref_ptr<osg::StateSet> createTexturedGoreState(osg::Texture2D& texture, osg::Texture2D* mask=nullptr);
}
#endif
