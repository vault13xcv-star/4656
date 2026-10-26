#include "renderingmanager.hpp"
#include "brutalbloodlighting.hpp"
#include "bloodsurfacevisitor.hpp"

#include <cstdlib>
#include <limits>

#include <osg/ClipControl>
#include <osg/ComputeBoundsVisitor>
#include <osg/Group>
#include <osg/Light>
#include <osg/Material>
#include <osg/Matrix>
#include <osg/UserDataContainer>

#include <osgUtil/LineSegmentIntersector>

#include <osgViewer/Viewer>

#include <components/nifosg/nifloader.hpp>

#include <components/debug/debuglog.hpp>

#include <components/stereo/multiview.hpp>
#include <components/stereo/stereomanager.hpp>

#include <components/resource/imagemanager.hpp>
#include <components/resource/keyframemanager.hpp>
#include <components/resource/resourcesystem.hpp>

#include <components/shader/removedalphafunc.hpp>
#include <components/shader/shadermanager.hpp>

#include <components/settings/values.hpp>

#include <components/sceneutil/cullsafeboundsvisitor.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/hitlocation.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/sceneutil/stateupdater.hpp>
#include <components/sceneutil/visitor.hpp>
#include <components/sceneutil/workqueue.hpp>
#include <components/sceneutil/writescene.hpp>

#include <components/misc/constants.hpp>
#include <components/ragdoll/blood.hpp>

#include <components/terrain/quadtreeworld.hpp>
#include <components/terrain/terraingrid.hpp>

#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm/util.hpp>
#include <components/esm4/loadcell.hpp>

#include <components/debug/debugdraw.hpp>
#include <components/detournavigator/navigator.hpp>
#include <components/detournavigator/navmeshcacheitem.hpp>

#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/groundcoverstore.hpp"
#include "../mwworld/scene.hpp"

#include "../mwgui/postprocessorhud.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "actorspaths.hpp"
#include "camera.hpp"
#include "effectmanager.hpp"
#include "brutalblood.hpp"
#include "../mwworld/esmstore.hpp"
#include <components/esm3/loadgmst.hpp>
#include <components/fallback/fallback.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/settings/categories/ragdoll.hpp>
#include "fogmanager.hpp"
#include "groundcover.hpp"
#include "navmesh.hpp"
#include "npcanimation.hpp"
#include "objectpaging.hpp"
#include "pathgrid.hpp"
#include "postprocessor.hpp"
#include "recastmesh.hpp"
#include "screenshotmanager.hpp"
#include "sky.hpp"
#include "terrainstorage.hpp"
#include "util.hpp"
#include "vismask.hpp"
#include "water.hpp"

namespace MWRender
{
    class PreloadCommonAssetsWorkItem : public SceneUtil::WorkItem
    {
    public:
        PreloadCommonAssetsWorkItem(Resource::ResourceSystem* resourceSystem)
            : mResourceSystem(resourceSystem)
        {
        }

        void doWork() override
        {
            try
            {
                for (const VFS::Path::Normalized& v : mModels)
                    mResourceSystem->getSceneManager()->getTemplate(v);
                for (const VFS::Path::Normalized& v : mTextures)
                    mResourceSystem->getImageManager()->getImage(v);
                for (const VFS::Path::Normalized& v : mKeyframes)
                    mResourceSystem->getKeyframeManager()->get(v);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "Failed to preload common assets: " << e.what();
            }
        }

        std::vector<VFS::Path::Normalized> mModels;
        std::vector<VFS::Path::Normalized> mTextures;
        std::vector<VFS::Path::Normalized> mKeyframes;

    private:
        Resource::ResourceSystem* mResourceSystem;
    };

