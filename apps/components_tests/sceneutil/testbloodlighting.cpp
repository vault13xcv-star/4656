#include <apps/openmw/mwrender/brutalbloodlighting.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/shader/shadermanager.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Geode>
#include <osg/Light>
#include <osg/MatrixTransform>
#include <osg/Viewport>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>

namespace
{
    // Real native LightManager queries need the same per-camera view state
    // used by normal culling, but no graphics context or game data.
    struct BloodLightView
    {
        osg::ref_ptr<osg::Camera> mCamera = new osg::Camera;
        osg::ref_ptr<osgUtil::RenderStage> mStage = new osgUtil::RenderStage;
        osg::ref_ptr<osgUtil::StateGraph> mGraph = new osgUtil::StateGraph;
        osg::ref_ptr<osgUtil::CullVisitor> mVisitor = new osgUtil::CullVisitor;

        explicit BloodLightView(unsigned frame, const osg::Matrixf& view = osg::Matrixf::translate(0, 0, -100))
        {
            mStage->setCamera(mCamera);
            mStage->setInitialViewMatrix(new osg::RefMatrix(view));
            mVisitor->setRenderStage(mStage);
            mVisitor->setStateGraph(mGraph);
            osg::ref_ptr<osg::FrameStamp> stamp = new osg::FrameStamp;
            stamp->setFrameNumber(frame);
            mVisitor->setFrameStamp(stamp);
            mVisitor->setTraversalNumber(frame);
            mVisitor->setCullingMode(osg::CullSettings::NO_CULLING);
            mVisitor->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
            mVisitor->pushViewport(new osg::Viewport(0, 0, 64, 64));
            mVisitor->pushProjectionMatrix(new osg::RefMatrix(osg::Matrix::ortho(-200, 200, -200, 200, 1, 2000)));
            mVisitor->pushModelViewMatrix(new osg::RefMatrix(view), osg::Transform::ABSOLUTE_RF);
        }

        ~BloodLightView()
        {
            mVisitor->popModelViewMatrix();
            mVisitor->popProjectionMatrix();
            mVisitor->popViewport();
        }
    };

    osg::ref_ptr<SceneUtil::LightSource> bloodPointLight(const osg::Vec4f& diffuse, float radius = 128)
    {
        osg::ref_ptr<SceneUtil::LightSource> source = new SceneUtil::LightSource;
        source->setRadius(radius);
        osg::ref_ptr<osg::Light> light = new osg::Light;
        light->setDiffuse(diffuse);
        light->setSpecular(diffuse);
        light->setConstantAttenuation(1);
        light->setLinearAttenuation(.1f);
        light->setQuadraticAttenuation(.01f);
        source->setLight(light);
        return source;
    }

    template <class T>
    T bloodUniform(const osg::StateSet& state, const char* name, unsigned index = 0)
    {
        T value{};
        const auto* uniform = state.getUniform(name);
        EXPECT_NE(uniform, nullptr) << name;
        if (uniform)
        {
            EXPECT_TRUE(uniform->getElement(index, value)) << name << '[' << index << ']';
        }
        return value;
    }

    SceneUtil::LightSettings bloodLightSettings()
    {
        SceneUtil::LightSettings settings;
        // Keep native camera-distance fade out of the transform assertions;
        // the adapter must copy the light already faded by LightManager.
        settings.mMaximumLightDistance = 0;
        return settings;
    }

