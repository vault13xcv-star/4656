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
        explicit BloodSurfaceBuild(std::vector<osg::Vec3f> triangles) : mTriangles(std::move(triangles)) {}
        void doWork() override
        {
            if (!mAborted.load())
            {
                auto surface=BloodTrail::visualSurface(mTriangles);
                if (!mAborted.load()) mSurface=std::move(surface);
            }
            std::vector<osg::Vec3f>().swap(mTriangles);
        }
        void abort() override { mAborted.store(true); }
        BloodTrail::Surface takeResult() { return isDone() && !mAborted.load()?std::move(mSurface):BloodTrail::Surface{}; }
    private:
        std::vector<osg::Vec3f> mTriangles;
        BloodTrail::Surface mSurface;
        std::atomic_bool mAborted{false};
    };
}
#endif