    RenderingManager::RenderingManager(osgViewer::Viewer* viewer, osg::ref_ptr<osg::Group> rootNode,
        Resource::ResourceSystem* resourceSystem, SceneUtil::WorkQueue* workQueue,
        DetourNavigator::Navigator& navigator, const MWWorld::GroundcoverStore& groundcoverStore,
        SceneUtil::UnrefQueue& unrefQueue)
        : mSkyBlending(Settings::fog().mSkyBlending)
        , mViewer(viewer)
        , mRootNode(rootNode)
        , mResourceSystem(resourceSystem)
        , mWorkQueue(workQueue)
        , mNavigator(navigator)
        , mNightEyeFactor(0.f)
        // TODO: Near clip should not need to be bounded like this, but too small values break OSG shadow calculations
        // CPU-side. See issue: #6072
        , mNearClip(Settings::camera().mNearClip)
        , mViewDistance(Settings::camera().mViewingDistance)
        , mFieldOfViewOverridden(false)
        , mFieldOfViewOverride(0.f)
        , mFieldOfView(Settings::camera().mFieldOfView)
        , mFirstPersonFieldOfView(Settings::camera().mFirstPersonFieldOfView)
        , mGroundCoverStore(groundcoverStore)
    {
        bool reverseZ = SceneUtil::AutoDepth::isReversed();

        resourceSystem->getSceneManager()->setParticleSystemMask(MWRender::Mask_ParticleSystem);
        resourceSystem->getSceneManager()->setAutoUseNormalMaps(Settings::shaders().mAutoUseObjectNormalMaps);
        resourceSystem->getSceneManager()->setNormalMapPattern(Settings::shaders().mNormalMapPattern);
        resourceSystem->getSceneManager()->setNormalHeightMapPattern(Settings::shaders().mNormalHeightMapPattern);
        resourceSystem->getSceneManager()->setAutoUseSpecularMaps(Settings::shaders().mAutoUseObjectSpecularMaps);
        resourceSystem->getSceneManager()->setSpecularMapPattern(Settings::shaders().mSpecularMapPattern);
        resourceSystem->getSceneManager()->setConvertAlphaTestToAlphaToCoverage(shouldAddMSAAIntermediateTarget());
        resourceSystem->getSceneManager()->setAdjustCoverageForAlphaTest(
            Settings::shaders().mAdjustCoverageForAlphaTest);

        // Let LightManager choose which backend to use based on our hint.
        // Ultimately dependent on support for various OpenGL extensions.
        osg::ref_ptr<SceneUtil::LightManager> sceneRoot = new SceneUtil::LightManager(SceneUtil::LightSettings{
            .mLightingMethod = Settings::shaders().mLightingMethod,
            .mMaxLights = Settings::shaders().mMaxLights,
            .mMaximumLightDistance = Settings::shaders().mMaximumLightDistance,
            .mLightFadeStart = Settings::shaders().mLightFadeStart,
            .mLightBoundsMultiplier = Settings::shaders().mLightBoundsMultiplier,
        });
        resourceSystem->getSceneManager()->setLightingMethod(sceneRoot->getLightingMethod());
        resourceSystem->getSceneManager()->setSupportedLightingMethods(sceneRoot->getSupportedLightingMethods());

        sceneRoot->setLightingMask(Mask_Lighting);
        mSceneRoot = sceneRoot;
        sceneRoot->setStartLight(1);
        sceneRoot->setNodeMask(Mask_Scene);
        sceneRoot->setName("Scene Root");

        int shadowCastingTraversalMask = Mask_Scene;
        if (Settings::shadows().mActorShadows)
            shadowCastingTraversalMask |= Mask_Actor;
        if (Settings::shadows().mPlayerShadows)
            shadowCastingTraversalMask |= Mask_Player;

        int indoorShadowCastingTraversalMask = shadowCastingTraversalMask;
        if (Settings::shadows().mObjectShadows)
            shadowCastingTraversalMask |= (Mask_Object | Mask_Static);
        if (Settings::shadows().mTerrainShadows)
            shadowCastingTraversalMask |= Mask_Terrain;

        mShadowManager = std::make_unique<SceneUtil::ShadowManager>(sceneRoot, mRootNode, shadowCastingTraversalMask,
            indoorShadowCastingTraversalMask, Mask_Terrain | Mask_Object | Mask_Static, Settings::shadows(),
            mResourceSystem->getSceneManager()->getShaderManager());

        Shader::ShaderManager::DefineMap globalDefines = Shader::getDefaultDefines();
        Shader::ShaderManager::DefineMap shadowDefines = mShadowManager->getShadowDefines(Settings::shadows());
        Shader::ShaderManager::DefineMap lightDefines = sceneRoot->getLightDefines();

        for (auto itr = shadowDefines.begin(); itr != shadowDefines.end(); itr++)
            globalDefines[itr->first] = itr->second;

        globalDefines["forcePPL"] = Settings::shaders().mForcePerPixelLighting ? "1" : "0";
        globalDefines["clamp"] = Settings::shaders().mClampLighting ? "1" : "0";
        globalDefines["preLightEnv"] = Settings::shaders().mApplyLightingToEnvironmentMaps ? "1" : "0";
        globalDefines["classicFalloff"] = Settings::shaders().mClassicFalloff ? "1" : "0";
        const bool exponentialFog = Settings::fog().mExponentialFog;
        globalDefines["radialFog"] = (exponentialFog || Settings::fog().mRadialFog) ? "1" : "0";
        globalDefines["exponentialFog"] = exponentialFog ? "1" : "0";
        globalDefines["skyBlending"] = mSkyBlending ? "1" : "0";

        for (auto itr = lightDefines.begin(); itr != lightDefines.end(); itr++)
            globalDefines[itr->first] = itr->second;

        // Refactor this at some point - most shaders don't care about these defines
        const float groundcoverDistance = Settings::groundcover().mRenderingDistance;
        globalDefines["groundcoverFadeStart"] = std::to_string(groundcoverDistance * 0.9f);
        globalDefines["groundcoverFadeEnd"] = std::to_string(groundcoverDistance);
        globalDefines["groundcoverStompMode"] = std::to_string(Settings::groundcover().mStompMode);
        globalDefines["groundcoverStompIntensity"] = std::to_string(Settings::groundcover().mStompIntensity);

        globalDefines["reverseZ"] = reverseZ ? "1" : "0";

        // It is unnecessary to stop/start the viewer as no frames are being rendered yet.
        mResourceSystem->getSceneManager()->getShaderManager().setGlobalDefines(globalDefines);
        auto& bloodShaders = mResourceSystem->getSceneManager()->getShaderManager();
        installNativeBloodLighting(*sceneRoot, Settings::shaders().mClassicFalloff,
            Settings::shaders().mClampLighting, exponentialFog ? 2 : 1,
            exponentialFog || Settings::fog().mRadialFog,
            bloodShaders.getShader("brutalblood_shadow.vert").get(),
            bloodShaders.getShader("brutalblood_shadow.frag", { { "useShadowDebugOverlay", "0" } }).get());

        mNavMesh = std::make_unique<NavMesh>(mRootNode, mWorkQueue, Settings::navigator().mEnableNavMeshRender,
            Settings::navigator().mNavMeshRenderMode);
        mActorsPaths = std::make_unique<ActorsPaths>(mRootNode, Settings::navigator().mEnableAgentsPathsRender);
        mRecastMesh = std::make_unique<RecastMesh>(mRootNode, Settings::navigator().mEnableRecastMeshRender);
        mPathgrid = std::make_unique<Pathgrid>(mRootNode);

        mObjects = std::make_unique<Objects>(mResourceSystem, sceneRoot, unrefQueue);

        if (getenv("OPENMW_DONT_PRECOMPILE") == nullptr)
        {
            mViewer->setIncrementalCompileOperation(new osgUtil::IncrementalCompileOperation);
            mViewer->getIncrementalCompileOperation()->setTargetFrameRate(Settings::cells().mTargetFramerate);
        }

        mDebugDraw = new Debug::DebugDrawer(mResourceSystem->getSceneManager()->getShaderManager());
        mDebugDraw->setNodeMask(Mask_Debug);
        sceneRoot->addChild(mDebugDraw);

        mResourceSystem->getSceneManager()->setIncrementalCompileOperation(mViewer->getIncrementalCompileOperation());

        mEffectManager = std::make_unique<EffectManager>(sceneRoot, mResourceSystem);
        mBloodProjectionBudget=std::make_shared<BloodProjectionBudget>(
            [viewer=osg::observer_ptr<osgViewer::Viewer>(mViewer)] {
                return viewer.valid() && viewer->getFrameStamp() ? std::uint64_t(viewer->getFrameStamp()->getFrameNumber()) : 0;
            });
        mBloodDecals = std::make_unique<BloodDecals>(*sceneRoot);
        const auto loadBloodTextures = [this](std::initializer_list<const char*> paths) {
            std::vector<osg::ref_ptr<osg::Image>> images;
            auto* manager = mResourceSystem->getImageManager();
            for (const auto* path : paths)
            {
                auto image = manager->getImage(VFS::Path::toNormalized(path));
                if (image && image != manager->getWarningImage())
                    images.push_back(std::move(image));
                else
                    Log(Debug::Warning) << "Brutal Morrowind: blood decal texture unavailable: " << path;
            }
            return images;
        };
        mBloodDecals->setTextures(
            loadBloodTextures({ "textures/brutal/blood/floor (1).dds", "textures/brutal/blood/floor (2).dds", "textures/brutal/blood/floor (3).dds" }),
            loadBloodTextures({ "textures/brutal/blood/wall (1).dds", "textures/brutal/blood/wall (2).dds",
                "textures/brutal/blood/walll.dds" }));
        if (auto* compile=mViewer->getIncrementalCompileOperation())
            compile->add(mBloodDecals->createPrecompileNode());

        const std::string& normalMapPattern = Settings::shaders().mNormalMapPattern;
        const std::string& heightMapPattern = Settings::shaders().mNormalHeightMapPattern;
        const std::string& specularMapPattern = Settings::shaders().mTerrainSpecularMapPattern;
        const bool useTerrainNormalMaps = Settings::shaders().mAutoUseTerrainNormalMaps;
        const bool useTerrainSpecularMaps = Settings::shaders().mAutoUseTerrainSpecularMaps;

        mTerrainStorage = std::make_unique<TerrainStorage>(mResourceSystem, normalMapPattern, heightMapPattern,
            useTerrainNormalMaps, specularMapPattern, useTerrainSpecularMaps);

        WorldspaceChunkMgr& chunkMgr = getWorldspaceChunkMgr(ESM::Cell::sDefaultWorldspaceId);
        mTerrain = chunkMgr.mTerrain.get();
        mGroundcover = chunkMgr.mGroundcover.get();
        mObjectPaging = chunkMgr.mObjectPaging.get();

        mStateUpdater = new SceneUtil::StateUpdater();
        sceneRoot->addUpdateCallback(mStateUpdater);

        mSharedUniformStateUpdater = new SceneUtil::SharedUniformStateUpdater(Settings::fog().mSkyBlendingStart);
        rootNode->addUpdateCallback(mSharedUniformStateUpdater);

        mPerViewUniformStateUpdater = new SceneUtil::PerViewUniformStateUpdater(mResourceSystem->getSceneManager(),
            mResourceSystem->getSceneManager()->getShaderManager().reserveGlobalTextureUnits(
                Shader::ShaderManager::Slot::OpaqueDepthTexture));
        rootNode->addCullCallback(mPerViewUniformStateUpdater);

        mPostProcessor = new PostProcessor(*this, viewer, mRootNode, resourceSystem->getVFS());
        resourceSystem->getSceneManager()->setOpaqueDepthTex(
            mPostProcessor->getTexture(PostProcessor::Tex_OpaqueDepth, 0),
            mPostProcessor->getTexture(PostProcessor::Tex_OpaqueDepth, 1));
        resourceSystem->getSceneManager()->setSupportsNormalsRT(mPostProcessor->getSupportsNormalsRT());
        resourceSystem->getSceneManager()->setWeatherParticleOcclusion(Settings::shaders().mWeatherParticleOcclusion);

        // water goes after terrain for correct waterculling order
        mWater = std::make_unique<Water>(
            sceneRoot->getParent(0), sceneRoot, mResourceSystem, mViewer->getIncrementalCompileOperation());

        mCamera = std::make_unique<Camera>(mViewer->getCamera());

        mScreenshotManager = std::make_unique<ScreenshotManager>(viewer);

        mViewer->setLightingMode(osgViewer::View::NO_LIGHT);

        osg::ref_ptr<osg::LightSource> source = new osg::LightSource;
        source->setNodeMask(Mask_Lighting);
        mSunLight = new osg::Light;
        source->setLight(mSunLight);
        mSunLight->setDiffuse(osg::Vec4f(0, 0, 0, 1));
        mSunLight->setAmbient(osg::Vec4f(0, 0, 0, 1));
        mSunLight->setSpecular(osg::Vec4f(0, 0, 0, 0));
        mSunLight->setConstantAttenuation(1.f);
        sceneRoot->setSunlight(mSunLight);
        sceneRoot->addChild(source);

        sceneRoot->getOrCreateStateSet()->setMode(GL_CULL_FACE, osg::StateAttribute::ON);
        sceneRoot->getOrCreateStateSet()->setMode(GL_NORMALIZE, osg::StateAttribute::ON);
        osg::ref_ptr<osg::Material> defaultMat(new osg::Material);
        defaultMat->setColorMode(osg::Material::OFF);
        defaultMat->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
        defaultMat->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
        defaultMat->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.f, 0.f, 0.f));
        sceneRoot->getOrCreateStateSet()->setAttribute(defaultMat);
        sceneRoot->getOrCreateStateSet()->addUniform(new osg::Uniform("emissiveMult", 1.f));
        sceneRoot->getOrCreateStateSet()->addUniform(new osg::Uniform("specStrength", 1.f));
        sceneRoot->getOrCreateStateSet()->addUniform(new osg::Uniform("distortionStrength", 0.f));

        resourceSystem->getSceneManager()->setUpNormalsRTForStateSet(sceneRoot->getOrCreateStateSet(), true);

        mFog = std::make_unique<FogManager>();

        mSky = std::make_unique<SkyManager>(
            sceneRoot, mRootNode, mViewer->getCamera(), resourceSystem->getSceneManager(), mSkyBlending);
        if (mSkyBlending)
        {
            int skyTextureUnit = mResourceSystem->getSceneManager()->getShaderManager().reserveGlobalTextureUnits(
                Shader::ShaderManager::Slot::SkyTexture);
            mPerViewUniformStateUpdater->enableSkyRTT(skyTextureUnit, mSky->getSkyRTT());
        }

        source->setStateSetModes(*mRootNode->getOrCreateStateSet(), osg::StateAttribute::ON);

        osg::Camera::CullingMode cullingMode = osg::Camera::DEFAULT_CULLING | osg::Camera::FAR_PLANE_CULLING;

        if (!Settings::camera().mSmallFeatureCulling)
            cullingMode &= ~(osg::CullStack::SMALL_FEATURE_CULLING);
        else
        {
            mViewer->getCamera()->setSmallFeatureCullingPixelSize(Settings::camera().mSmallFeatureCullingPixelSize);
            cullingMode |= osg::CullStack::SMALL_FEATURE_CULLING;
        }

        mViewer->getCamera()->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
        mViewer->getCamera()->setCullingMode(cullingMode);
        mViewer->getCamera()->setName(Constants::SceneCamera);

        auto mask = ~(Mask_UpdateVisitor | Mask_SimpleWater);
        MWBase::Environment::get().getWindowManager()->setCullMask(mask);
        NifOsg::Loader::setHiddenNodeMask(Mask_UpdateVisitor);
        NifOsg::Loader::setIntersectionDisabledNodeMask(Mask_Effect);
        NifOsg::Loader::setSoftEffectEnabled(Settings::shaders().mSoftParticles);
        Nif::Reader::setLoadUnsupportedFiles(Settings::models().mLoadUnsupportedNifFiles);

        mStateUpdater->setFogEnd(mViewDistance);

        // Hopefully, anything genuinely requiring the default alpha func of GL_ALWAYS explicitly sets it
        mRootNode->getOrCreateStateSet()->setAttribute(Shader::RemovedAlphaFunc::getInstance(GL_ALWAYS));
        // The transparent renderbin sets alpha testing on because that was faster on old GPUs. It's now slower and
        // breaks things.
        mRootNode->getOrCreateStateSet()->setMode(GL_ALPHA_TEST, osg::StateAttribute::OFF);

        if (reverseZ)
        {
            osg::ref_ptr<osg::ClipControl> clipcontrol
                = new osg::ClipControl(osg::ClipControl::LOWER_LEFT, osg::ClipControl::ZERO_TO_ONE);
            mRootNode->getOrCreateStateSet()->setAttributeAndModes(new SceneUtil::AutoDepth, osg::StateAttribute::ON);
            mRootNode->getOrCreateStateSet()->setAttributeAndModes(clipcontrol, osg::StateAttribute::ON);
        }

        mRootNode->getOrCreateStateSet()->setMode(
            GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::PROTECTED | osg::StateAttribute::OVERRIDE);

        SceneUtil::setCameraClearDepth(mViewer->getCamera());

        updateProjectionMatrix();

        mViewer->getCamera()->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }

    RenderingManager::~RenderingManager()
    {
        // let background loading thread finish before we delete anything else
        mWorkQueue = nullptr;
    }

    osgUtil::IncrementalCompileOperation* RenderingManager::getIncrementalCompileOperation()
    {
        return mViewer->getIncrementalCompileOperation();
    }

    MWRender::Objects& RenderingManager::getObjects()
    {
        return *mObjects.get();
    }

    Resource::ResourceSystem* RenderingManager::getResourceSystem()
    {
        return mResourceSystem;
    }

    SceneUtil::WorkQueue* RenderingManager::getWorkQueue()
    {
        return mWorkQueue.get();
    }

    Terrain::World* RenderingManager::getTerrain()
    {
        return mTerrain;
    }

    void RenderingManager::preloadCommonAssets()
    {
        osg::ref_ptr<PreloadCommonAssetsWorkItem> workItem(new PreloadCommonAssetsWorkItem(mResourceSystem));
        mSky->listAssetsToPreload(workItem->mModels, workItem->mTextures);
        mWater->listAssetsToPreload(workItem->mTextures);

        workItem->mModels.push_back(Settings::models().mXbaseanim);
        workItem->mModels.push_back(Settings::models().mXbaseanim1st);
        workItem->mModels.push_back(Settings::models().mXbaseanimfemale);
        workItem->mModels.push_back(Settings::models().mXargonianswimkna);

        workItem->mKeyframes.push_back(Settings::models().mXbaseanimkf);
        workItem->mKeyframes.push_back(Settings::models().mXbaseanim1stkf);
        workItem->mKeyframes.push_back(Settings::models().mXbaseanimfemalekf);
        workItem->mKeyframes.push_back(Settings::models().mXargonianswimknakf);

        workItem->mTextures.emplace_back("textures/_land_default.dds");

        mWorkQueue->addWorkItem(std::move(workItem));
    }

    double RenderingManager::getReferenceTime() const
    {
        return mViewer->getFrameStamp()->getReferenceTime();
    }

    SceneUtil::LightManager* RenderingManager::getLightRoot()
    {
        return mSceneRoot.get();
    }

    void RenderingManager::setNightEyeFactor(float factor)
    {
        if (factor != mNightEyeFactor)
        {
            mNightEyeFactor = factor;
            updateAmbient();
        }
    }

    void RenderingManager::setAmbientColour(const osg::Vec4f& colour)
    {
        mAmbientColor = colour;
        updateAmbient();
    }

    int RenderingManager::skyGetMasserPhase() const
    {
        return mSky->getMasserPhase();
    }

    int RenderingManager::skyGetSecundaPhase() const
    {
        return mSky->getSecundaPhase();
    }

    void RenderingManager::skySetMoonColour(bool red)
    {
        mSky->setMoonColour(red);
    }

    void RenderingManager::configureAmbient(const MWWorld::Cell& cell)
    {
        bool isInterior = !cell.isExterior() && !cell.isQuasiExterior();
        bool needsAdjusting = false;
        needsAdjusting = isInterior && !Settings::shaders().mClassicFalloff;

        osg::Vec4f ambient = SceneUtil::colourFromRGB(cell.getMood().mAmbiantColor);

        if (needsAdjusting)
        {
            constexpr float pR = 0.2126f;
            constexpr float pG = 0.7152f;
            constexpr float pB = 0.0722f;

            // we already work in linear RGB so no conversions are needed for the luminosity function
            float relativeLuminance = pR * ambient.r() + pG * ambient.g() + pB * ambient.b();
            const float minimumAmbientLuminance = Settings::shaders().mMinimumInteriorBrightness;
            if (relativeLuminance < minimumAmbientLuminance)
            {
                // brighten ambient so it reaches the minimum threshold but no more, we want to mess with content data
                // as least we can
                if (ambient.r() == 0.f && ambient.g() == 0.f && ambient.b() == 0.f)
                    ambient = osg::Vec4(
                        minimumAmbientLuminance, minimumAmbientLuminance, minimumAmbientLuminance, ambient.a());
                else
                    ambient *= minimumAmbientLuminance / relativeLuminance;
            }
        }

        setAmbientColour(ambient);

        osg::Vec4f diffuse = SceneUtil::colourFromRGB(cell.getMood().mDirectionalColor);

        setSunColour(diffuse, diffuse, 1.f);
        // This is total nonsense but it's what Morrowind uses
        static const osg::Vec4f interiorSunPos
            = osg::Vec4f(-1.f, osg::DegreesToRadians(45.f), osg::DegreesToRadians(45.f), 0.f);
        mPostProcessor->getStateUpdater()->setSunPos(interiorSunPos, false);
        mPostProcessor->getStateUpdater()->setSunVec(-interiorSunPos);
        mSunLight->setPosition(interiorSunPos);
    }

    void RenderingManager::setSunColour(const osg::Vec4f& diffuse, const osg::Vec4f& specular, float sunVis)
    {
        // need to wrap this in a StateUpdater?
        mSunLight->setDiffuse(diffuse);
        mSunLight->setSpecular(osg::Vec4f(specular.x(), specular.y(), specular.z(), specular.w() * sunVis));

        mPostProcessor->getStateUpdater()->setSunColor(diffuse);
        mPostProcessor->getStateUpdater()->setSunVis(sunVis);
    }

    void RenderingManager::setSunDirection(const osg::Vec3f& direction)
    {
        osg::Vec3f position = -direction;

        // This is based on the exterior sun orbit and won't make sense for interiors, see WeatherManager::update
        position.z() = 400.f - std::abs(position.x());

        // The sun is not always synchronized with the sunlight because reasons
        const osg::Vec3f sunlightPos = Settings::shaders().mMatchSunlightToSun ? position : -direction;
        // need to wrap this in a StateUpdater?
        mSunLight->setPosition(osg::Vec4f(sunlightPos, 0.f));

        mSky->setSunDirection(position);

        mPostProcessor->getStateUpdater()->setSunPos(osg::Vec4f(position, 0.f), mNight);
        mPostProcessor->getStateUpdater()->setSunVec(osg::Vec4f(-sunlightPos, 0.f));
    }

    void RenderingManager::addCell(const MWWorld::CellStore* store)
    {
        invalidateBloodSurfaces(nullptr, store);
        mBloodDecals->setCellActive(store->getCell()->getId().serializeText(), true);
        mPathgrid->addCell(store);

        mWater->changeCell(store);

        if (store->getCell()->isExterior())
        {
            enableTerrain(true, store->getCell()->getWorldSpace());
            mTerrain->loadCell(store->getCell()->getGridX(), store->getCell()->getGridY());
        }
    }
    void RenderingManager::removeCell(const MWWorld::CellStore* store)
    {
        invalidateBloodSurfaces(nullptr, store);
        const auto bloodCell = store->getCell()->getId().serializeText();
        std::erase_if(mBloodBursts, [&](const auto& burst) { return burst.mCell == bloodCell; });
        mBloodDecals->setCellActive(store->getCell()->getId().serializeText(), false);
        mPathgrid->removeCell(store);
        mActorsPaths->removeCell(store);
        mObjects->removeCell(store);

        if (store->getCell()->isExterior())
        {
            getWorldspaceChunkMgr(store->getCell()->getWorldSpace())
                .mTerrain->unloadCell(store->getCell()->getGridX(), store->getCell()->getGridY());
        }

        mWater->removeCell(store);
    }

    void RenderingManager::enableTerrain(bool enable, ESM::RefId worldspace)
    {
        if (!enable)
            mWater->setCullCallback(nullptr);
        else
        {
            WorldspaceChunkMgr& newChunks = getWorldspaceChunkMgr(worldspace);
            if (newChunks.mTerrain.get() != mTerrain)
            {
                mTerrain->enable(false);
                mTerrain = newChunks.mTerrain.get();
                mGroundcover = newChunks.mGroundcover.get();
                mObjectPaging = newChunks.mObjectPaging.get();
            }
        }
        mTerrain->enable(enable);
    }

    void RenderingManager::setSkyEnabled(bool enabled)
    {
        mSky->setEnabled(enabled);
        if (enabled)
            mShadowManager->enableOutdoorMode();
        else
            mShadowManager->enableIndoorMode(Settings::shadows());
        mPostProcessor->getStateUpdater()->setIsInterior(!enabled);
    }

    bool RenderingManager::toggleBorders()
    {
        bool borders = !mTerrain->getBordersVisible();
        mTerrain->setBordersVisible(borders);
        return borders;
    }

    bool RenderingManager::toggleRenderMode(RenderMode mode)
    {
        if (mode == Render_CollisionDebug || mode == Render_Pathgrid)
            return mPathgrid->toggleRenderMode(mode);
        else if (mode == Render_Wireframe)
        {
            bool wireframe = !mStateUpdater->getWireframe();
            mStateUpdater->setWireframe(wireframe);
            return wireframe;
        }
        else if (mode == Render_Water)
        {
            return mWater->toggle();
        }
        else if (mode == Render_Scene)
        {
            const auto wm = MWBase::Environment::get().getWindowManager();
            unsigned int mask = wm->getCullMask();
            bool enabled = !(mask & sToggleWorldMask);
            if (enabled)
                mask |= sToggleWorldMask;
            else
                mask &= ~sToggleWorldMask;
            mWater->showWorld(enabled);
            wm->setCullMask(mask);
            return enabled;
        }
        else if (mode == Render_NavMesh)
        {
            return mNavMesh->toggle();
        }
        else if (mode == Render_ActorsPaths)
        {
            return mActorsPaths->toggle();
        }
        else if (mode == Render_RecastMesh)
        {
            return mRecastMesh->toggle();
        }
        return false;
    }

    void RenderingManager::configureFog(const MWWorld::Cell& cell)
    {
        mFog->configure(mViewDistance, cell);
    }

    void RenderingManager::configureFog(
        float fogDepth, float underwaterFog, float dlFactor, float dlOffset, const osg::Vec4f& color)
    {
        mFog->configure(mViewDistance, fogDepth, underwaterFog, dlFactor, dlOffset, color);
    }

    SkyManager* RenderingManager::getSkyManager()
    {
        return mSky.get();
    }

    void RenderingManager::update(float dt, bool paused)
    {
        reportStats();

        mResourceSystem->getSceneManager()->getShaderManager().update(*mViewer);

        mWater->setRainIntensity(mSky->getRainRipplesEnabled() ? mSky->getPrecipitationAlpha() : 0.f);

        mWater->update(dt, paused);
        if (!paused)
        {
            mBloodSurfaceTime += std::max(0.f, dt);
            mEffectManager->update(dt);
            mBloodDecals->update(dt, Settings::ragdoll().mBloodLifetime.get());
            if (Settings::ragdoll().mBloodAmount.get()<=0) mBloodBursts.clear();
            unsigned bloodSnapshots = 0;
            for (auto& burst : mBloodBursts)
            {
                burst.mAge += dt;
                if (!burst.mVictim.isEmpty() && burst.mAge < burst.mEmissionDuration)
                {
                    burst.mSource.mPosition = burst.mVictim.getRefData().getPosition().asVec3()
                        + osg::Vec3f(0, 0, burst.mBodyHeight);
                    if (const auto* animation = getAnimation(burst.mVictim))
                    {
                        osg::Vec3f corpsePosition;
                        if (animation->getRagdollPosition(corpsePosition))
                            burst.mSource.mPosition = corpsePosition;
                    }
                }
                // Reuse nearby shared geometry, refreshing at most twice per
                // frame globally (the same budget is used by corpse wounds).
                // Keep refreshing after emission stops until flying drops land.
                burst.mSinceSnapshot += dt;
                if (bloodSnapshots < 2 && (burst.mSinceSnapshot >= .5f || !bool(*burst.mSurface)))
                {
                    ++bloodSnapshots;
                    const float radius = 768.f * std::max(1.f,
                        Ragdoll::bloodLaunchScale(Settings::ragdoll().mBloodAmount.get()));
                    auto surface = getBloodSurface(burst.mReceiverCell, burst.mSource.mPosition, radius);
                    if (surface)
                    {
                        *burst.mSurface = std::move(surface);
                        burst.mSnapshotCenter = burst.mSource.mPosition;
                        burst.mSinceSnapshot = 0;
                    }
                }
                const std::vector<BloodTrail::Source> source = burst.mAge < burst.mEmissionDuration
                    ? std::vector<BloodTrail::Source>{ burst.mSource } : std::vector<BloodTrail::Source>{};
                burst.mTrail->update(dt, source, Settings::ragdoll().mBloodAmount.get(), burst.mAge);
            }
            std::erase_if(mBloodBursts, [](const auto& burst) {
                return burst.mAge>burst.mEmissionDuration
                    && ((!burst.mTrail->particleCount() && !burst.mTrail->queuedImpacts())
                        || burst.mAge>burst.mEmissionDuration+BloodTrail::MaxDropAge+BloodTrail::MaxPendingAge+.25f);
            });
            mSky->update(dt);

            const MWWorld::Ptr& player = mPlayerAnimation->getPtr();
            osg::Vec3f playerPos(player.getRefData().getPosition().asVec3());

            float windSpeed = mSky->getBaseWindSpeed();
            mSharedUniformStateUpdater->setWindSpeed(windSpeed);
            mSharedUniformStateUpdater->setPlayerPos(playerPos);
        }

        updateNavMesh();
        updateRecastMesh();

        if (mUpdateProjectionMatrix)
        {
            mUpdateProjectionMatrix = false;
            updateProjectionMatrix();
        }
        mCamera->update(dt, paused);

        bool isUnderwater = mWater->isUnderwater(mCamera->getPosition());

        float fogStart = mFog->getFogStart(isUnderwater);
        float fogEnd = mFog->getFogEnd(isUnderwater);
        osg::Vec4f fogColor = mFog->getFogColor(isUnderwater);

        mStateUpdater->setFogStart(fogStart);
        mStateUpdater->setFogEnd(fogEnd);
        setFogColor(fogColor);

        auto world = MWBase::Environment::get().getWorld();
        const auto& stateUpdater = mPostProcessor->getStateUpdater();

        stateUpdater->setFogRange(fogStart, fogEnd);
        stateUpdater->setNearFar(mNearClip, mViewDistance);
        stateUpdater->setIsUnderwater(isUnderwater);
        stateUpdater->setFogColor(fogColor);
        stateUpdater->setGameHour(world->getTimeStamp().getHour());
        stateUpdater->setWeatherId(world->getCurrentWeatherScriptId());
        stateUpdater->setNextWeatherId(world->getNextWeatherScriptId());
        stateUpdater->setWeatherTransition(world->getWeatherTransition());
        stateUpdater->setWindSpeed(world->getWindSpeed());
        stateUpdater->setSkyColor(mSky->getSkyColor());
        mPostProcessor->setUnderwaterFlag(isUnderwater);
    }

    void RenderingManager::updatePlayerPtr(const MWWorld::Ptr& ptr)
    {
        if (mPlayerAnimation.get())
        {
            setupPlayer(ptr);
            mPlayerAnimation->updatePtr(ptr);
        }
        mCamera->attachTo(ptr);
    }

    void RenderingManager::removePlayer(const MWWorld::Ptr& player)
    {
        mWater->removeEmitter(player);
    }

    void RenderingManager::rotateObject(const MWWorld::Ptr& ptr, const osg::Quat& rot)
    {
        const bool receiverChanged = !ptr.getClass().isActor() && ptr.getRefData().getBaseNode()->getAttitude() != rot;
        if (receiverChanged) invalidateBloodSurfaces(&ptr);
        if (ptr == mCamera->getTrackingPtr() && !mCamera->isVanityOrPreviewModeEnabled())
        {
            mCamera->rotateCameraToTrackingPtr();
        }

        ptr.getRefData().getBaseNode()->setAttitude(rot);
        if (receiverChanged) invalidateBloodSurfaces(&ptr);
    }

    void RenderingManager::moveObject(const MWWorld::Ptr& ptr, const osg::Vec3f& pos)
    {
        const bool receiverChanged = !ptr.getClass().isActor() && ptr.getRefData().getBaseNode()->getPosition() != pos;
        if (receiverChanged) invalidateBloodSurfaces(&ptr);
        ptr.getRefData().getBaseNode()->setPosition(pos);
        if (receiverChanged) invalidateBloodSurfaces(&ptr);
    }

    void RenderingManager::scaleObject(const MWWorld::Ptr& ptr, const osg::Vec3f& scale)
    {
        const bool receiverChanged = !ptr.getClass().isActor() && ptr.getRefData().getBaseNode()->getScale() != scale;
        if (receiverChanged) invalidateBloodSurfaces(&ptr);
        ptr.getRefData().getBaseNode()->setScale(scale);
        if (receiverChanged) invalidateBloodSurfaces(&ptr);

        if (ptr == mCamera->getTrackingPtr()) // update height of camera
            mCamera->processViewChange();
    }

    void RenderingManager::removeObject(const MWWorld::Ptr& ptr)
    {
        if (!ptr.getClass().isActor()) invalidateBloodSurfaces(&ptr);
        std::erase_if(mBloodBursts, [&](const auto& burst) { return burst.mVictim == ptr; });
        mActorsPaths->remove(ptr);
        mObjects->removeObject(ptr);
        mWater->removeEmitter(ptr);
    }

    void RenderingManager::setWaterEnabled(bool enabled)
    {
        mWater->setEnabled(enabled);
        mSky->setWaterEnabled(enabled);

        mPostProcessor->getStateUpdater()->setIsWaterEnabled(enabled);
    }

    void RenderingManager::setWaterHeight(float height)
    {
        mWater->setCullCallback(mTerrain->getHeightCullCallback(height, Mask_Water));
        mWater->setHeight(height);
        mSky->setWaterHeight(height);

        mPostProcessor->getStateUpdater()->setWaterHeight(height);
    }

    void RenderingManager::screenshot(osg::Image* image, int w, int h)
    {
        mScreenshotManager->screenshot(image, w, h);
    }

    osg::Vec2f RenderingManager::getScreenCoords(const osg::BoundingBox& bb)
    {
        if (bb.valid())
        {
            const osg::Matrix viewProj
                = mViewer->getCamera()->getViewMatrix() * mViewer->getCamera()->getProjectionMatrix();
            const osg::Vec3f worldPoint((bb.xMin() + bb.xMax()) * 0.5f, (bb.yMin() + bb.yMax()) * 0.5f, bb.zMax());
            const osg::Vec4f clipPoint = osg::Vec4f(worldPoint, 1.0f) * viewProj;
            if (clipPoint.w() > 0.f)
            {
                const float screenPointX = (clipPoint.x() / clipPoint.w() + 1.f) * 0.5f;
                const float screenPointY = (clipPoint.y() / clipPoint.w() - 1.f) * (-0.5f);
                if (screenPointX >= 0.f && screenPointX <= 1.f && screenPointY >= 0.f && screenPointY <= 1.f)
                    return osg::Vec2f(screenPointX, screenPointY);
            }
        }

        return osg::Vec2f(0.5f, 0.f);
    }

    RenderingManager::RayResult getIntersectionResult(osgUtil::LineSegmentIntersector* intersector,
        const osg::ref_ptr<osgUtil::IntersectionVisitor>& visitor, std::span<const MWWorld::Ptr> ignoreList = {})
    {
        constexpr auto nonObjectWorldMask = Mask_Terrain | Mask_Water;
        RenderingManager::RayResult result;
        result.mHit = false;
        result.mRatio = 0;

        if (!intersector->containsIntersections())
            return result;

        auto test = [&](const osgUtil::LineSegmentIntersector::Intersection& intersection) {
            PtrHolder* ptrHolder = nullptr;
            std::vector<RefnumMarker*> refnumMarkers;
            bool hitNonObjectWorld = false;
            for (osg::Node* node : intersection.nodePath)
            {
                const auto& nodeMask = node->getNodeMask();
                if (!hitNonObjectWorld)
                    hitNonObjectWorld = nodeMask & nonObjectWorldMask;

                osg::UserDataContainer* userDataContainer = node->getUserDataContainer();
                if (!userDataContainer)
                    continue;
                for (unsigned int i = 0; i < userDataContainer->getNumUserObjects(); ++i)
                {
                    if (PtrHolder* p = dynamic_cast<PtrHolder*>(userDataContainer->getUserObject(i)))
                    {
                        if (std::find(ignoreList.begin(), ignoreList.end(), p->mPtr) == ignoreList.end())
                        {
                            ptrHolder = p;
                        }
                    }
                    if (RefnumMarker* r = dynamic_cast<RefnumMarker*>(userDataContainer->getUserObject(i)))
                    {
                        refnumMarkers.push_back(r);
                    }
                }
            }

            if (ptrHolder)
                result.mHitObject = ptrHolder->mPtr;

            unsigned int vertexCounter = 0;
            for (unsigned int i = 0; i < refnumMarkers.size(); ++i)
            {
                unsigned int intersectionIndex = intersection.indexList.empty() ? 0 : intersection.indexList[0];
                if (!refnumMarkers[i]->mNumVertices
                    || (intersectionIndex >= vertexCounter
                        && intersectionIndex < vertexCounter + refnumMarkers[i]->mNumVertices))
                {
                    auto it = std::find_if(
                        ignoreList.begin(), ignoreList.end(), [target = refnumMarkers[i]->mRefnum](const auto& ptr) {
                            return target == ptr.getCellRef().getRefNum();
                        });

                    if (it == ignoreList.end())
                    {
                        result.mHitRefnum = refnumMarkers[i]->mRefnum;
                    }

                    break;
                }
                vertexCounter += refnumMarkers[i]->mNumVertices;
            }

            if (!result.mHitObject.isEmpty() || result.mHitRefnum.isSet() || hitNonObjectWorld)
            {
                result.mHit = true;
                result.mHitPointWorld = intersection.getWorldIntersectPoint();
                result.mHitNormalWorld = intersection.getWorldIntersectNormal();
                result.mRatio = static_cast<float>(intersection.ratio);
                result.mHitBody = SceneUtil::getHitBody(intersection.nodePath);
            }
        };

        if (ignoreList.empty() || intersector->getIntersectionLimit() != osgUtil::LineSegmentIntersector::NO_LIMIT)
        {
            test(intersector->getFirstIntersection());
        }
        else
        {
            for (const auto& intersection : intersector->getIntersections())
            {
                test(intersection);

                if (result.mHit)
                {
                    break;
                }
            }
        }

        return result;
    }

    class IntersectionVisitorWithIgnoreList : public osgUtil::IntersectionVisitor
    {
    public:
        bool skipTransform(osg::Transform& transform)
        {
            if (mContainsPagedRefs)
                return false;

            osg::UserDataContainer* userDataContainer = transform.getUserDataContainer();
            if (!userDataContainer)
                return false;

            for (unsigned int i = 0; i < userDataContainer->getNumUserObjects(); ++i)
            {
                if (PtrHolder* p = dynamic_cast<PtrHolder*>(userDataContainer->getUserObject(i)))
                {
                    if (std::find(mIgnoreList.begin(), mIgnoreList.end(), p->mPtr) != mIgnoreList.end())
                    {
                        return true;
                    }
                }
            }

            return false;
        }

        void apply(osg::Transform& transform) override
        {
            if (skipTransform(transform))
            {
                return;
            }
            osgUtil::IntersectionVisitor::apply(transform);
        }

        void setIgnoreList(std::span<const MWWorld::Ptr> ignoreList) { mIgnoreList = ignoreList; }
        void setContainsPagedRefs(bool contains) { mContainsPagedRefs = contains; }

    private:
        std::span<const MWWorld::Ptr> mIgnoreList;
        bool mContainsPagedRefs = false;
    };

    osg::ref_ptr<osgUtil::IntersectionVisitor> RenderingManager::getIntersectionVisitor(
        osgUtil::Intersector* intersector, bool ignorePlayer, bool ignoreActors,
        std::span<const MWWorld::Ptr> ignoreList)
    {
        if (!mIntersectionVisitor)
            mIntersectionVisitor = new IntersectionVisitorWithIgnoreList;

        mIntersectionVisitor->setIgnoreList(ignoreList);
        mIntersectionVisitor->setContainsPagedRefs(false);

        MWWorld::Scene* worldScene = MWBase::Environment::get().getWorldScene();
        for (const auto& ptr : ignoreList)
        {
            if (worldScene->isPagedRef(ptr))
            {
                mIntersectionVisitor->setContainsPagedRefs(true);
                intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::NO_LIMIT);
                break;
            }
        }

        mIntersectionVisitor->setTraversalNumber(mViewer->getFrameStamp()->getFrameNumber());
        mIntersectionVisitor->setFrameStamp(mViewer->getFrameStamp());
        mIntersectionVisitor->setIntersector(intersector);

        unsigned int mask = ~0u;
        mask &= ~(Mask_RenderToTexture | Mask_Sky | Mask_Debug | Mask_Effect | Mask_Water | Mask_SimpleWater
            | Mask_Groundcover);
        if (ignorePlayer)
            mask &= ~(Mask_Player);
        if (ignoreActors)
            mask &= ~(Mask_Actor | Mask_Player);

        mIntersectionVisitor->setTraversalMask(mask);
        return mIntersectionVisitor;
    }

    RenderingManager::RayResult RenderingManager::castRay(const osg::Vec3f& origin, const osg::Vec3f& dest,
        bool ignorePlayer, bool ignoreActors, std::span<const MWWorld::Ptr> ignoreList)
    {
        osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector(
            new osgUtil::LineSegmentIntersector(osgUtil::LineSegmentIntersector::MODEL, origin, dest));
        intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::LIMIT_NEAREST);

        mRootNode->accept(*getIntersectionVisitor(intersector, ignorePlayer, ignoreActors, ignoreList));

        return getIntersectionResult(intersector, mIntersectionVisitor, ignoreList);
    }

    RenderingManager::RayResult RenderingManager::castCameraToViewportRay(
        const float nX, const float nY, float maxDistance, bool ignorePlayer, bool ignoreActors)
    {
        osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector(new osgUtil::LineSegmentIntersector(
            osgUtil::LineSegmentIntersector::PROJECTION, nX * 2.f - 1.f, nY * (-2.f) + 1.f));

        osg::Vec3d dist(0.f, 0.f, -maxDistance);

        dist = dist * mViewer->getCamera()->getProjectionMatrix();

        osg::Vec3d end = intersector->getEnd();
        end.z() = dist.z();
        intersector->setEnd(end);
        intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::LIMIT_NEAREST);

        mViewer->getCamera()->accept(*getIntersectionVisitor(intersector, ignorePlayer, ignoreActors));

        return getIntersectionResult(intersector, mIntersectionVisitor);
    }

    void RenderingManager::updatePtr(const MWWorld::Ptr& old, const MWWorld::Ptr& updated)
    {
        for (auto& burst : mBloodBursts)
            if (burst.mVictim == old)
            {
                burst.mVictim = updated;
                if (updated.isInCell())
                {
                    burst.mReceiverCell = updated.getCell();
                    burst.mCell = updated.getCell()->getCell()->getId().serializeText();
                    if (burst.mDepositCell)
                        *burst.mDepositCell = burst.mCell;
                }
            }
        mObjects->updatePtr(old, updated);
        mActorsPaths->updatePtr(old, updated);
    }

    void RenderingManager::spawnEffect(VFS::Path::NormalizedView model, std::string_view texture,
        const osg::Vec3f& worldPosition, float scale, bool isMagicVFX, bool useAmbientLight, std::string_view effectId,
        bool loop)
    {
        const float bloodAmount = Ragdoll::bloodEmissionScale(Settings::ragdoll().mBloodAmount.get());
        if (bloodAmount != 1.f)
            for (int i = 0; i < 3; ++i)
            {
                const auto key = "Blood_Model_" + std::to_string(i);
                const auto* setting = MWBase::Environment::get().getESMStore()->get<ESM::GameSetting>().search(key);
                const auto mesh = setting ? std::string_view(setting->mValue.getString()) : Fallback::Map::getString(key);
                if (!mesh.empty() && model.value() == VFS::Path::Normalized("meshes/" + std::string(mesh)).value())
                {
                    if (bloodAmount <= 0) return;
                    scale *= std::sqrt(bloodAmount);
                    break;
                }
            }
        mEffectManager->addEffect(model, texture, worldPosition, scale, isMagicVFX, useAmbientLight, effectId, loop);
    }

    void RenderingManager::removeEffect(std::string_view effectId)
    {
        mEffectManager->removeEffect(effectId);
    }

    void RenderingManager::invalidateBloodSurfaces(const MWWorld::Ptr* receiver, const MWWorld::CellStore* cell)
    {
        osg::Vec3f center;
        float radius = 0;
        if (receiver && !receiver->isEmpty())
        {
            const auto* node = receiver->getRefData().getBaseNode();
            if (node && node->getBound().valid())
            {
                center = node->getBound().center();
                radius = node->getBound().radius();
            }
            else center = receiver->getRefData().getPosition().asVec3();
        }
        for (auto it = mBloodSurfaceSnapshots.begin(); it != mBloodSurfaceSnapshots.end();)
        {
            if (cell && it->mCell != cell)
            {
                if (!cell->isExterior() || !it->mCell || !it->mCell->isExterior()
                    || cell->getCell()->getWorldSpace() != it->mCell->getCell()->getWorldSpace())
                { ++it; continue; }
                const float size = ESM::getCellSize(cell->getCell()->getWorldSpace());
                const float x = cell->getCell()->getGridX() * size, y = cell->getCell()->getGridY() * size;
                const float dx = it->mCenter.x() - std::clamp(it->mCenter.x(), x, x + size);
                const float dy = it->mCenter.y() - std::clamp(it->mCenter.y(), y, y + size);
                if (dx * dx + dy * dy > it->mRadius * it->mRadius) { ++it; continue; }
            }
            if (receiver)
            {
                if (!receiver->isInCell()) { ++it; continue; }
                if (it->mCell != receiver->getCell()
                    && (!it->mCell || !it->mCell->isExterior() || !receiver->getCell()->isExterior()))
                { ++it; continue; }
                const float reach = it->mRadius + radius;
                if ((it->mCenter - center).length2() > reach * reach) { ++it; continue; }
            }
            // All active users hold a wrapper of this slot. Invalidating it
            // immediately prevents projecting onto a moved/deleted receiver.
            *it->mSurface = {};
            if (it->mBuild) it->mBuild->abort();
            it = mBloodSurfaceSnapshots.erase(it);
        }
    }

    BloodTrail::Surface RenderingManager::getBloodSurface(const MWWorld::CellStore* cell,
        const osg::Vec3f& center, float radius)
    {
        if (!cell || !std::isfinite(center.length2()) || !std::isfinite(radius)) return {};
        radius = std::clamp(radius, 128.f, 1536.f);
        const unsigned frame = mViewer->getFrameStamp() ? mViewer->getFrameStamp()->getFrameNumber() : 0;
        if (mBloodSurfaceFrame != frame)
        {
            mBloodSurfaceFrame = frame;
            mBloodSurfacesBuilt = 0;
        }
        const auto wrap = [](const std::shared_ptr<BloodTrail::Surface>& slot) {
            return BloodTrail::Surface{
                [slot](const auto& from, const auto& to, auto& point, auto& normal) {
                    return slot->mRaycast && slot->mRaycast(from, to, point, normal);
                },
                [slot](const auto& point, const auto& normal, float size) {
                    return slot->mProjector ? slot->mProjector(point, normal, size) : std::vector<osg::Vec3f>{};
                },
                [slot] { return bool(*slot); }
            };
        };
        auto reusable = mBloodSurfaceSnapshots.end();
        for (auto it = mBloodSurfaceSnapshots.begin(); it != mBloodSurfaceSnapshots.end(); ++it)
        {
            const float distance = (it->mCenter - center).length();
            const bool sameWorld = it->mCell == cell || (it->mCell && it->mCell->isExterior() && cell->isExterior()
                && it->mCell->getCell()->getWorldSpace() == cell->getCell()->getWorldSpace());
            if (sameWorld && distance + radius <= it->mRadius)
            {
                if (it->mBuild)
                {
                    if (!it->mBuild->isDone()) return wrap(it->mSurface);
                    *it->mSurface=it->mBuild->takeResult();
                    it->mBuild=nullptr;
                }
                // World-object moves/deletions explicitly invalidate these
                // slots. Reusing unchanged ground longer avoids building a
                // separate terrain BVH twice a second for every open wound.
                if (*it->mSurface)
                {
                    const auto result = wrap(it->mSurface);
                    auto recent = std::move(*it);
                    mBloodSurfaceSnapshots.erase(it);
                    mBloodSurfaceSnapshots.push_back(std::move(recent));
                    return result;
                }
                reusable = it;
                break;
            }
        }
        if (mBloodSurfacesBuilt >= 1) return {};
        ++mBloodSurfacesBuilt;
        const auto filter = [cell](const osg::Node& node) {
            if (cell->isExterior()) return true;
            const auto* data = node.getUserDataContainer();
            if (!data) return true;
            for (unsigned i = 0; i < data->getNumUserObjects(); ++i)
                if (const auto* holder = dynamic_cast<const PtrHolder*>(data->getUserObject(i)))
                    if (!holder->mPtr.isEmpty() && holder->mPtr.isInCell() && holder->mPtr.getCell() != cell)
                        return false;
            return true;
        };
        const auto captureCenter = reusable == mBloodSurfaceSnapshots.end() ? center : reusable->mCenter;
        const float captureRadius = reusable == mBloodSurfaceSnapshots.end() ? radius + 128.f : reusable->mRadius;
        auto triangles = cell->isExterior()
            ? MWBase::Environment::get().getWorld()->getRagdollCollisionTriangles(captureCenter, captureRadius)
            : collectBloodSurfaceTriangles(*mSceneRoot, captureCenter, captureRadius, filter, mViewer->getFrameStamp(), false);
        if (triangles.empty()) return {};
        auto surface = cell->isExterior() ? BloodTrail::collisionSurface(triangles) : BloodTrail::Surface{};
        osg::ref_ptr<BloodSurfaceBuild> build;
        if (!cell->isExterior())
        {
            build=new BloodSurfaceBuild(std::move(triangles));
            mWorkQueue->addWorkItem(build,/*front=*/true);
        }
        if (cell->isExterior() && mBloodSurfaceTime - mBloodSurfaceWarningTime >= 10)
        {
            osg::Vec3f ground, normal;
            if (!surface || !surface.mRaycast(center + osg::Vec3f(0, 0, 8),
                    center - osg::Vec3f(0, 0, captureRadius), ground, normal))
            {
                mBloodSurfaceWarningTime = mBloodSurfaceTime;
                Log(Debug::Warning) << "Brutal Morrowind: exterior blood ground probe missed; cell="
                    << cell->getCell()->getId().serializeText() << " position=" << center.x() << ',' << center.y() << ',' << center.z()
                    << " radius=" << captureRadius << " receiver triangles=" << triangles.size() / 3;
            }
        }
        if (!surface && !build) return {};
        if (reusable != mBloodSurfaceSnapshots.end())
        {
            // Refresh the slot in place. Appending an equivalent snapshot used
            // to evict other corpses' still-live receivers from the 16 slots,
            // intermittently dropping their outdoor ray hits and floor stains.
            *reusable->mSurface = std::move(surface);
            reusable->mBuild=std::move(build);
            reusable->mTime = mBloodSurfaceTime;
            const auto result = wrap(reusable->mSurface);
            auto recent = std::move(*reusable);
            mBloodSurfaceSnapshots.erase(reusable);
            mBloodSurfaceSnapshots.push_back(std::move(recent));
            return result;
        }
        auto slot = std::make_shared<BloodTrail::Surface>(std::move(surface));
        if (mBloodSurfaceSnapshots.size() >= 16)
        {
            *mBloodSurfaceSnapshots.front().mSurface = {};
            if (mBloodSurfaceSnapshots.front().mBuild) mBloodSurfaceSnapshots.front().mBuild->abort();
            mBloodSurfaceSnapshots.pop_front();
        }
        mBloodSurfaceSnapshots.push_back({ cell, center, captureRadius, mBloodSurfaceTime, slot,std::move(build) });
        return wrap(slot);
    }

    void RenderingManager::addBloodDecal(const std::string& cell, const BloodStamp& stamp)
    {
        mBloodDecals->add(cell, stamp);
    }
    void RenderingManager::emitBloodHit(const MWWorld::Ptr& victim, const Ragdoll::Impact& hit)
    {
        if (victim.isEmpty())
            return;
        const auto world = MWBase::Environment::get().getWorld();
        const bool creature = victim.getType() == ESM::Creature::sRecordId;
        if (!victim.isInCell() || (!victim.getClass().isNpc() && !creature) || victim == world->getPlayerPtr()
            || !hit.mHasWeaponContact || Settings::ragdoll().mBloodAmount.get() <= 0) return;
        if (creature)
        {
            const auto& record = *victim.get<ESM::Creature>()->mBase;
            if (!Ragdoll::creatureHasBlood(record.mId.toString(), record.mOriginal.toString(), record.mModel))
                return;
        }
        if (!std::isfinite(hit.mWeaponPosition.length2()) || !std::isfinite(hit.mWeaponDirection.length2()))
            return;
        if (mBloodBursts.size() >= 32
            && std::none_of(mBloodBursts.begin(), mBloodBursts.end(), [](const auto& b) { return b.mVictim.isEmpty(); }))
            return;
        const auto cell = victim.getCell()->getCell()->getId().serializeText();
        BloodBurst burst;
        burst.mCell = cell;
        burst.mReceiverCell = victim.getCell();
        burst.mSource = { hit.mWeaponPosition, -hit.mWeaponDirection, 1, false };
        const float radius = 768.f * std::max(1.f, Ragdoll::bloodLaunchScale(Settings::ragdoll().mBloodAmount.get()));
        auto surface = getBloodSurface(burst.mReceiverCell, burst.mSource.mPosition, radius);
        burst.mSinceSnapshot = surface ? 0.f : .5f;
        burst.mSurface = std::make_shared<BloodTrail::Surface>(std::move(surface));
        burst.mTrail = std::make_unique<BloodTrail>(*mSceneRoot,
            [surface = burst.mSurface](const auto& a, const auto& b, auto& p, auto& n) {
                return surface->mRaycast && surface->mRaycast(a, b, p, n);
            }, [this, cell](const auto& stamp) { mBloodDecals->add(cell, stamp); },
            [surface = burst.mSurface](const auto& p, const auto& n, float r) {
                return surface->mProjector ? surface->mProjector(p, n, r) : std::vector<osg::Vec3f>{};
            }, [surface = burst.mSurface] { return bool(*surface); },mBloodProjectionBudget);
        if (mBloodBursts.size() >= 32)
        {
            const auto disposable = std::find_if(mBloodBursts.begin(), mBloodBursts.end(),
                [](const auto& candidate) { return candidate.mVictim.isEmpty(); });
            if (disposable == mBloodBursts.end())
                return;
            mBloodBursts.erase(disposable);
        }
        mBloodBursts.push_back(std::move(burst));
    }

    void RenderingManager::emitCreatureDeathBlood(const MWWorld::Ptr& victim)
    {
        if (victim.isEmpty() || !victim.isInCell() || victim.getType() != ESM::Creature::sRecordId
            || Settings::ragdoll().mBloodAmount.get() <= 0)
            return;
        const auto& record = *victim.get<ESM::Creature>()->mBase;
        if (!Ragdoll::creatureHasBlood(record.mId.toString(), record.mOriginal.toString(), record.mModel))
            return;
        // Death presentation is independent of supported ragdoll skeletons.
        // A native animated creature therefore also receives a fountain.
        const auto world = MWBase::Environment::get().getWorld();
        BloodBurst burst;
        burst.mCell = victim.getCell()->getCell()->getId().serializeText();
        burst.mVictim = victim;
        burst.mReceiverCell = victim.getCell();
        burst.mBodyHeight = std::clamp(world->getHalfExtents(victim, true).z(), 2.f, 256.f);
        burst.mSource = { victim.getRefData().getPosition().asVec3() + osg::Vec3f(0, 0, burst.mBodyHeight),
            osg::Vec3f(0, 0, 1), 1.f, true };
        if (!std::isfinite(burst.mSource.mPosition.length2()))
            return;
        burst.mEmissionDuration = 10.f * Settings::ragdoll().mStatusDurationScale.get();
        const float radius = 768.f * std::max(1.f, Ragdoll::bloodLaunchScale(Settings::ragdoll().mBloodAmount.get()));
        auto surface = getBloodSurface(burst.mReceiverCell, burst.mSource.mPosition, radius);
        burst.mSinceSnapshot = surface ? 0.f : .5f;
        burst.mSurface = std::make_shared<BloodTrail::Surface>(std::move(surface));
        burst.mSnapshotCenter = burst.mSource.mPosition;
        burst.mDepositCell = std::make_shared<std::string>(burst.mCell);
        burst.mTrail = std::make_unique<BloodTrail>(*mSceneRoot,
            [surface = burst.mSurface](const auto& a, const auto& b, auto& p, auto& n) {
                return surface->mRaycast && surface->mRaycast(a, b, p, n);
            }, [this, cell = burst.mDepositCell](const auto& stamp) { mBloodDecals->add(*cell, stamp); },
            [surface = burst.mSurface](const auto& p, const auto& n, float r) {
                return surface->mProjector ? surface->mProjector(p, n, r) : std::vector<osg::Vec3f>{};
            }, [surface = burst.mSurface] { return bool(*surface); },mBloodProjectionBudget);
        if (mBloodBursts.size() >= 32)
        {
            const auto disposable = std::find_if(mBloodBursts.begin(), mBloodBursts.end(),
                [](const auto& candidate) { return candidate.mVictim.isEmpty(); });
            mBloodBursts.erase(disposable == mBloodBursts.end() ? mBloodBursts.begin() : disposable);
        }
        mBloodBursts.push_back(std::move(burst));
    }
    void RenderingManager::writeBloodState(ESM::ESMWriter& writer) const
    {
        writer.startRecord(ESM::REC_BRBL);
        mBloodDecals->getSaveRecord().save(writer);
        writer.endRecord(ESM::REC_BRBL);
    }
    void RenderingManager::readBloodState(ESM::ESMReader& reader)
    {
        ESM::BrutalBloodState state;
        state.load(reader);
        mBloodDecals->update(0, Settings::ragdoll().mBloodLifetime.get());
        mBloodDecals->loadSaveRecord(state);
    }

    void RenderingManager::notifyWorldSpaceChanged()
    {
        invalidateBloodSurfaces();
        mEffectManager->clear();
        mWater->clearRipples();
    }

    void RenderingManager::clear()
    {
        mBloodDecals->clear();
        mBloodBursts.clear();
        mSky->setMoonColour(false);

        notifyWorldSpaceChanged();
        if (mObjectPaging)
            mObjectPaging->clear();
    }

    MWRender::Animation* RenderingManager::getAnimation(const MWWorld::Ptr& ptr)
    {
        if (mPlayerAnimation.get() && ptr == mPlayerAnimation->getPtr())
            return mPlayerAnimation.get();

        return mObjects->getAnimation(ptr);
    }

    const MWRender::Animation* RenderingManager::getAnimation(const MWWorld::ConstPtr& ptr) const
    {
        if (mPlayerAnimation.get() && ptr == mPlayerAnimation->getPtr())
            return mPlayerAnimation.get();

        return mObjects->getAnimation(ptr);
    }

    PostProcessor* RenderingManager::getPostProcessor()
    {
        return mPostProcessor;
    }

    void RenderingManager::setupPlayer(const MWWorld::Ptr& player)
    {
        if (!mPlayerNode)
        {
            mPlayerNode = new SceneUtil::PositionAttitudeTransform;
            mPlayerNode->setNodeMask(Mask_Player);
            mPlayerNode->setName("Player Root");
            mSceneRoot->addChild(mPlayerNode);
        }

        mPlayerNode->setUserDataContainer(new osg::DefaultUserDataContainer);
        mPlayerNode->getUserDataContainer()->addUserObject(new PtrHolder(player));

        player.getRefData().setBaseNode(mPlayerNode);

        mWater->removeEmitter(player);
        mWater->addEmitter(player);
    }

    void RenderingManager::renderPlayer(const MWWorld::Ptr& player)
    {
        mPlayerAnimation = new NpcAnimation(player, player.getRefData().getBaseNode(), mResourceSystem, 0,
            NpcAnimation::VM_Normal, mFirstPersonFieldOfView);

        mCamera->setAnimation(mPlayerAnimation.get());
        mCamera->attachTo(player);
    }

    void RenderingManager::rebuildPtr(const MWWorld::Ptr& ptr)
    {
        NpcAnimation* anim = nullptr;
        if (ptr == mPlayerAnimation->getPtr())
            anim = mPlayerAnimation.get();
        else
            anim = dynamic_cast<NpcAnimation*>(mObjects->getAnimation(ptr));
        if (anim)
        {
            anim->rebuild();
            if (mCamera->getTrackingPtr() == ptr)
            {
                mCamera->attachTo(ptr);
                mCamera->setAnimation(anim);
            }
        }
    }

    void RenderingManager::addWaterRippleEmitter(const MWWorld::Ptr& ptr)
    {
        mWater->addEmitter(ptr);
    }

    void RenderingManager::removeWaterRippleEmitter(const MWWorld::Ptr& ptr)
    {
        mWater->removeEmitter(ptr);
    }

    void RenderingManager::emitWaterRipple(const osg::Vec3f& pos)
    {
        mWater->emitRipple(pos);
    }

    void RenderingManager::updateProjectionMatrix()
    {
        if (mNearClip < 0.0f)
            throw std::runtime_error("Near clip is less than zero");
        if (mViewDistance < mNearClip)
            throw std::runtime_error("Viewing distance is less than near clip");

        const int width = Settings::video().mResolutionX;
        const int height = Settings::video().mResolutionY;

        const double aspect = (height == 0) ? 1.0 : static_cast<double>(width) / height;
        const float fov = mFieldOfViewOverridden ? mFieldOfViewOverride : mFieldOfView;

        osg::Matrix unreversedProjectionMatrix = osg::Matrix::perspective(fov, aspect, mNearClip, mViewDistance);

        osg::Matrix projectionMatrix = SceneUtil::AutoDepth::isReversed()
            ? SceneUtil::getReversedZProjectionMatrixAsPerspective(fov, aspect, mNearClip, mViewDistance)
            : unreversedProjectionMatrix;

        if (width != 0 && height != 0)
        {
            double offsetX = (mProjectionOffset.x() / width) * 2.0;
            double offsetY = (mProjectionOffset.y() / height) * 2.0;

            const osg::Matrix translation = osg::Matrix::translate(offsetX, offsetY, 0.0);

            projectionMatrix.postMult(translation);
            unreversedProjectionMatrix.postMult(translation);
        }

        // We always set the cameras projection matrix to the un-reversed variant for correct frustum culling.
        mViewer->getCamera()->setProjectionMatrix(unreversedProjectionMatrix);

        mPerViewUniformStateUpdater->setProjectionMatrix(projectionMatrix);

        mSharedUniformStateUpdater->setNear(mNearClip);
        mSharedUniformStateUpdater->setFar(mViewDistance);

        if (Stereo::getStereo())
        {
            auto res = Stereo::Manager::instance().eyeResolution();
            setScreenRes(res.x(), res.y());
            Stereo::Manager::instance().setMasterProjectionMatrix(mPerViewUniformStateUpdater->getProjectionMatrix());
        }
        else
        {
            setScreenRes(width, height);
        }

        // Since our fog is not radial yet, we should take FOV in account, otherwise terrain near viewing distance may
        // disappear. Limit FOV here just for sure, otherwise viewing distance can be too high.
        float distanceMult = std::cos(osg::DegreesToRadians(std::min(fov, 140.f)) / 2.f);
        mTerrain->setViewDistance(mViewDistance * (distanceMult ? 1.f / distanceMult : 1.f));

        if (mPostProcessor)
        {
            mPostProcessor->getStateUpdater()->setProjectionMatrix(mPerViewUniformStateUpdater->getProjectionMatrix());
            mPostProcessor->getStateUpdater()->setFov(fov);
        }
    }

    void RenderingManager::setScreenRes(int width, int height)
    {
        mSharedUniformStateUpdater->setScreenRes(static_cast<float>(width), static_cast<float>(height));
    }

    void RenderingManager::updateTextureFiltering()
    {
        mViewer->stopThreading();

        mResourceSystem->getSceneManager()->setFilterSettings(Settings::general().mTextureMagFilter,
            Settings::general().mTextureMinFilter, Settings::general().mTextureMipmap,
            static_cast<float>(Settings::general().mAnisotropy));

        mTerrain->updateTextureFiltering();
        mWater->processChangedSettings({});

        mViewer->startThreading();
    }

    void RenderingManager::updateAmbient()
    {
        osg::Vec4f color = mAmbientColor;

        if (mNightEyeFactor > 0.f)
            color += osg::Vec4f(0.7f, 0.7f, 0.7f, 0.0f) * mNightEyeFactor;

        mPostProcessor->getStateUpdater()->setAmbientColor(color);
        mStateUpdater->setAmbientColor(color);
    }

    void RenderingManager::setFogColor(const osg::Vec4f& color)
    {
        mViewer->getCamera()->setClearColor(color);

        mStateUpdater->setFogColor(color);
    }

    RenderingManager::WorldspaceChunkMgr& RenderingManager::getWorldspaceChunkMgr(ESM::RefId worldspace)
    {
        auto existingChunkMgr = mWorldspaceChunks.find(worldspace);
        if (existingChunkMgr != mWorldspaceChunks.end())
            return existingChunkMgr->second;
        RenderingManager::WorldspaceChunkMgr newChunkMgr;

        const float lodFactor = Settings::terrain().mLodFactor;
        const bool groundcover = Settings::groundcover().mEnabled && worldspace == ESM::Cell::sDefaultWorldspaceId;
        const bool distantTerrain = Settings::terrain().mDistantTerrain;
        const double expiryDelay = Settings::cells().mCacheExpiryDelay;
        if (distantTerrain || groundcover)
        {
            const int compMapResolution = Settings::terrain().mCompositeMapResolution;
            const int compMapPower = Settings::terrain().mCompositeMapLevel;
            const float compMapLevel = static_cast<float>(std::pow(2, compMapPower));
            const int vertexLodMod = Settings::terrain().mVertexLodMod;
            const float maxCompGeometrySize = Settings::terrain().mMaxCompositeGeometrySize;
            const bool debugChunks = Settings::terrain().mDebugChunks;
            auto quadTreeWorld = std::make_unique<Terrain::QuadTreeWorld>(mSceneRoot, mRootNode, mResourceSystem,
                mTerrainStorage.get(), Mask_Terrain, Mask_PreCompile, Mask_Debug, compMapResolution, compMapLevel,
                lodFactor, vertexLodMod, maxCompGeometrySize, debugChunks, worldspace, expiryDelay);
            if (Settings::terrain().mObjectPaging)
            {
                newChunkMgr.mObjectPaging
                    = std::make_unique<ObjectPaging>(mResourceSystem->getSceneManager(), worldspace);
                quadTreeWorld->addChunkManager(newChunkMgr.mObjectPaging.get());
                mResourceSystem->addResourceManager(newChunkMgr.mObjectPaging.get());
            }
            if (groundcover)
            {
                const float groundcoverDistance = Settings::groundcover().mRenderingDistance;
                const float density = Settings::groundcover().mDensity;

                newChunkMgr.mGroundcover = std::make_unique<Groundcover>(
                    mResourceSystem->getSceneManager(), density, groundcoverDistance, mGroundCoverStore);
                quadTreeWorld->addChunkManager(newChunkMgr.mGroundcover.get());
                mResourceSystem->addResourceManager(newChunkMgr.mGroundcover.get());
            }
            newChunkMgr.mTerrain = std::move(quadTreeWorld);
        }
        else
            newChunkMgr.mTerrain = std::make_unique<Terrain::TerrainGrid>(mSceneRoot, mRootNode, mResourceSystem,
                mTerrainStorage.get(), Mask_Terrain, worldspace, expiryDelay, Mask_PreCompile, Mask_Debug);

        newChunkMgr.mTerrain->setTargetFrameRate(Settings::cells().mTargetFramerate);
        float distanceMult = std::cos(osg::DegreesToRadians(std::min(mFieldOfView, 140.f)) / 2.f);
        newChunkMgr.mTerrain->setViewDistance(mViewDistance * (distanceMult ? 1.f / distanceMult : 1.f));
        newChunkMgr.mTerrain->enableHeightCullCallback(Settings::terrain().mWaterCulling);

        return mWorldspaceChunks.emplace(worldspace, std::move(newChunkMgr)).first->second;
    }

    void RenderingManager::reportStats() const
    {
        osg::Stats* stats = mViewer->getViewerStats();
        unsigned int frameNumber = mViewer->getFrameStamp()->getFrameNumber();
        if (stats->collectStats("resource"))
        {
            mTerrain->reportStats(frameNumber, stats);
        }
    }

    void RenderingManager::processChangedSettings(const Settings::CategorySettingVector& changed)
    {
        // Only perform a projection matrix update once if a relevant setting is changed.
        bool updateProjection = false;

        for (Settings::CategorySettingVector::const_iterator it = changed.begin(); it != changed.end(); ++it)
        {
            if (it->first == "Camera" && it->second == "field of view")
            {
                mFieldOfView = Settings::camera().mFieldOfView;
                updateProjection = true;
            }
            else if (it->first == "Video" && (it->second == "resolution x" || it->second == "resolution y"))
            {
                updateProjection = true;
            }
            else if (it->first == "Camera" && it->second == "viewing distance")
            {
                setViewDistance(Settings::camera().mViewingDistance);
            }
            else if (it->first == "General"
                && (it->second == "texture filter" || it->second == "texture mipmap" || it->second == "anisotropy"))
            {
                updateTextureFiltering();
            }
            else if (it->first == "Water")
            {
                mWater->processChangedSettings(changed);
            }
            else if (it->first == "Shaders" && it->second == "minimum interior brightness")
            {
                if (MWMechanics::getPlayer().isInCell())
                    configureAmbient(*MWMechanics::getPlayer().getCell()->getCell());
            }
            else if (it->first == "Shaders"
                && (it->second == "force per pixel lighting" || it->second == "classic falloff"
                    || it->second == "clamp lighting"))
            {
                mViewer->stopThreading();

                auto defines = mResourceSystem->getSceneManager()->getShaderManager().getGlobalDefines();
                defines["forcePPL"] = Settings::shaders().mForcePerPixelLighting ? "1" : "0";
                defines["classicFalloff"] = Settings::shaders().mClassicFalloff ? "1" : "0";
                defines["clamp"] = Settings::shaders().mClampLighting ? "1" : "0";
                mResourceSystem->getSceneManager()->getShaderManager().setGlobalDefines(defines);

                if (MWMechanics::getPlayer().isInCell() && it->second == "classic falloff")
                    configureAmbient(*MWMechanics::getPlayer().getCell()->getCell());

                mViewer->startThreading();
            }
            else if (it->first == "Shaders"
                && (it->second == "light bounds multiplier" || it->second == "maximum light distance"
                    || it->second == "light fade start" || it->second == "max lights"))
            {
                auto* lightManager = getLightRoot();

                lightManager->processChangedSettings(Settings::shaders().mLightBoundsMultiplier,
                    Settings::shaders().mMaximumLightDistance, Settings::shaders().mLightFadeStart);

                if (it->second == "max lights")
                {
                    mViewer->stopThreading();

                    lightManager->updateMaxLights(Settings::shaders().mMaxLights);

                    auto defines = mResourceSystem->getSceneManager()->getShaderManager().getGlobalDefines();
                    for (const auto& [name, key] : lightManager->getLightDefines())
                        defines[name] = key;
                    mResourceSystem->getSceneManager()->getShaderManager().setGlobalDefines(defines);

                    mStateUpdater->reset();

                    mViewer->startThreading();
                }
            }
            else if (it->first == "Post Processing" && it->second == "enabled")
            {
                if (Settings::postProcessing().mEnabled)
                    mPostProcessor->enable();
                else
                {
                    mPostProcessor->disable();
                    if (auto* hud = MWBase::Environment::get().getWindowManager()->getPostProcessorHud())
                        hud->setVisible(false);
                }
            }
        }

        if (updateProjection)
        {
            updateProjectionMatrix();
        }
    }

    void RenderingManager::setViewDistance(float distance, bool delay)
    {
        mViewDistance = distance;

        if (delay)
        {
            mUpdateProjectionMatrix = true;
            return;
        }

        updateProjectionMatrix();
    }

    float RenderingManager::getTerrainHeightAt(const osg::Vec3f& pos, ESM::RefId worldspace)
    {
        return getWorldspaceChunkMgr(worldspace).mTerrain->getHeightAt(pos);
    }

    void RenderingManager::overrideFieldOfView(float val)
    {
        if (mFieldOfViewOverridden != true || mFieldOfViewOverride != val)
        {
            mFieldOfViewOverridden = true;
            mFieldOfViewOverride = val;
            updateProjectionMatrix();
        }
    }

    void RenderingManager::setFieldOfView(float val)
    {
        mFieldOfView = val;
        mUpdateProjectionMatrix = true;
    }

    float RenderingManager::getFieldOfView() const
    {
        return mFieldOfViewOverridden ? mFieldOfViewOverridden : mFieldOfView;
    }

    osg::Vec3f RenderingManager::getHalfExtents(const MWWorld::ConstPtr& object) const
    {
        osg::Vec3f halfExtents(0, 0, 0);
        VFS::Path::Normalized modelName(object.getClass().getCorrectedModel(object));
        if (modelName.empty())
            return halfExtents;

        osg::ref_ptr<const osg::Node> node = mResourceSystem->getSceneManager()->getTemplate(modelName);
        osg::ComputeBoundsVisitor computeBoundsVisitor;
        computeBoundsVisitor.setTraversalMask(~(MWRender::Mask_ParticleSystem | MWRender::Mask_Effect));
        const_cast<osg::Node*>(node.get())->accept(computeBoundsVisitor);
        osg::BoundingBox bounds = computeBoundsVisitor.getBoundingBox();

        if (bounds.valid())
        {
            halfExtents[0] = std::abs(bounds.xMax() - bounds.xMin()) / 2.f;
            halfExtents[1] = std::abs(bounds.yMax() - bounds.yMin()) / 2.f;
            halfExtents[2] = std::abs(bounds.zMax() - bounds.zMin()) / 2.f;
        }

        return halfExtents;
    }

    osg::BoundingBox RenderingManager::getCullSafeBoundingBox(const MWWorld::Ptr& ptr) const
    {
        if (ptr.isEmpty())
            return {};

        osg::ref_ptr<SceneUtil::PositionAttitudeTransform> rootNode = ptr.getRefData().getBaseNode();

        // Recalculate bounds on the ptr's template when the object is not loaded or is loaded but paged
        MWWorld::Scene* worldScene = MWBase::Environment::get().getWorldScene();
        if (!rootNode || worldScene->isPagedRef(ptr))
        {
            const VFS::Path::Normalized model(ptr.getClass().getCorrectedModel(ptr));

            if (model.empty())
                return {};

            rootNode = new SceneUtil::PositionAttitudeTransform;
            // Hack even used by osg internally, osg's NodeVisitor won't accept const qualified nodes
            rootNode->addChild(const_cast<osg::Node*>(mResourceSystem->getSceneManager()->getTemplate(model).get()));

            const float refScale = ptr.getCellRef().getScale();
            rootNode->setScale({ refScale, refScale, refScale });
            const auto& rotation = ptr.getCellRef().getPosition().rot;
            if (!ptr.getClass().isActor())
                rootNode->setAttitude(osg::Quat(rotation[0], osg::Vec3(-1, 0, 0))
                    * osg::Quat(rotation[1], osg::Vec3(0, -1, 0)) * osg::Quat(rotation[2], osg::Vec3(0, 0, -1)));
            rootNode->setPosition(ptr.getCellRef().getPosition().asVec3());

            osg::ref_ptr<Animation> animation = nullptr;

            if (ptr.getClass().isNpc())
            {
                rootNode->setNodeMask(Mask_Actor);
                animation = new NpcAnimation(ptr, osg::ref_ptr<osg::Group>(rootNode), mResourceSystem);
            }
        }

        SceneUtil::CullSafeBoundsVisitor computeBounds;
        computeBounds.setTraversalMask(~(MWRender::Mask_ParticleSystem | MWRender::Mask_Effect));
        rootNode->accept(computeBounds);

        return computeBounds.mBoundingBox;
    }

    void RenderingManager::resetFieldOfView()
    {
        if (mFieldOfViewOverridden == true)
        {
            mFieldOfViewOverridden = false;

            updateProjectionMatrix();
        }
    }
    void RenderingManager::exportSceneGraph(
        const MWWorld::Ptr& ptr, const std::filesystem::path& filename, const std::string& format)
    {
        osg::Node* node = mViewer->getSceneData();
        if (!ptr.isEmpty())
            node = ptr.getRefData().getBaseNode();

        SceneUtil::writeScene(node, filename, format);
    }

    LandManager* RenderingManager::getLandManager() const
    {
        return mTerrainStorage->getLandManager();
    }

    void RenderingManager::updateActorPath(const MWWorld::ConstPtr& actor, const std::deque<osg::Vec3f>& path,
        const DetourNavigator::AgentBounds& agentBounds, const osg::Vec3f& start, const osg::Vec3f& end) const
    {
        mActorsPaths->update(actor, path, agentBounds, start, end, mNavigator.getSettings());
    }

    void RenderingManager::removeActorPath(const MWWorld::ConstPtr& actor) const
    {
        mActorsPaths->remove(actor);
    }

    void RenderingManager::setNavMeshNumber(const std::size_t value)
    {
        mNavMeshNumber = value;
    }

    void RenderingManager::updateNavMesh()
    {
        if (!mNavMesh->isEnabled())
            return;

        const auto navMeshes = mNavigator.getNavMeshes();

        auto it = navMeshes.begin();
        for (std::size_t i = 0; it != navMeshes.end() && i < mNavMeshNumber; ++i)
            ++it;
        if (it == navMeshes.end())
        {
            mNavMesh->reset();
        }
        else
        {
            try
            {
                mNavMesh->update(it->second, mNavMeshNumber, mNavigator.getSettings());
            }
            catch (const std::exception& e)
            {
                Log(Debug::Error) << "NavMesh render update exception: " << e.what();
            }
        }
    }

    void RenderingManager::updateRecastMesh()
    {
        if (!mRecastMesh->isEnabled())
            return;

        mRecastMesh->update(mNavigator.getRecastMeshTiles(), mNavigator.getSettings());
    }

    void RenderingManager::setActiveGrid(const osg::Vec4i& grid)
    {
        mTerrain->setActiveGrid(grid);
    }
    bool RenderingManager::pagingEnableObject(int type, const MWWorld::ConstPtr& ptr, bool enabled)
    {
        if (!ptr.isInCell() || !ptr.getCell()->isExterior() || !mObjectPaging)
            return false;
        if (mObjectPaging->enableObject(type, ptr.getCellRef().getRefNum(), ptr.getCellRef().getPosition().asVec3(),
                osg::Vec2i(ptr.getCell()->getCell()->getGridX(), ptr.getCell()->getCell()->getGridY()), enabled))
        {
            mTerrain->rebuildViews();
            return true;
        }
        return false;
    }
    void RenderingManager::pagingBlacklistObject(int type, const MWWorld::ConstPtr& ptr)
    {
        if (!ptr.isInCell() || !ptr.getCell()->isExterior() || !mObjectPaging)
            return;
        ESM::RefNum refnum = ptr.getCellRef().getRefNum();
        if (!refnum.hasContentFile())
            return;
        if (mObjectPaging->blacklistObject(type, refnum, ptr.getCellRef().getPosition().asVec3(),
                osg::Vec2i(ptr.getCell()->getCell()->getGridX(), ptr.getCell()->getCell()->getGridY())))
            mTerrain->rebuildViews();
    }
    bool RenderingManager::pagingUnlockCache()
    {
        if (mObjectPaging && mObjectPaging->unlockCache())
        {
            mTerrain->rebuildViews();
            return true;
        }
        return false;
    }
    void RenderingManager::getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out)
    {
        if (mObjectPaging)
            mObjectPaging->getPagedRefnums(activeGrid, out);
    }

    void RenderingManager::setNavMeshMode(Settings::NavMeshRenderMode value)
    {
        mNavMesh->setMode(value);
    }
}