    TEST(BloodNativeLighting, readsNativeSunAndPointLightWithItsActualAttenuation)
    {
        osg::ref_ptr<SceneUtil::LightManager> manager = new SceneUtil::LightManager(bloodLightSettings());
        osg::ref_ptr<osg::Light> sun = new osg::Light;
        sun->setPosition({ 1, 0, 0, 0 });
        sun->setDiffuse({ .7f, .6f, .5f, 1 });
        manager->setSunlight(sun);
        auto point = bloodPointLight({ .8f, .5f, .2f, 1 });
        point->getLight(0)->setAmbient({ .01f, .02f, .03f, 1 });
        manager->update(0);
        manager->addLight(point, osg::Matrixf::translate(0, 0, 20), 0);

        osg::ref_ptr<osg::Geode> receiver = new osg::Geode;
        receiver->setInitialBound(osg::BoundingSphere(osg::Vec3f(), 10));
        BloodLightView view(0);
        osg::ref_ptr<MWRender::NativeBloodLighting> adapter
            = new MWRender::NativeBloodLighting(manager, true, false, 2, true);
        const auto* state = adapter->lightingState(*receiver, *view.mVisitor);
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(bloodUniform<osg::Vec3f>(*state, "brutalBloodSunDirection"), osg::Vec3f(1, 0, 0));
        EXPECT_EQ(bloodUniform<osg::Vec3f>(*state, "brutalBloodSunDiffuse"), osg::Vec3f(.7f, .6f, .5f));
        EXPECT_EQ(bloodUniform<int>(*state, "brutalBloodPointCount"), 1);
        EXPECT_EQ(bloodUniform<osg::Vec3f>(*state, "brutalBloodPointPosition"), osg::Vec3f(0, 0, -80));
        EXPECT_EQ(bloodUniform<osg::Vec3f>(*state, "brutalBloodPointDiffuse"), osg::Vec3f(.8f, .5f, .2f));
        EXPECT_EQ(bloodUniform<osg::Vec3f>(*state, "brutalBloodPointAmbient"), osg::Vec3f(.01f, .02f, .03f));
        EXPECT_EQ(bloodUniform<osg::Vec4f>(*state, "brutalBloodPointAttenuation"), osg::Vec4f(1, .1f, .01f, 128));
        EXPECT_TRUE(bloodUniform<bool>(*state, "brutalBloodClassicFalloff"));
        EXPECT_FALSE(bloodUniform<bool>(*state, "brutalBloodClamp"));
        EXPECT_EQ(bloodUniform<int>(*state, "brutalBloodFogMode"), 2);
        EXPECT_TRUE(bloodUniform<bool>(*state, "brutalBloodFogRadial"));
    }

    TEST(BloodNativeLighting, cameraTranslationDoesNotChangeReceiverLightSelectionOrGrowBounds)
    {
        osg::ref_ptr<SceneUtil::LightManager> manager = new SceneUtil::LightManager(bloodLightSettings());
        auto near = bloodPointLight({ .8f, .5f, .2f, 1 }, 16);
        auto outside = bloodPointLight({ 1, 1, 1, 1 }, 3);
        manager->update(0);
        manager->addLight(near, osg::Matrixf::translate(0, 0, 4), 0);
        manager->addLight(outside, osg::Matrixf::translate(100, 0, 0), 0);
        osg::ref_ptr<osg::Geode> receiver = new osg::Geode;
        receiver->setInitialBound(osg::BoundingSphere(osg::Vec3f(), 2));
        osg::ref_ptr<MWRender::NativeBloodLighting> adapter = new MWRender::NativeBloodLighting(manager);
        BloodLightView first(0), moved(0, osg::Matrixf::translate(0, 0, -1200));
        const auto* before = adapter->lightingState(*receiver, *first.mVisitor);
        const auto* after = adapter->lightingState(*receiver, *moved.mVisitor);
        ASSERT_NE(before, nullptr);
        ASSERT_NE(after, nullptr);
        EXPECT_NE(before, after);
        EXPECT_EQ(bloodUniform<int>(*before, "brutalBloodPointCount"), 1);
        EXPECT_EQ(bloodUniform<int>(*after, "brutalBloodPointCount"), 1);
        EXPECT_EQ(bloodUniform<osg::Vec3f>(*before, "brutalBloodPointDiffuse"),
            bloodUniform<osg::Vec3f>(*after, "brutalBloodPointDiffuse"));
        EXPECT_EQ(bloodUniform<osg::Vec3f>(*after, "brutalBloodPointPosition"), osg::Vec3f(0, 0, -1196));
    }

