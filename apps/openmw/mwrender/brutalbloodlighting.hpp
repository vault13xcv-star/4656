#ifndef OPENMW_MWRENDER_BRUTALBLOODLIGHTING_HPP
#define OPENMW_MWRENDER_BRUTALBLOODLIGHTING_HPP

#include "brutalbloodlightingbase.hpp"
#include <components/sceneutil/lightmanager.hpp>

namespace MWRender
{
    class NativeBloodLighting : public osg::NodeCallback
    {
    public:
        NativeBloodLighting() = default;
        NativeBloodLighting(SceneUtil::LightManager* manager, bool classic = false, bool clamp = true,
            int fogMode = 1, bool radial = false)
            : mManager(manager), mClassic(classic), mClamp(clamp), mFogMode(fogMode), mRadial(radial) {}
        NativeBloodLighting(const NativeBloodLighting& rhs, const osg::CopyOp& copy)
            : osg::Object(rhs, copy), osg::Callback(rhs, copy), osg::NodeCallback(rhs, copy), mManager(rhs.mManager)
            , mClassic(rhs.mClassic), mClamp(rhs.mClamp), mFogMode(rhs.mFogMode), mRadial(rhs.mRadial) {}
        META_Object(MWRender, NativeBloodLighting)

        osg::StateSet* lightingState(osg::Node& node, osgUtil::CullVisitor& cv)
        {
            if (!mManager.valid()) return nullptr;
            if (!cv.getCurrentCamera() || !cv.getCurrentRenderStage()) return nullptr;
            const auto* view = cv.getCurrentRenderStage()->getInitialViewMatrix();
            if (!view) return nullptr;
            // A draw thread may still use the other frame's uniforms. Reflection
            // and other cameras also need their own view-space light positions.
            for (auto it = mStates.begin(); it != mStates.end();)
                if (!it->first.valid()) it = mStates.erase(it); else ++it;
            auto& state = mStates[cv.getCurrentCamera()][cv.getTraversalNumber() % 2];
            if (!state) state = createBloodLightingState(mClassic, mClamp, mFogMode, mRadial);
            state->getUniform("brutalBloodPointCount")->set(0);
            state->getUniform("brutalBloodSunDirection")->set(osg::Vec3f());
            state->getUniform("brutalBloodSunDiffuse")->set(osg::Vec3f());
            if (const auto sun = mManager->getSunlight()) setBloodSunLight(*state, *sun, osg::Matrixf(*view));
            const int limit = std::clamp(mManager->getMaxLights() - mManager->getStartLight(), 0, BloodMaxPointLights);
            if (!limit) return state;

            osg::BoundingSphere bound;
            if (auto* transform = node.asTransform())
                for (unsigned i = 0; i < transform->getNumChildren(); ++i) bound.expandBy(transform->getChild(i)->getBound());
            else bound = node.getBound();
            const auto& modelView = *cv.getModelViewMatrix();
            bound.center() = bound.center() * modelView;
            // Use only the linear transform for radius; translation must never
            // enlarge a receiver's light bounds when the camera moves.
            const osg::Vec3d x(modelView(0, 0), modelView(0, 1), modelView(0, 2));
            const osg::Vec3d y(modelView(1, 0), modelView(1, 1), modelView(1, 2));
            const osg::Vec3d z(modelView(2, 0), modelView(2, 1), modelView(2, 2));
            bound.radius() *= std::max({ x.length(), y.length(), z.length() });
            const auto& lights = mManager->getLightsInViewSpace(&cv, view, cv.getTraversalNumber());
            std::array<const SceneUtil::LightManager::LightSourceViewBound*, BloodMaxPointLights> nearest{};
            int count = 0;
            const auto distance = [&](const auto* light) {
                return (light->mViewBound.center() - bound.center()).length2()
                    / std::max(.001f, light->mViewBound.radius2());
            };
            for (const auto& light : lights)
            {
                if (light.mLightSource->getEmpty() || !light.mViewBound.intersects(bound)) continue;
                const auto where = std::lower_bound(nearest.begin(), nearest.begin() + count, &light,
                    [&](const auto* left, const auto* right) { return distance(left) < distance(right); });
                const int index = where - nearest.begin();
                if (index >= limit) continue;
                for (int i = std::min(count, limit - 1); i > index; --i) nearest[i] = nearest[i - 1];
                nearest[index] = &light;
                count = std::min(count + 1, limit);
            }
            int index = 0;
            for (int i = 0; i < count; ++i)
            {
                const auto* light = nearest[i];
                setBloodPointLight(*state, index++, *light->mLightSource->getLight(cv.getTraversalNumber()),
                    light->mLightSource->getRadius(), osg::Matrixf(*view));
            }
            return state;
        }

        void operator()(osg::Node* node, osg::NodeVisitor* visitor) override
        {
            auto* cv = dynamic_cast<osgUtil::CullVisitor*>(visitor);
            auto* state = cv ? lightingState(*node, *cv) : nullptr;
            if (state) cv->pushStateSet(state);
            traverse(node, visitor);
            if (state) cv->popStateSet();
        }

    private:
        osg::observer_ptr<SceneUtil::LightManager> mManager;
        bool mClassic = false, mClamp = true;
        int mFogMode = 1;
        bool mRadial = false;
        std::map<osg::observer_ptr<osg::Camera>, std::array<osg::ref_ptr<osg::StateSet>, 2>> mStates;
    };

    inline void installNativeBloodLighting(SceneUtil::LightManager& manager, bool classic, bool clamp,
        int fogMode, bool radial, osg::Shader* shadowVertex = nullptr, osg::Shader* shadowFragment = nullptr)
    {
        const osg::observer_ptr<SceneUtil::LightManager> weak(&manager);
        setBloodLightingRoot(manager, [weak, classic, clamp, fogMode, radial]() -> osg::Callback* {
            return weak.valid() ? new NativeBloodLighting(weak.get(), classic, clamp, fogMode, radial) : nullptr;
        }, shadowVertex, shadowFragment);
    }
}

#endif
