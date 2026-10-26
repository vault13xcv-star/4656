// Optional Linux/EGL integration test for the real native shader templates,
// LightManager callbacks and cascaded depth-comparison textures.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "bloodterrainfixture.hpp"
#include <apps/openmw/mwrender/bloodterrain.hpp>
#include <osgDB/WriteFile>
#include <cstdlib>

#include <apps/openmw/mwrender/brutalblood.hpp>
#include <apps/openmw/mwrender/brutalbloodlighting.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/terrain/compositemaprenderer.hpp>
#include <components/terrain/terraindrawable.hpp>

#include <osg/Geode>
#include <osg/LightModel>
#include <osg/MatrixTransform>
#include <osg/Texture2D>
#include <osgViewer/GraphicsWindow>
#include <osgViewer/Viewer>
#include <osgDB/ReadFile>

#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    constexpr int Width = 320, Height = 240;
    void require(bool condition, const std::string& message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    struct SurfacelessGL
    {
        EGLDisplay mDisplay = EGL_NO_DISPLAY;
        EGLSurface mSurface = EGL_NO_SURFACE;
        EGLContext mContext = EGL_NO_CONTEXT;
        SurfacelessGL()
        {
            const auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
            require(platform, "EGL surfaceless display unavailable");
            mDisplay = platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
            EGLint major = 0, minor = 0;
            require(eglInitialize(mDisplay, &major, &minor) && eglBindAPI(EGL_OPENGL_API), "EGL initialization failed");
            const EGLint attributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
                EGL_STENCIL_SIZE, 8, EGL_NONE };
            EGLConfig config;
            EGLint count = 0;
            require(eglChooseConfig(mDisplay, attributes, &config, 1, &count) && count, "EGL configuration missing");
            const EGLint size[] = { EGL_WIDTH, Width, EGL_HEIGHT, Height, EGL_NONE };
            mSurface = eglCreatePbufferSurface(mDisplay, config, size);
            mContext = eglCreateContext(mDisplay, config, EGL_NO_CONTEXT, nullptr);
            require(eglMakeCurrent(mDisplay, mSurface, mSurface, mContext), "EGL context missing");
        }
        ~SurfacelessGL()
        {
            eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (mContext != EGL_NO_CONTEXT) eglDestroyContext(mDisplay, mContext);
            if (mSurface != EGL_NO_SURFACE) eglDestroySurface(mDisplay, mSurface);
            if (mDisplay != EGL_NO_DISPLAY) eglTerminate(mDisplay);
        }
    };

    osg::ref_ptr<osg::Texture2D> shadowDepth(float value)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(1, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT);
        *reinterpret_cast<float*>(image->data()) = value;
        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
        texture->setInternalFormat(GL_DEPTH_COMPONENT24);
        texture->setResizeNonPowerOfTwoHint(false);
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        texture->setShadowComparison(true);
        texture->setShadowCompareFunc(osg::Texture::LEQUAL);
        texture->setShadowTextureMode(osg::Texture::LUMINANCE);
        return texture;
    }

    osg::Matrixf constantShadowCoordinates(float u, float v, float depth)
    {
        osg::Matrixf result;
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column) result(row, column) = 0;
        result(3, 0) = u; result(3, 1) = v; result(3, 2) = depth; result(3, 3) = 1;
        return result;
    }

    osg::Program* bloodProgram(osg::Group& root)
    {
        for (unsigned i = 0; i < root.getNumChildren(); ++i)
        {
            auto* child = root.getChild(i);
            if (child->getName() == "Brutal Morrowind blood stains")
                return dynamic_cast<osg::Program*>(child->getStateSet()->getAttribute(osg::StateAttribute::PROGRAM));
            if (auto* group = child->asGroup())
                if (auto* program = bloodProgram(*group)) return program;
        }
        return nullptr;
    }

    void drawNativeOverlapBlood(osgViewer::Viewer& viewer, bool reversed, bool perimeter)
    {
        osg::ref_ptr<SceneUtil::LightManager> scene = new SceneUtil::LightManager(SceneUtil::LightSettings{});
        MWRender::installNativeBloodLighting(*scene, false, true, 0, false);
        auto* state = scene->getOrCreateStateSet();
        osg::ref_ptr<osg::LightModel> ambient = new osg::LightModel;
        ambient->setAmbientIntensity({ 1, 1, 1, 1 });
        state->setAttributeAndModes(ambient, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        auto* camera = viewer.getCamera();
        camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
        camera->setViewMatrixAsLookAt({ 0, 0, 500 }, { 0, 0, 0 }, { 0, 1, 0 });
        camera->setProjectionMatrixAsOrtho(-128, 128, -96, 96, 1, 1000);
        camera->setClearColor({ .3f, .3f, .3f, 1 });
        auto projection = camera->getProjectionMatrix();
        if (reversed) for (int row = 0; row < 4; ++row) projection(row, 2) = -projection(row, 2);
        state->addUniform(new osg::Uniform("projectionMatrix", osg::Matrixf(projection)));
        const auto surface = MWRender::BloodTrail::visualSurface({
            { -512, -512, 0 }, { 512, -512, 0 }, { 512, 512, 0 },
            { -512, -512, 0 }, { 512, 512, 0 }, { -512, 512, 0 } });
        MWRender::BloodDecals blood(*scene);
        std::vector<osg::ref_ptr<osg::Image>> floor;
        for (const auto* name : { "floor06.png", "floor07.png" })
            floor.push_back(osgDB::readRefImageFile((std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR)
                / "files/data/textures/brutal/blood" / name).string()));
        blood.setTextures(floor, {}); blood.setCellActive("overlap", true);
        const auto add = [&](osg::Vec3f position, float radius, bool pool, unsigned seed) {
            MWRender::BloodStamp stamp;
            stamp.mCenter = position; stamp.mNormal = { 0, 0, 1 }; stamp.mRadius = radius;
            stamp.mPool = pool; stamp.mVisualSeed = seed;
            stamp.mTriangles = surface.mProjector(position, stamp.mNormal, radius);
            blood.add("overlap", stamp);
        };
        if (perimeter)
        {
            add({ -40, -10, 0 }, 100, true, 8);
            add({ 40, -10, 0 }, 100, true, 12);
        }
        else
        {
            add({}, 24, false, 8); add({}, 24, false, 12);
            add({}, 80, true, 16);
        }
        const auto sample = [&] {
            blood.update(.11f, 1200);
            viewer.setSceneData(scene); viewer.frame(); viewer.frame(); glFinish();
            std::vector<unsigned char> pixels(Width * Height * 3);
            glReadPixels(0, 0, Width, Height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
            require(glGetError() == GL_NO_ERROR, "Overlap framebuffer read failed");
            return pixels;
        };
        const auto before = sample();
        if (perimeter) add({ 0, 82, 0 }, 14, false, 20);
        const auto after = perimeter ? sample() : before;
        unsigned emptyCore = 0, changed = 0;
        for (int y = 0; y < Height; ++y)
            for (int x = 0; x < Width; ++x)
            {
                const float px = (x + .5f) / Width * 256 - 128;
                const float py = (y + .5f) / Height * 192 - 96;
                const auto i = std::size_t(y * Width + x) * 3;
                const bool red = after[i] > after[i + 1] + 6 && after[i] > after[i + 2] + 6;
                if (px * px + py * py < 25 * 25 && !red) ++emptyCore;
                if (std::abs(px) < 15 && std::abs(py - 82) < 15 && red
                    && (std::abs(int(after[i]) - int(before[i]))
                        + std::abs(int(after[i + 1]) - int(before[i + 1])) > 5)) ++changed;
            }
        if (const char* directory = std::getenv("BRUTAL_TEST_OUTPUT"))
        {
            osg::ref_ptr<osg::Image> screenshot = new osg::Image;
            screenshot->readPixels(0, 0, Width, Height, GL_RGB, GL_UNSIGNED_BYTE);
            osgDB::writeImageFile(*screenshot, (std::filesystem::path(directory)
                / (std::string(perimeter ? "perimeter-" : "overlap-") + (reversed ? "reversed" : "normal") + ".png")).string());
        }
        std::cout << "Overlap dry core pixels=" << emptyCore << " perimeter changed pixels=" << changed << '\n';
        require(perimeter ? changed > 20 : emptyCore == 0,
            perimeter ? "Transparent pool rectangles suppressed a new splash on dry ground"
                      : "Sparse splash rectangles cut square dry holes in a larger pool");
        viewer.setSceneData(nullptr);
    }

    unsigned drawNativeExteriorBlood(osgViewer::Viewer& viewer, bool reversed, bool distant,
        float amount = 1, int corpseCount = 1, unsigned frames = 120, bool rough = false, bool onlySplashes = false)
    {
        BloodTest::ExteriorBloodScene terrain(-4, -3, .2f, .1f, distant, rough);
        const auto center = terrain.mStorage.center();
        osg::ref_ptr<SceneUtil::LightManager> scene = new SceneUtil::LightManager(SceneUtil::LightSettings{});
        scene->setNodeMask(MWRender::Mask_Scene);
        scene->addChild(terrain.mRoot);
        MWRender::installNativeBloodLighting(*scene, false, true, 0, false);
        auto* state = scene->getOrCreateStateSet();
        osg::ref_ptr<osg::LightModel> ambient = new osg::LightModel;
        ambient->setAmbientIntensity({ 1, 1, 1, 1 });
        state->setAttributeAndModes(ambient, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        state->setAttributeAndModes(new SceneUtil::AutoDepth, osg::StateAttribute::ON);
        osg::ref_ptr<osg::Group> objects = new osg::Group;
        osg::ref_ptr<osg::Geode> decoration = new osg::Geode;
        osg::ref_ptr<osg::Geometry> triangle = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array;
        positions->push_back({ -20, -20, 0 }); positions->push_back({ 20, -20, 0 }); positions->push_back({ 0, 20, 40 });
        triangle->setVertexArray(positions); triangle->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
        decoration->addDrawable(triangle);
        for (unsigned i = 0; i < 18000; ++i)
        {
            const double side = i % 2 ? 1 : -1;
            osg::ref_ptr<osg::MatrixTransform> object = new osg::MatrixTransform;
            object->setNodeMask(MWRender::Mask_Object);
            object->setMatrix(osg::Matrixd::translate(osg::Vec3d(center)
                + osg::Vec3d(side * (3000 + i % 100), side * (3000 + i / 100), 0)));
            object->addChild(decoration); objects->addChild(object);
        }
        terrain.mRoot->insertChild(0, objects);
        auto receivers = MWRender::collectBloodTerrainTriangles(terrain.mStorage,
            ESM::Cell::sDefaultWorldspaceId, center + osg::Vec3f(0, 0, 80), 900);
        require(!receivers.empty(), "Direct LAND blood receiver was empty");
        auto objectReceivers = MWRender::collectBloodSurfaceTriangles(*terrain.mRoot,
            center + osg::Vec3f(0, 0, 80), 900, {}, nullptr, false);
        receivers.insert(receivers.end(), objectReceivers.begin(), objectReceivers.end());
        const auto surface = MWRender::BloodTrail::visualSurface(receivers);
        require(bool(surface), "Native exterior terrain disappeared from the blood receiver query behind the object tree");

        // The fixture uses real native chunks, their original vertex/index
        // buffers and draw/cull implementation. Only their material is a
        // neutral grey so blood pixels can be measured unambiguously.
        osg::ref_ptr<osg::StateSet> pass = new osg::StateSet;
        pass->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
        pass->setAttributeAndModes(new SceneUtil::AutoDepth, osg::StateAttribute::ON);
        osg::ref_ptr<osg::Program> groundProgram = new osg::Program;
        groundProgram->addShader(new osg::Shader(osg::Shader::VERTEX,
            "#version 120\nuniform mat4 projectionMatrix;void main(){gl_Position=projectionMatrix*gl_ModelViewMatrix*gl_Vertex;}"));
        groundProgram->addShader(new osg::Shader(osg::Shader::FRAGMENT,
            "#version 120\nvoid main(){gl_FragColor=vec4(.3,.3,.3,1.0);}"));
        pass->setAttributeAndModes(groundProgram, osg::StateAttribute::ON);
        struct GroundPass : osg::NodeVisitor
        {
            osg::StateSet* mPass;
            osg::Vec3f mEye;
            GroundPass(osg::StateSet* pass, const osg::Vec3f& eye)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN), mPass(pass), mEye(eye)
            {
                setVisitorType(INTERSECTION_VISITOR);
                setTraversalMask(MWRender::Mask_Terrain | MWRender::Mask_Scene);
                setUserValue("brutalBloodReceiverRadius", 900.f);
            }
            osg::Vec3 getEyePoint() const override { return mEye; }
            void apply(osg::Drawable& drawable) override
            {
                if (auto* land = dynamic_cast<Terrain::TerrainDrawable*>(&drawable))
                {
                    land->setPasses({ mPass });
                    land->createClusterCullingCallback();
                }
            }
        } groundPass(pass, center);
        terrain.mRoot->accept(groundPass);

        MWRender::BloodDecals blood(*scene);
        std::vector<osg::ref_ptr<osg::Image>> floor;
        for (const char* name : { "floor06.png", "floor07.png" })
        {
            auto image = osgDB::readRefImageFile((std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR)
                / "files/data/textures/brutal/blood" / name).string());
            require(bool(image), "Original floor blood artwork failed to load in the exterior rendering test");
            floor.push_back(image);
        }
        blood.setTextures(floor, {});
        blood.setCellActive("Esm3ExteriorCell:-3:-2", true);
        std::vector<std::unique_ptr<MWRender::BloodTrail>> fountains;
        for (int corpse = 0; corpse < corpseCount; ++corpse)
            fountains.push_back(std::make_unique<MWRender::BloodTrail>(*scene, surface.mRaycast,
                [&](const auto& stamp) {
                    if (!onlySplashes || !stamp.mPool) blood.add("Esm3ExteriorCell:-3:-2", stamp);
                }, surface.mProjector, true));
        for (unsigned frame = 0; frame < frames; ++frame)
        {
            blood.update(1.f / 60, 1200);
            for (int corpse = 0; corpse < corpseCount; ++corpse)
            {
                const float x = center.x() + (corpse - (corpseCount - 1) * .5f) * 300;
                const float age = frame / 60.f;
                const float height = corpseCount > 1 ? std::max(28.f, 110.f - age * 180) : 28.f;
                const auto position = osg::Vec3f(x, center.y(), terrain.mStorage.height(x, center.y()) + height);
                std::vector<MWRender::BloodTrail::Source> wounds{ { position,
                    corpseCount > 1 ? osg::Vec3f(.15f, .2f, 1) : osg::Vec3f(0, 0, -1),
                    1.f - .8f * std::min(1.f, age / 10), true } };
                if (corpseCount > 1)
                {
                    wounds.push_back({ position + osg::Vec3f(-12, -8, -5), { -1, .3f, .4f }, .8f, false, true });
                    wounds.push_back({ position + osg::Vec3f(12, 8, -5), { 1, -.2f, .2f }, .8f, false, true });
                }
                fountains[corpse]->update(1.f / 60, wounds, amount, age);
            }
        }
        for (unsigned frame = 0; frame < 210; ++frame)
        {
            blood.update(1.f / 60, 1200);
            for (auto& fountain : fountains) fountain->update(1.f / 60, {}, amount, frames / 60.f + frame / 60.f);
        }
        for (const auto& fountain : fountains)
            require(fountain->particleCount() == 0, "Exterior decal visibility test still contained flying sprites");
        require(blood.size() > 0, "Native terrain ray hits did not create any saved blood stains");
        auto* camera = viewer.getCamera();
        camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
        if (corpseCount == 1)
        {
            camera->setViewMatrixAsLookAt(osg::Vec3d(center) + osg::Vec3d(0, -180, 130), osg::Vec3d(center), { 0, 0, 1 });
            camera->setProjectionMatrixAsPerspective(60, double(Width) / Height, 1, 81920);
        }
        else
        {
            camera->setViewMatrixAsLookAt(osg::Vec3d(center) + osg::Vec3d(0, 0, 1000), osg::Vec3d(center), { 0, 1, 0 });
            camera->setProjectionMatrixAsOrtho(-600, 600, -450, 450, 1, 81920);
        }
        auto projection = camera->getProjectionMatrix();
        if (reversed) for (int row = 0; row < 4; ++row) projection(row, 2) = -projection(row, 2);
        state->addUniform(new osg::Uniform("projectionMatrix", osg::Matrixf(projection)));
        viewer.setSceneData(scene);
        viewer.frame(); viewer.frame(); glFinish();
        std::vector<unsigned char> pixels(Width * Height * 3);
        glReadPixels(0, 0, Width, Height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
        require(glGetError() == GL_NO_ERROR, "Native exterior blood framebuffer read failed");
        unsigned red = 0;
        for (std::size_t i = 0; i < pixels.size(); i += 3)
            red += pixels[i] > pixels[i + 1] + 6 && pixels[i] > pixels[i + 2] + 6;
        require(red > 20, "Exterior blood existed in CPU memory but no original floor decal pixels reached the native terrain framebuffer");
        std::cout << "PASS native " << (distant ? "quadtree" : "default grid") << " terrain + 18000 objects -> ray -> fountain -> "
                  << blood.size() << " decals -> " << red << " blood pixels; amount=" << amount << " corpses=" << corpseCount
                  << " rough=" << rough << " splashes-only=" << onlySplashes << '\n';
        if (const char* directory = std::getenv("BRUTAL_TEST_OUTPUT"))
        {
            osg::ref_ptr<osg::Image> screenshot = new osg::Image;
            screenshot->readPixels(0, 0, Width, Height, GL_RGB, GL_UNSIGNED_BYTE);
            osgDB::writeImageFile(*screenshot, (std::filesystem::path(directory)
                / (std::string(distant ? "quadtree-" : "grid-") + (rough ? "rough-" : "")
                    + (onlySplashes ? "splashes-" : "") + std::to_string(int(amount * 100)) + ".png")).string());
        }
        viewer.setSceneData(nullptr);
        return red;
    }

    void drawNativeLayerCounter(osgViewer::Viewer& viewer, bool reversed)
    {
        osg::ref_ptr<SceneUtil::LightManager> scene = new SceneUtil::LightManager(SceneUtil::LightSettings{});
        MWRender::installNativeBloodLighting(*scene, false, true, 0, false);
        auto* camera = viewer.getCamera();
        camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
        camera->setViewMatrixAsLookAt({ 0, 0, 500 }, {}, { 0, 1, 0 });
        camera->setProjectionMatrixAsOrtho(-128, 128, -96, 96, 1, 1000);
        camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        camera->setClearStencil(0xa0);
        auto projection = camera->getProjectionMatrix();
        if (reversed) for (int row = 0; row < 4; ++row) projection(row, 2) = -projection(row, 2);
        scene->getOrCreateStateSet()->addUniform(new osg::Uniform("projectionMatrix", osg::Matrixf(projection)));
        osg::ref_ptr<osg::Image> opaque = new osg::Image;
        opaque->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        opaque->setColor({ 1, 0, 0, 1 }, 0, 0);
        MWRender::BloodDecals blood(*scene);
        blood.setTextures({ opaque }, {}); blood.setCellActive("counter", true);
        MWRender::BloodStamp stamp;
        stamp.mCenter = {}; stamp.mNormal = { 0, 0, 1 }; stamp.mRadius = 60;
        stamp.mTriangles = { { -60, -60, .03f }, { 60, -60, .03f }, { 60, 60, .03f },
            { -60, -60, .03f }, { 60, 60, .03f }, { -60, 60, .03f } };
        for (unsigned i = 1; i <= 300; ++i) { stamp.mVisualSeed = i; blood.add("counter", stamp); }
        require(blood.size() == 300, "Layer-counter regression did not retain the hundreds of source quads");
        blood.update(.11f, 1200);
        struct Probe : osg::Drawable::DrawCallback
        {
            mutable unsigned mMaximum = 0, mCount = 0;
            void drawImplementation(osg::RenderInfo&, const osg::Drawable*) const override
            {
                std::vector<unsigned char> stencil(Width * Height);
                glReadPixels(0, 0, Width, Height, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, stencil.data());
                for (const auto value : stencil)
                {
                    require((value & 0xfc) == 0xa0, "Blood changed stencil bits reserved for other scene effects");
                    mMaximum = std::max(mMaximum, unsigned(value & 3));
                }
                ++mCount;
            }
        };
        osg::ref_ptr<Probe> probe = new Probe;
        osg::ref_ptr<osg::Geode> node = new osg::Geode;
        node->getOrCreateStateSet()->setRenderBinDetails(9, "BrutalBloodBin");
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> points = new osg::Vec3Array;
        points->push_back({ -1, -1, 0 }); points->push_back({ 1, -1, 0 }); points->push_back({ 0, 1, 0 });
        geometry->setVertexArray(points); geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
        geometry->getOrCreateStateSet()->setRenderBinDetails(2, "RenderBin");
        geometry->setUseDisplayList(false); geometry->setDrawCallback(probe);
        node->addDrawable(geometry); scene->addChild(node);
        viewer.setSceneData(scene); viewer.frame(); viewer.frame(); glFinish();
        require(probe->mCount > 0 && probe->mMaximum == 2, "Blood's visible fragment counter did not enforce exactly two layers");
        std::vector<unsigned char> stencil(Width * Height);
        glReadPixels(0, 0, Width, Height, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, stencil.data());
        require(std::all_of(stencil.begin(), stencil.end(), [](auto value) { return value == 0xa0; }),
            "Blood left its counter bits in the scene after drawing");
        require(glGetError() == GL_NO_ERROR, "Blood layer counter raised an OpenGL error");
        std::cout << "PASS 300 overlapping quads -> maximum two visible layers; scene stencil preserved\n";
        viewer.setSceneData(nullptr);
    }
}