    TEST(BloodNativeLighting, tracksMovingAndExtinguishedLightsWithoutOverwritingPreviousFrame)
    {
        osg::ref_ptr<SceneUtil::LightManager> manager = new SceneUtil::LightManager(bloodLightSettings());
        auto point = bloodPointLight({ .8f, .5f, .2f, 1 }, 32);
        manager->update(0);
        manager->addLight(point, osg::Matrixf::translate(0, 0, 8), 0);
        osg::ref_ptr<osg::Geode> receiver = new osg::Geode;
        receiver->setInitialBound(osg::BoundingSphere(osg::Vec3f(), 2));
        osg::ref_ptr<MWRender::NativeBloodLighting> adapter = new MWRender::NativeBloodLighting(manager);
        BloodLightView view(0);
        const auto* first = adapter->lightingState(*receiver, *view.mVisitor);
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(bloodUniform<int>(*first, "brutalBloodPointCount"), 1);

        manager->update(1);
        manager->addLight(point, osg::Matrixf::translate(1000, 0, 8), 1);
        view.mVisitor->setTraversalNumber(1);
        const auto* moved = adapter->lightingState(*receiver, *view.mVisitor);
        ASSERT_NE(moved, nullptr);
        EXPECT_NE(first, moved);
        EXPECT_EQ(bloodUniform<int>(*moved, "brutalBloodPointCount"), 0);
        EXPECT_EQ(bloodUniform<int>(*first, "brutalBloodPointCount"), 1);

        point->setEmpty(true);
        manager->update(2);
        manager->addLight(point, osg::Matrixf::translate(0, 0, 8), 2);
        view.mVisitor->setTraversalNumber(2);
        EXPECT_EQ(bloodUniform<int>(*adapter->lightingState(*receiver, *view.mVisitor), "brutalBloodPointCount"), 0);
    }

    TEST(BloodNativeLighting, retainsNegativeLightsAndBoundsTheNativeLightPalette)
    {
        osg::ref_ptr<SceneUtil::LightManager> manager = new SceneUtil::LightManager(bloodLightSettings());
        std::vector<osg::ref_ptr<SceneUtil::LightSource>> sources;
        manager->update(0);
        for (int i = 0; i < MWRender::BloodMaxPointLights + 4; ++i)
        {
            auto source = bloodPointLight(i == 0 ? osg::Vec4f(-.4f, -.2f, -.1f, 1) : osg::Vec4f(.8f, .5f, .2f, 1));
            manager->addLight(source, osg::Matrixf::translate(0, 0, float(i + 1)), 0);
            sources.push_back(source);
        }
        osg::ref_ptr<osg::Geode> receiver = new osg::Geode;
        receiver->setInitialBound(osg::BoundingSphere(osg::Vec3f(), 2));
        osg::ref_ptr<MWRender::NativeBloodLighting> adapter = new MWRender::NativeBloodLighting(manager);
        BloodLightView view(0);
        const auto* state = adapter->lightingState(*receiver, *view.mVisitor);
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(bloodUniform<int>(*state, "brutalBloodPointCount"), MWRender::BloodMaxPointLights);
        EXPECT_EQ(bloodUniform<osg::Vec3f>(*state, "brutalBloodPointDiffuse"), osg::Vec3f(-.4f, -.2f, -.1f));
        EXPECT_EQ(bloodUniform<osg::Vec3f>(*state, "brutalBloodPointPosition", MWRender::BloodMaxPointLights - 1),
            osg::Vec3f(0, 0, float(MWRender::BloodMaxPointLights) - 100));
        manager->setStartLight(1);
        manager->updateMaxLights(3);
        const auto* limited = adapter->lightingState(*receiver, *view.mVisitor);
        ASSERT_NE(limited, nullptr);
        EXPECT_EQ(bloodUniform<int>(*limited, "brutalBloodPointCount"), 2);
        EXPECT_EQ(bloodUniform<osg::Vec3f>(*limited, "brutalBloodPointPosition", 1), osg::Vec3f(0, 0, -98));
    }

