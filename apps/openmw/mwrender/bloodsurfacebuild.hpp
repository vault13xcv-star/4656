#ifndef OPENMW_MWRENDER_BLOODSURFACEBUILD_HPP
#define OPENMW_MWRENDER_BLOODSURFACEBUILD_HPP

#include "brutalblood.hpp"
#include <components/sceneutil/workqueue.hpp>

namespace MWRender
{
    // The scene is sampled on the update thread. The expensive immutable BVH
    // is built by a worker, which never reads scene nodes or game objects.
    class BloodSurfaceBuild final : public SceneUtil::WorkItem
    {
    public:
        explicit BloodSurfaceBuild(std::vector<osg::Vec3f> triangles, BloodTrail::Surface fallback = {})
            : mTriangles(std::move(triangles)), mFallback(std::move(fallback)) {}
        void doWork() override
        {
            if (!mAborted.load())
            {
                auto surface=BloodTrail::visualSurface(mTriangles);
                if (!mAborted.load()) mSurface=BloodTrail::combineSurfaces(std::move(surface),std::move(mFallback));
            }
            std::vector<osg::Vec3f>().swap(mTriangles);
        }
        void abort() override { mAborted.store(true); }
        BloodTrail::Surface takeResult() { return isDone() && !mAborted.load()?std::move(mSurface):BloodTrail::Surface{}; }
    private:
        std::vector<osg::Vec3f> mTriangles;
        BloodTrail::Surface mSurface;
        BloodTrail::Surface mFallback;
        std::atomic_bool mAborted{false};
    };
}
#endif