int main(int argc, char** argv)
{
    try
    {
        const bool reversed = argc > 1 && std::string(argv[1]) == "reversed";
        SurfacelessGL context;
        SceneUtil::AutoDepth::setReversed(reversed);
        osgViewer::Viewer viewer;
        viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
        viewer.setUpViewerAsEmbeddedInWindow(0, 0, Width, Height);
        auto* camera = viewer.getCamera();
        camera->setClearColor({ 0, 0, 0, 1 });
        camera->setClearDepth(reversed ? 0 : 1);
        camera->setProjectionMatrixAsOrtho(-80, 80, -20, 100, 1, 500);
        camera->setViewMatrixAsLookAt({ 0, -190, 40 }, { 0, 0, 40 }, { 0, 0, 1 });
        camera->setDrawBuffer(GL_FRONT); camera->setReadBuffer(GL_FRONT);

        Shader::ShaderManager shaders;
        shaders.setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
        auto defines = Shader::getDefaultDefines();
        for (const auto& [key, value] : SceneUtil::ShadowManager::getShadowsDisabledDefines()) defines[key] = value;
        defines["shadows_enabled"] = "1";
        defines["shadow_texture_unit_list"] = "0,1";
        defines["perspectiveShadowMaps"] = "1";
        defines["disableNormalOffsetShadows"] = "1";
        defines["limitShadowMapDistance"] = "1";
        shaders.setGlobalDefines(defines);
        const auto shadowVertex = shaders.getShader("compatibility/brutalblood_shadow.vert");
        const auto shadowFragment = shaders.getShader("compatibility/brutalblood_shadow.frag");
        require(shadowVertex.valid() && shadowFragment.valid(), "Native blood shadow wrappers failed preprocessing");

        SceneUtil::LightSettings lightSettings;
        lightSettings.mMaximumLightDistance = 0;
        osg::ref_ptr<SceneUtil::LightManager> root = new SceneUtil::LightManager(lightSettings);
        root->setStartLight(1);
        MWRender::installNativeBloodLighting(*root, false, true, 0, false, shadowVertex, shadowFragment);
        auto* state = root->getOrCreateStateSet();
        auto projection = camera->getProjectionMatrix();
        if (reversed)
            for (int row = 0; row < 4; ++row) projection(row, 2) = -projection(row, 2);
        state->addUniform(new osg::Uniform("projectionMatrix", osg::Matrixf(projection)));
        state->setAttributeAndModes(new SceneUtil::AutoDepth, osg::StateAttribute::ON);
        osg::ref_ptr<osg::LightModel> ambient = new osg::LightModel;
        ambient->setAmbientIntensity({ 0, 0, 0, 1 });
        state->setAttributeAndModes(ambient, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        const auto firstDepth = shadowDepth(.75f), secondDepth = shadowDepth(.25f);
        state->setTextureAttributeAndModes(4, firstDepth, osg::StateAttribute::ON);
        state->setTextureAttributeAndModes(5, secondDepth, osg::StateAttribute::ON);
        state->addUniform(new osg::Uniform("shadowTexture0", 4));
        state->addUniform(new osg::Uniform("shadowTexture1", 5));
        state->addUniform(new osg::Uniform("shadowSpaceMatrix0", constantShadowCoordinates(.5f, .5f, .5f)));
        state->addUniform(new osg::Uniform("shadowSpaceMatrix1", constantShadowCoordinates(.5f, .5f, .5f)));
        state->addUniform(new osg::Uniform("validRegionMatrix0", constantShadowCoordinates(0, 0, .5f)));
        state->addUniform(new osg::Uniform("validRegionMatrix1", constantShadowCoordinates(0, 0, .5f)));
        state->addUniform(new osg::Uniform("maximumShadowMapDistance", 1000.f));
        state->addUniform(new osg::Uniform("shadowFadeStart", 900.f));

        osg::ref_ptr<osg::Light> sun = new osg::Light;
        sun->setPosition({ 0, -1, 0, 0 }); sun->setDiffuse({ .8f, .8f, .8f, 1 });
        root->setSunlight(sun);
        osg::ref_ptr<osg::Light> torch = new osg::Light;
        torch->setDiffuse({ 1, .65f, .35f, 1 }); torch->setAmbient({ 0, 0, 0, 1 });
        torch->setConstantAttenuation(1); torch->setLinearAttenuation(.01f); torch->setQuadraticAttenuation(0);
        osg::ref_ptr<SceneUtil::LightSource> point = new SceneUtil::LightSource;
        point->setLight(torch); point->setRadius(100); point->setEmpty(true);
        osg::ref_ptr<osg::MatrixTransform> pointTransform = new osg::MatrixTransform;
        pointTransform->setMatrix(osg::Matrixf::translate(-30, -30, 40));
        pointTransform->addChild(point); root->addChild(pointTransform);

        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        image->setColor({ 1, 0, 0, 1 }, 0, 0);
        MWRender::BloodDecals blood(*root);
        blood.setTextures({ image }, { image }); blood.setCellActive("native-lighting", true);
        MWRender::BloodStamp stamp;
        stamp.mCenter = { -30, 0, 40 }; stamp.mNormal = { 0, -1, 0 }; stamp.mRadius = 20; stamp.mVisualSeed = 8;
        stamp.mTriangles = { { -50, 0, 20 }, { -10, 0, 20 }, { -10, 0, 60 },
            { -50, 0, 20 }, { -10, 0, 60 }, { -50, 0, 60 } };
        blood.add("native-lighting", stamp); blood.update(.1f, 1200);
        auto* program = bloodProgram(*root);
        require(program && program->getNumShaders() == 4, "Native factory did not replace blood's fallback shadow libraries");
        viewer.setSceneData(root); viewer.realize();

        if (argc > 2 && std::string(argv[2]) == "counter")
        {
            drawNativeLayerCounter(viewer, reversed);
            return 0;
        }

        if (argc > 2 && std::string(argv[2]) == "landscape")
        {
            for (const bool distant : { false, true })
            {
                const auto flat = drawNativeExteriorBlood(viewer, reversed, distant, 1, 3, 300, false, true);
                const auto rough = drawNativeExteriorBlood(viewer, reversed, distant, 1, 3, 300, true, true);
                require(rough >= 200 && rough >= flat * .6f,
                    "Ballistic splashes disappeared on rough native LAND without any pools");
            }
            return 0;
        }

        if (argc > 2 && (std::string(argv[2]) == "overlap" || std::string(argv[2]) == "perimeter"))
        {
            drawNativeOverlapBlood(viewer, reversed, std::string(argv[2]) == "perimeter");
            return 0;
        }

        if (argc > 2 && std::string(argv[2]) == "exterior")
        {
            for (const bool distant : { false, true }) drawNativeExteriorBlood(viewer, reversed, distant);
            return 0;
        }

        if (argc > 2 && std::string(argv[2]) == "density")
        {
            for (const bool distant : { false, true })
            {
                const auto normal = drawNativeExteriorBlood(viewer, reversed, distant, 1, 3, 600);
                const auto maximum = drawNativeExteriorBlood(viewer, reversed, distant, 3, 3, 600);
                require(normal >= 500, "Three ordinary deaths left only tiny, sparse floor marks at the default blood amount");
                require(maximum >= normal * 2, "300% blood failed to substantially increase actual visible floor coverage");
            }
            return 0;
        }

        const auto sample = [&] {
            viewer.frame(); glFinish();
            auto* graphicsState = camera->getGraphicsContext()->getState();
            const auto* linked = program->getPCP(*graphicsState);
            std::string log;
            if (linked) linked->getInfoLog(log);
            require(linked && linked->isLinked(), "Actual native blood/shadow GLSL failed to link: " + log);
            const auto window = stamp.mCenter * camera->getViewMatrix() * camera->getProjectionMatrix()
                * camera->getViewport()->computeWindowMatrix();
            std::array<unsigned char, 4> pixel{};
            glReadPixels(int(window.x()), int(window.y()), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
            require(glGetError() == GL_NO_ERROR, "Native blood/shadow draw raised an OpenGL error");
            return osg::Vec3f(pixel[0], pixel[1], pixel[2]);
        };
        const auto firstCascadeLit = sample();
        require(firstCascadeLit.x() > 70, "First lit native shadow cascade did not illuminate blood");
        state->getUniform("shadowSpaceMatrix0")->set(constantShadowCoordinates(2, 2, .5f));
        const auto secondCascadeShadowed = sample();
        require(secondCascadeShadowed.length() <= 3, "Blood ignored the actual second-cascade depth comparison");
        *reinterpret_cast<float*>(secondDepth->getImage()->data()) = .75f;
        secondDepth->getImage()->dirty();
        const auto secondCascadeLit = sample();
        require((secondCascadeLit - firstCascadeLit).length() <= 3, "Second native cascade did not restore direct sunlight");
        *reinterpret_cast<float*>(secondDepth->getImage()->data()) = .25f;
        secondDepth->getImage()->dirty();
        ambient->setAmbientIntensity({ .2f, .2f, .2f, 1 });
        const auto ambientShadowed = sample();
        require(ambientShadowed.x() > 15, "Native sun shadow incorrectly removed blood's ambient illumination");
        sun->setDiffuse({ 0, 0, 0, 1 });
        require((sample() - ambientShadowed).length() <= 3, "Shadowed sunlight leaked into the ambient blood color");
        ambient->setAmbientIntensity({ 0, 0, 0, 1 });
        point->setEmpty(false);
        const auto pointOnly = sample();
        require(pointOnly.x() > 40, "Native LightManager point light did not reach the blood shader");
        sun->setDiffuse({ .8f, .8f, .8f, 1 });
        require((sample() - pointOnly).length() <= 3, "Native sun shadow incorrectly removed or changed torch illumination");
        point->setEmpty(true);
        state->getUniform("maximumShadowMapDistance")->set(100.f);
        state->getUniform("shadowFadeStart")->set(50.f);
        require((sample() - firstCascadeLit).length() <= 3, "Native shadow-distance fade did not restore blood sunlight");
        state->getUniform("maximumShadowMapDistance")->set(1000.f);
        state->getUniform("shadowFadeStart")->set(900.f);

        // The same ShaderManager-owned shader objects must recompile/relink in
        // existing chunk programs when the player changes shadow settings.
        defines["shadows_enabled"] = "0";
        defines["shadow_texture_unit_list"] = "";
        shaders.setGlobalDefines(defines);
        require((sample() - firstCascadeLit).length() <= 3, "Existing blood failed native shadow-disabled shader reload");
        defines["shadows_enabled"] = "1";
        defines["shadow_texture_unit_list"] = "0,1";
        defines["perspectiveShadowMaps"] = "0";
        shaders.setGlobalDefines(defines);
        require(sample().length() <= 3, "Existing blood failed native orthographic-cascade shader reload");
        // Sprites have a different vertex program from surface decals. Link
        // that actual program against the same native shadow libraries too.
        blood.clear();
        sun->setPosition({ 0, 0, 1, 0 });
        ambient->setAmbientIntensity({ 0, 0, 0, 1 });
        MWRender::BloodTrail airborne(*root, {}, {});
        airborne.update(.1f, { { { -30, 0, 40 }, { 1, 0, 0 }, 1, true } }, .1f, .14f);
        require(airborne.particleCount() == 1, "Native sprite fixture did not isolate one airborne drop");
        auto* spriteNode = root->getChild(root->getNumChildren() - 1)->asGroup()->getChild(0)->asGeode();
        auto* spriteMesh = spriteNode->getDrawable(0)->asGeometry();
        auto* spriteProgram = dynamic_cast<osg::Program*>(spriteNode->getStateSet()->getAttribute(osg::StateAttribute::PROGRAM));
        require(spriteProgram && spriteProgram->getNumShaders() == 4,
            "Native factory did not install real shadow libraries in the sprite program");
        require(spriteMesh->getVertexArray()->getNumElements() == 4 && spriteMesh->getPrimitiveSet(0)->getNumIndices() == 6,
            "Native airborne blood still used a solid mesh");
        const auto spritePixels = [&] {
            viewer.frame(); glFinish();
            const auto* linked = spriteProgram->getPCP(*camera->getGraphicsContext()->getState());
            std::string log;
            if (linked) linked->getInfoLog(log);
            require(linked && linked->isLinked(), "Native blood sprite/shadow GLSL failed to link: " + log);
            std::vector<unsigned char> pixels(Width * Height * 3);
            glReadPixels(0, 0, Width, Height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
            require(glGetError() == GL_NO_ERROR, "Native sprite/shadow draw raised an OpenGL error");
            unsigned visible = 0;
            for (std::size_t i = 0; i < pixels.size(); i += 3)
                visible += pixels[i] > 3 || pixels[i + 1] > 3 || pixels[i + 2] > 3;
            return visible;
        };
        require(spritePixels() == 0, "Camera-facing blood sprites ignored the actual native shadow depth comparison");
        *reinterpret_cast<float*>(secondDepth->getImage()->data()) = .75f;
        secondDepth->getImage()->dirty();
        require(spritePixels() > 3, "Native world-lit blood sprite was invisible after leaving the shadow");
        sun->setDiffuse({ 0, 0, 0, 1 });
        require(spritePixels() == 0, "Native blood sprite glowed after its actual sunlight was removed");
        ambient->setAmbientIntensity({ .8f, .8f, .8f, 1 });
        require(spritePixels() > 3, "Native blood sprite failed to receive the scene's ambient light");
        ambient->setAmbientIntensity({ 0, 0, 0, 1 });
        const auto spriteCenter = (*static_cast<const osg::Vec3Array*>(spriteMesh->getVertexArray()))[0];
        pointTransform->setMatrix(osg::Matrixf::translate(spriteCenter + osg::Vec3f(0, -30, 0)));
        point->setEmpty(false);
        require(spritePixels() > 3, "A real native torch at the sprite's own height failed to illuminate blood");
        point->setEmpty(true);
        require(spritePixels() == 0, "Native blood sprites remained illuminated after their torch was extinguished");

        {
            // Terrain keeps its vertices near a local chunk origin. Blood must
            // use the same precision strategy; world-sized float vertices lose
            // the shallow-angle depth test far from the world origin.
            osg::ref_ptr<osg::Group> precisionRoot = new osg::Group;
            auto* precisionState = precisionRoot->getOrCreateStateSet();
            precisionState->merge(*MWRender::createBloodLightingState());
            osg::ref_ptr<osg::LightModel> sceneAmbient = new osg::LightModel;
            sceneAmbient->setAmbientIntensity({ 1, 1, 1, 1 });
            precisionState->setAttributeAndModes(sceneAmbient, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            precisionState->setAttributeAndModes(new SceneUtil::AutoDepth, osg::StateAttribute::ON);
            osg::ref_ptr<osg::MatrixTransform> groundTransform = new osg::MatrixTransform;
            osg::ref_ptr<Terrain::TerrainDrawable> ground = new Terrain::TerrainDrawable;
            osg::ref_ptr<osg::Vec3Array> groundVertices = new osg::Vec3Array;
            const std::array groundCorners{
                osg::Vec3f(-4000, -4000, 0), osg::Vec3f(4000, -4000, 0), osg::Vec3f(4000, 4000, 0),
                osg::Vec3f(-4000, -4000, 0), osg::Vec3f(4000, 4000, 0), osg::Vec3f(-4000, 4000, 0)
            };
            for (const auto& vertex : groundCorners)
                groundVertices->push_back(vertex + osg::Vec3f(0, 0, vertex.x() * .2f + vertex.y() * .1f));
            ground->setVertexArray(groundVertices);
            ground->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, groundVertices->size()));
            ground->setCullingActive(false); ground->createClusterCullingCallback();
            osg::ref_ptr<osg::StateSet> groundPass = new osg::StateSet;
            groundPass->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
            groundPass->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
            groundPass->setAttributeAndModes(new SceneUtil::AutoDepth, osg::StateAttribute::ON);
            osg::ref_ptr<osg::Program> groundProgram = new osg::Program;
            groundProgram->addShader(new osg::Shader(osg::Shader::VERTEX,
                "#version 120\nuniform mat4 projectionMatrix;void main(){gl_Position=projectionMatrix*gl_ModelViewMatrix*gl_Vertex;}"));
            groundProgram->addShader(new osg::Shader(osg::Shader::FRAGMENT,
                "#version 120\nvoid main(){gl_FragColor=vec4(.3,.3,.3,1.0);}"));
            groundPass->setAttributeAndModes(groundProgram, osg::StateAttribute::ON);
            ground->setPasses({ groundPass });
            groundTransform->addChild(ground); precisionRoot->addChild(groundTransform);
            MWRender::BloodDecals preciseBlood(*precisionRoot);
            preciseBlood.setTextures({ image }, { image });
            viewer.setSceneData(precisionRoot);
            camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
            unsigned originPixels = 0;
            for (const auto& world : { osg::Vec3f(), osg::Vec3f(-45056, -28672, 512), osg::Vec3f(500000, 500000, 1000) })
            {
                groundTransform->setMatrix(osg::Matrixf::translate(world));
                const double distance = 700, rise = std::tan(8 * 3.141592653589793 / 180) * distance;
                camera->setViewMatrixAsLookAt(osg::Vec3d(world) + osg::Vec3d(-distance * .707106781, -distance * .707106781, rise),
                    osg::Vec3d(world), { 0, 0, 1 });
                camera->setProjectionMatrixAsPerspective(60, double(Width) / Height, 1, 81920);
                auto precisionProjection = camera->getProjectionMatrix();
                if (reversed) for (int row = 0; row < 4; ++row) precisionProjection(row, 2) = -precisionProjection(row, 2);
                precisionState->addUniform(new osg::Uniform("projectionMatrix", osg::Matrixf(precisionProjection)));
                preciseBlood.clear(); preciseBlood.setCellActive("exterior-depth", true);
                MWRender::BloodStamp projected;
                projected.mCenter = world; projected.mNormal = { -.2f, -.1f, 1 };
                projected.mNormal.normalize(); projected.mRadius = 20; projected.mVisualSeed = 8;
                const std::array corners{
                    osg::Vec3f(-20, -20, 0), osg::Vec3f(20, -20, 0), osg::Vec3f(20, 20, 0),
                    osg::Vec3f(-20, -20, 0), osg::Vec3f(20, 20, 0), osg::Vec3f(-20, 20, 0)
                };
                for (const auto& vertex : corners)
                    projected.mTriangles.push_back(world + vertex
                        + osg::Vec3f(0, 0, vertex.x() * .2f + vertex.y() * .1f) + projected.mNormal * .03f);
                preciseBlood.add("exterior-depth", projected); preciseBlood.update(.1f, 1200);
                const auto pixels = [&] {
                    viewer.frame(); glFinish();
                    std::vector<unsigned char> buffer(Width * Height * 3);
                    glReadPixels(0, 0, Width, Height, GL_RGB, GL_UNSIGNED_BYTE, buffer.data());
                    require(glGetError() == GL_NO_ERROR, "Native exterior terrain/blood draw failed");
                    unsigned visible = 0;
                    for (std::size_t i = 0; i < buffer.size(); i += 3)
                        visible += buffer[i] > buffer[i + 1] + 12 && buffer[i] > buffer[i + 2] + 12;
                    return visible;
                };
                pixels();
                const unsigned visible = pixels();
                if (world == osg::Vec3f())
                {
                    originPixels = visible;
                    require(originPixels > 25, "Native sloped terrain fixture did not display blood at the origin");
                }
                else require(visible >= originPixels * .9f && visible <= originPixels * 1.1f,
                    "Blood lost visible terrain coverage at far exterior coordinates or a shallow camera angle");
            }
        }
        std::cout << "PASS actual native GLSL compilation/link, LightManager callbacks, two depth-comparison cascades, "
            "ambient/torch preservation, shadow fade, shader reload, billboard sprites and far exterior terrain depth ("
            << (reversed ? "reversed" : "normal") << " depth)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