    TEST(BloodNativeLighting, installedSceneFactoryReachesNestedCorpseRootsAndPreservesOtherUserData)
    {
        osg::ref_ptr<SceneUtil::LightManager> manager = new SceneUtil::LightManager(bloodLightSettings());
        osg::ref_ptr<osg::Referenced> existingData = new osg::Referenced;
        manager->setUserData(existingData);
        MWRender::installNativeBloodLighting(*manager, false, true, 1, false);
        EXPECT_EQ(manager->getUserData(), existingData.get());
        osg::ref_ptr<osg::MatrixTransform> corpse = new osg::MatrixTransform;
        osg::ref_ptr<osg::Group> bloodRoot = new osg::Group;
        manager->addChild(corpse);
        corpse->addChild(bloodRoot);
        osg::ref_ptr<osg::Geode> first = new osg::Geode, second = new osg::Geode;
        MWRender::attachBloodLighting(*first, *bloodRoot);
        MWRender::attachBloodLighting(*second, *bloodRoot);
        EXPECT_NE(dynamic_cast<MWRender::NativeBloodLighting*>(first->getCullCallback()), nullptr);
        EXPECT_NE(dynamic_cast<MWRender::NativeBloodLighting*>(second->getCullCallback()), nullptr);
        EXPECT_NE(first->getCullCallback(), second->getCullCallback());

        osg::ref_ptr<osg::Group> fixtureRoot = new osg::Group;
        osg::ref_ptr<osg::Geode> unlit = new osg::Geode;
        MWRender::attachBloodLighting(*unlit, *fixtureRoot);
        EXPECT_EQ(unlit->getCullCallback(), nullptr);
    }

    class CaptureBloodLightState : public osg::NodeCallback
    {
    public:
        osg::ref_ptr<const osg::StateSet> mState;
        void operator()(osg::Node*, osg::NodeVisitor* visitor) override
        {
            auto* cull = dynamic_cast<osgUtil::CullVisitor*>(visitor);
            if (cull)
                mState = cull->getCurrentStateGraph()->getStateSet();
        }
    };

    TEST(BloodNativeLighting, installedCallbackPushesNativeLightsForActualCullTraversal)
    {
        osg::ref_ptr<SceneUtil::LightManager> manager = new SceneUtil::LightManager(bloodLightSettings());
        MWRender::installNativeBloodLighting(*manager, false, true, 1, false);
        auto point = bloodPointLight({ .8f, .5f, .2f, 1 }, 32);
        manager->update(0);
        manager->addLight(point, osg::Matrixf::translate(0, 0, 8), 0);
        osg::ref_ptr<osg::Group> corpse = new osg::Group;
        manager->addChild(corpse);
        osg::ref_ptr<osg::Geode> receiver = new osg::Geode;
        receiver->setInitialBound(osg::BoundingSphere(osg::Vec3f(), 2));
        MWRender::attachBloodLighting(*receiver, *corpse);
        osg::ref_ptr<CaptureBloodLightState> capture = new CaptureBloodLightState;
        receiver->addCullCallback(capture);
        BloodLightView view(0);
        ASSERT_NE(receiver->getCullCallback(), nullptr);
        receiver->getCullCallback()->run(receiver, view.mVisitor);
        ASSERT_TRUE(capture->mState.valid());
        EXPECT_EQ(bloodUniform<int>(*capture->mState, "brutalBloodPointCount"), 1);
        EXPECT_EQ(bloodUniform<osg::Vec3f>(*capture->mState, "brutalBloodPointPosition"), osg::Vec3f(0, 0, -92));
        EXPECT_EQ(view.mVisitor->getCurrentStateGraph(), view.mGraph.get());
    }

    TEST(BloodNativeLighting, nativeShadowLibrariesRemainLinkedWhenShaderManagerReloadsSettings)
    {
        Shader::ShaderManager shaders;
        shaders.setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
        auto defines = Shader::getDefaultDefines();
        for (const auto& [key, value] : SceneUtil::ShadowManager::getShadowsDisabledDefines())
            defines[key] = value;
        shaders.setGlobalDefines(defines);
        const auto vertex = shaders.getShader("compatibility/brutalblood_shadow.vert");
        const auto fragment = shaders.getShader("compatibility/brutalblood_shadow.frag");
        ASSERT_TRUE(vertex.valid());
        ASSERT_TRUE(fragment.valid());
        EXPECT_EQ(vertex->getShaderSource().find('@'), std::string::npos);
        EXPECT_EQ(fragment->getShaderSource().find('@'), std::string::npos);
        EXPECT_NE(vertex->getShaderSource().find("#define SHADOWS 0"), std::string::npos);
        EXPECT_NE(vertex->getShaderSource().find("void setupShadowCoords"), std::string::npos);
        EXPECT_NE(fragment->getShaderSource().find("float unshadowedLightRatio"), std::string::npos);

        osg::ref_ptr<osg::Program> original = new osg::Program;
        osg::ref_ptr<osg::Shader> mainVertex = new osg::Shader(osg::Shader::VERTEX, "void main() {}\n");
        osg::ref_ptr<osg::Shader> mainFragment = new osg::Shader(osg::Shader::FRAGMENT, "void main() {}\n");
        osg::ref_ptr<osg::Shader> fallbackVertex = new osg::Shader(osg::Shader::VERTEX);
        osg::ref_ptr<osg::Shader> fallbackFragment = new osg::Shader(osg::Shader::FRAGMENT);
        fallbackVertex->setName("Brutal blood fallback shadows vertex");
        fallbackFragment->setName("Brutal blood fallback shadows fragment");
        original->addShader(mainVertex);
        original->addShader(mainFragment);
        original->addShader(fallbackVertex);
        original->addShader(fallbackFragment);
        osg::ref_ptr<SceneUtil::LightManager> manager = new SceneUtil::LightManager(bloodLightSettings());
        MWRender::installNativeBloodLighting(*manager, false, true, 1, false, vertex, fragment);
        osg::ref_ptr<osg::Geode> first = new osg::Geode, second = new osg::Geode;
        first->getOrCreateStateSet()->setAttribute(original);
        second->getOrCreateStateSet()->setAttribute(original);
        MWRender::attachBloodLighting(*first, *manager);
        MWRender::attachBloodLighting(*second, *manager);
        const auto* linked = dynamic_cast<const osg::Program*>(first->getStateSet()->getAttribute(osg::StateAttribute::PROGRAM));
        ASSERT_NE(linked, nullptr);
        EXPECT_NE(linked, original.get());
        EXPECT_EQ(linked, second->getStateSet()->getAttribute(osg::StateAttribute::PROGRAM));
        ASSERT_EQ(linked->getNumShaders(), 4u);
        EXPECT_EQ(linked->getShader(0), mainVertex.get());
        EXPECT_EQ(linked->getShader(1), mainFragment.get());
        EXPECT_EQ(linked->getShader(2), vertex.get());
        EXPECT_EQ(linked->getShader(3), fragment.get());
        EXPECT_EQ(original->getShader(2), fallbackVertex.get());
        EXPECT_EQ(original->getShader(3), fallbackFragment.get());

        defines["shadows_enabled"] = "1";
        defines["shadow_texture_unit_list"] = "3,4";
        defines["perspectiveShadowMaps"] = "1";
        defines["disableNormalOffsetShadows"] = "0";
        defines["shadowNormalOffset"] = "0.01";
        defines["limitShadowMapDistance"] = "1";
        shaders.setGlobalDefines(defines);
        // Already attached programs retain ShaderManager's live shader objects,
        // including cascade/region uniforms needed by the surrounding scene.
        EXPECT_EQ(linked->getShader(2), vertex.get());
        EXPECT_EQ(linked->getShader(3), fragment.get());
        EXPECT_EQ(vertex->getShaderSource().find('@'), std::string::npos);
        EXPECT_EQ(fragment->getShaderSource().find('@'), std::string::npos);
        EXPECT_NE(vertex->getShaderSource().find("#define SHADOWS 1"), std::string::npos);
        EXPECT_NE(vertex->getShaderSource().find("shadowSpaceMatrix3"), std::string::npos);
        EXPECT_NE(vertex->getShaderSource().find("validRegionMatrix4"), std::string::npos);
        EXPECT_NE(fragment->getShaderSource().find("shadowTexture3"), std::string::npos);
        EXPECT_NE(fragment->getShaderSource().find("shadowTexture4"), std::string::npos);
    }
}
