// Optional Linux/EGL regression for the real OpenMW projection/depth contract.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <apps/openmw/mwrender/ragdollappearance.hpp>
#include <apps/openmw/mwrender/brutalblood.hpp>
#include <apps/openmw/mwrender/brutalbloodlightingbase.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <components/sceneutil/depth.hpp>
#include <iostream>
#include <memory>
#include <osg/AlphaFunc>
#include <osg/Fog>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/Light>
#include <osg/LightModel>
#include <osg/Program>
#include <osg/ShapeDrawable>
#include <osg/Shader>
#include <osgViewer/GraphicsWindow>
#include <osgViewer/Viewer>
#include <osgDB/ReadFile>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
    constexpr int width = 320, height = 240;
    osg::Geode* bloodGeode(osg::Node& node)
    {
        if (auto* geode = node.asGeode(); geode && geode->getName() == "Brutal Morrowind blood stains")
            return geode;
        if (auto* group = node.asGroup())
            for (unsigned child = 0; child < group->getNumChildren(); ++child)
                if (auto* result = bloodGeode(*group->getChild(child))) return result;
        return nullptr;
    }
}
int main(int argc, char** argv)
{
    try
    {
        const bool reversed = argc > 1 && std::string(argv[1]) == "reversed";
        const std::string effect = argc > 2 ? argv[2] : "fire";
        const auto element = effect == "poison" ? Ragdoll::DeathElement::Poison
            : effect == "shock"                 ? Ragdoll::DeathElement::Shock
                                                : Ragdoll::DeathElement::Fire;
        SceneUtil::AutoDepth::setReversed(reversed);
        const auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        check(platform, "EGL surfaceless display unavailable");
        const auto display = platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        EGLint major, minor;
        check(eglInitialize(display, &major, &minor) && eglBindAPI(EGL_OPENGL_API), "EGL initialization failed");
        const EGLint attributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE };
        EGLConfig config; EGLint count;
        check(eglChooseConfig(display, attributes, &config, 1, &count) && count, "EGL config missing");
        const EGLint size[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
        const auto surface = eglCreatePbufferSurface(display, config, size);
        const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
        check(eglMakeCurrent(display, surface, surface, context), "EGL context missing");

        osgViewer::Viewer viewer;
        viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
        viewer.setUpViewerAsEmbeddedInWindow(0, 0, width, height);
        auto* camera = viewer.getCamera();
        camera->setClearColor({ 0.12f, 0.15f, 0.19f, 1 });
        camera->setClearDepth(reversed ? 0 : 1);
        camera->setProjectionMatrixAsOrtho(-80, 80, -20, 100, 1, 500);
        camera->setViewMatrixAsLookAt({ 0, -190, 40 }, { 0, 0, 40 }, { 0, 0, 1 });
        camera->setDrawBuffer(GL_FRONT); camera->setReadBuffer(GL_FRONT);
        // Exactly like RenderingManager: culling keeps a conventional projection,
        // shaders receive the actual (possibly reversed) per-view projection.
        auto renderProjection = camera->getProjectionMatrix();
        if (reversed) for (int row=0; row<4; ++row) renderProjection(row,2) = -renderProjection(row,2);
        osg::ref_ptr<osg::Group> root = new osg::Group;
        auto* state = root->getOrCreateStateSet();
        state->addUniform(new osg::Uniform("projectionMatrix", osg::Matrixf(renderProjection)));
        if (effect.rfind("blood", 0) == 0)
        {
            state->merge(*MWRender::createBloodLightingState());
            osg::ref_ptr<osg::LightModel> lightModel = new osg::LightModel;
            lightModel->setAmbientIntensity({ .8f, .8f, .8f, 1 });
            state->setAttributeAndModes(lightModel, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        }
        state->setAttributeAndModes(new SceneUtil::AutoDepth, osg::StateAttribute::ON);
        // NPC materials may enable alpha testing and override their shader.
        state->setAttributeAndModes(new osg::AlphaFunc(osg::AlphaFunc::GREATER, 0.95f), osg::StateAttribute::ON);
        osg::ref_ptr<osg::Program> opaque = new osg::Program;
        opaque->addShader(new osg::Shader(osg::Shader::VERTEX,
            "#version 120\nuniform mat4 projectionMatrix; void main(){gl_Position=projectionMatrix*gl_ModelViewMatrix*gl_Vertex;}"));
        opaque->addShader(new osg::Shader(osg::Shader::FRAGMENT,
            "#version 120\nvoid main(){gl_FragColor=vec4(0.25,0.25,0.25,1.0);}"));
        state->setAttributeAndModes(opaque, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        osg::ref_ptr<osg::Geometry> wall = new osg::Geometry;
        const osg::Vec3f corners[] = { {0,-40,-50}, {100,-40,-50}, {100,-40,200}, {0,-40,200} };
        wall->setVertexArray(new osg::Vec3Array(4, corners));
        wall->addPrimitiveSet(new osg::DrawArrays(GL_QUADS, 0, 4));
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(wall); root->addChild(geode);
        viewer.setSceneData(root); viewer.realize();
        if (effect == "blood-sprites")
        {
            root->removeChild(geode);
            camera->setClearColor({ 0, 0, 0, 1 });
            auto* ambient = static_cast<osg::LightModel*>(state->getAttribute(osg::StateAttribute::LIGHTMODEL));
            check(ambient, "Blood sprite fixture lost the real ambient light state");
            MWRender::BloodTrail airborne(*root, {}, {});
            airborne.update(.1f, { { { 0, 0, 40 }, { 1, 0, 0 }, 1, true } }, .1f, .14f);
            check(airborne.particleCount() == 1, "Isolated sprite fixture emitted an unexpected droplet count");
            auto* spriteNode = root->getChild(0)->asGroup()->getChild(0)->asGeode();
            auto* mesh = spriteNode->getDrawable(0)->asGeometry();
            auto* vertices = static_cast<osg::Vec3Array*>(mesh->getVertexArray());
            auto* sizes = static_cast<osg::Vec2Array*>(mesh->getTexCoordArray(1));
            auto* travel = static_cast<osg::Vec3Array*>(mesh->getNormalArray());
            check(vertices->size() == 4 && mesh->getPrimitiveSet(0)->getNumIndices() == 6,
                "A visible blood droplet still used solid geometry rather than a two-triangle sprite");
            const auto center = (*vertices)[0];
            const float naturalRadius = std::sqrt((*sizes)[0].length2());
            const auto naturalBounds = mesh->getBoundingBox();
            check(naturalBounds.contains(center - osg::Vec3f(naturalRadius, naturalRadius, naturalRadius))
                && naturalBounds.contains(center + osg::Vec3f(naturalRadius, naturalRadius, naturalRadius)),
                "GPU-expanded blood sprites were culled using their degenerate center-only CPU bounds");
            auto* texture = dynamic_cast<osg::Texture2D*>(spriteNode->getStateSet()->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
            check(texture && texture->getImage(), "Blood sprites did not use a shared alpha-mask texture");
            const auto* image = texture->getImage();
            check(image->getColor(0, 0).a() < .01f && image->getColor(image->s() - 1, image->t() - 1).a() < .01f,
                "Blood sprite texture retained opaque rectangular corners");
            bool smooth = false;
            for (int y = 0; y < image->t(); ++y) for (int x = 0; x < image->s(); ++x)
            {
                const float alpha = image->getColor(x, y).a();
                smooth |= alpha > .05f && alpha < .95f;
            }
            check(smooth, "Blood sprite silhouette lost its smoothly transparent boundary");
            // Enlarge only this isolated test sprite to sample its transparent
            // corners independently of subpixel rounding at the default size.
            for (auto& size : *sizes) size.set(6, 10);
            sizes->dirty();
            mesh->setInitialBound(osg::BoundingBox(center - osg::Vec3f(15, 15, 15), center + osg::Vec3f(15, 15, 15)));
            mesh->dirtyBound();
            const auto read = [&] {
                viewer.frame(); glFinish();
                std::vector<unsigned char> pixels(width * height * 3);
                glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                check(glGetError() == GL_NO_ERROR, "OpenGL sprite drawing failed");
                return pixels;
            };
            const auto visible = [&](const auto& pixels) {
                unsigned count = 0;
                for (std::size_t i = 0; i < pixels.size(); i += 3)
                    count += pixels[i] > 5 || pixels[i + 1] > 5 || pixels[i + 2] > 5;
                return count;
            };
            for (const auto& view : { osg::Vec3f(0, -190, 0), osg::Vec3f(190, 0, 0), osg::Vec3f(-125, -125, 70) })
            {
                camera->setViewMatrixAsLookAt(center + view, center, { 0, 0, 1 });
                ambient->setAmbientIntensity({ 0, 0, 0, 1 });
                check(visible(read()) == 0, "A camera-facing blood sprite emitted light in a dark scene");
                ambient->setAmbientIntensity({ .8f, .8f, .8f, 1 });
                const auto pixels = read();
                const unsigned filled = visible(pixels);
                check(filled > 30 && filled < 800,
                    "Blood sprite was invisible edge-on or rendered its whole rectangular billboard");
                const osg::Vec3d eye = osg::Vec3d(center) * camera->getViewMatrix();
                const auto direction = osg::Matrixf::transform3x3((*travel)[0], osg::Matrixf(camera->getViewMatrix()));
                osg::Vec2f axis(direction.x(), direction.y());
                const float projected = axis.length();
                if (projected < .001f) axis.set(0, 1); else axis /= projected;
                const osg::Vec2f across(axis.y(), -axis.x());
                const float length = 6 + 4 * std::clamp(projected, 0.f, 1.f);
                const auto corner = eye + osg::Vec3d(across.x() * 5.5f + axis.x() * length * .94f,
                    across.y() * 5.5f + axis.y() * length * .94f, 0);
                const auto window = corner * camera->getProjectionMatrix() * camera->getViewport()->computeWindowMatrix();
                const auto offset = (std::size_t(window.y()) * width + std::size_t(window.x())) * 3;
                check(offset + 2 < pixels.size() && pixels[offset] < 3 && pixels[offset + 1] < 3 && pixels[offset + 2] < 3,
                    "Blood sprite alpha mask did not remove billboard corners in the actual shader");
            }
            ambient->setAmbientIntensity({ 0, 0, 0, 1 });
            osg::ref_ptr<osg::Light> torch = new osg::Light;
            torch->setPosition(osg::Vec4f(center + osg::Vec3f(0, -80, 0), 1));
            torch->setDiffuse({ 1, .65f, .35f, 1 }); torch->setAmbient({ 0, 0, 0, 1 });
            torch->setConstantAttenuation(1); torch->setLinearAttenuation(.001f); torch->setQuadraticAttenuation(0);
            unsigned maximumRed = 0;
            for (const auto& view : { osg::Vec3f(0, -190, 0), osg::Vec3f(190, 0, 0) })
            {
                camera->setViewMatrixAsLookAt(center + view, center, { 0, 0, 1 });
                MWRender::setBloodPointLight(*state, 0, *torch, 200, osg::Matrixf(camera->getViewMatrix()));
                const auto pixels = read();
                check(visible(pixels) > 30, "A native torch at the blood sprite's height failed to illuminate it");
                unsigned red = 0;
                for (std::size_t i = 0; i < pixels.size(); i += 3) red = std::max(red, unsigned(pixels[i]));
                check(maximumRed == 0 || std::abs(int(red) - int(maximumRed)) <= 4,
                    "Blood sprite lighting rotated with the camera rather than its native torch");
                maximumRed = red;
            }
            state->getUniform("brutalBloodPointCount")->set(0);
            check(visible(read()) == 0, "Blood sprites remained lit after their native point light was extinguished");
            MWRender::BloodTrail another(*root, {}, {});
            auto* otherNode = root->getChild(1)->asGroup()->getChild(0)->asGeode();
            check(otherNode->getStateSet()->getTextureAttribute(0, osg::StateAttribute::TEXTURE) == texture,
                "Each blood source allocated its own sprite texture");
            std::cout << "PASS batched two-triangle blood sprites, GPU billboarding at three angles, alpha silhouette, dark/lit response and conservative bounds\n";
            return 0;
        }
        if (effect == "blood-lighting")
        {
            root->removeChild(geode);
            camera->setClearColor({ 0, 0, 0, 1 });
            auto* ambient = static_cast<osg::LightModel*>(state->getAttribute(osg::StateAttribute::LIGHTMODEL));
            check(ambient, "Blood lighting fixture did not use the actual OpenGL light-model ambient state");
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            image->setColor({ 1, 0, 0, 1 }, 0, 0);
            // Exercise the same library replacement used by ShaderManager's
            // cascade shaders, with deterministic shadow visibility for pixels.
            osg::ref_ptr<osg::Shader> shadowVertex = new osg::Shader(osg::Shader::VERTEX,
                "#version 120\nvoid setupShadowCoords(vec4 viewPos,vec3 viewNormal){}");
            osg::ref_ptr<osg::Shader> shadowFragment = new osg::Shader(osg::Shader::FRAGMENT,
                "#version 120\nuniform float testSunVisibility;float unshadowedLightRatio(float distance){return testSunVisibility;}");
            MWRender::setBloodLightingRoot(*root, {}, shadowVertex, shadowFragment);
            state->addUniform(new osg::Uniform("testSunVisibility", 1.f));
            MWRender::BloodDecals blood(*root);
            blood.setTextures({ image }, { image }); blood.setCellActive("lighting", true);
            MWRender::BloodStamp stamp;
            stamp.mCenter = { -30, 0, 40 }; stamp.mNormal = { 0, -1, 0 }; stamp.mRadius = 20; stamp.mVisualSeed = 8;
            stamp.mTriangles = { { -50, 0, 20 }, { -10, 0, 20 }, { -10, 0, 60 },
                { -50, 0, 20 }, { -10, 0, 60 }, { -50, 0, 60 } };
            blood.add("lighting", stamp); blood.update(.1f, 1200);
            const auto sample = [&] {
                viewer.frame(); glFinish();
                const auto window = stamp.mCenter * camera->getViewMatrix() * camera->getProjectionMatrix()
                    * camera->getViewport()->computeWindowMatrix();
                std::array<unsigned char, 3> pixel{};
                glReadPixels(int(window.x()), int(window.y()), 1, 1, GL_RGB, GL_UNSIGNED_BYTE, pixel.data());
                check(glGetError() == GL_NO_ERROR, "OpenGL world-lit blood error");
                return osg::Vec3f(pixel[0], pixel[1], pixel[2]);
            };
            ambient->setAmbientIntensity({ 0, 0, 0, 1 });
            const auto dark = sample();
            check(dark.length() <= 2, "Blood emitted light in an unlit room with zero ambient illumination");
            ambient->setAmbientIntensity({ .8f, .8f, .8f, 1 });
            const auto lit = sample();
            check(lit.x() > dark.x() + 70 && lit.x() > lit.y() + 40,
                "Blood did not brighten with the real world ambient light");
            ambient->setAmbientIntensity({ 0, 0, 0, 1 });
            check((sample() - dark).length() <= 2, "Existing blood did not darken after ambient lighting changed");
            osg::ref_ptr<osg::Light> sun = new osg::Light;
            sun->setPosition({ 0, -1, 0, 0 }); sun->setDiffuse({ .8f, .8f, .8f, 1 });
            MWRender::setBloodSunLight(*state, *sun, osg::Matrixf(camera->getViewMatrix()));
            check(sample().x() > dark.x() + 50, "Blood ignored the native directional sunlight state");
            state->getUniform("testSunVisibility")->set(0.f);
            check(sample().length() <= 3, "Blood ignored the linked world-shadow visibility shader");
            ambient->setAmbientIntensity({ .2f, .2f, .2f, 1 });
            check(sample().x() > 10, "A direct-sun shadow incorrectly removed ambient illumination from blood");
            ambient->setAmbientIntensity({ 0, 0, 0, 1 });
            sun->setDiffuse({ 0, 0, 0, 1 });
            MWRender::setBloodSunLight(*state, *sun, osg::Matrixf(camera->getViewMatrix()));
            osg::ref_ptr<osg::Light> torch = new osg::Light;
            torch->setPosition({ -30, -30, 40, 1 }); torch->setDiffuse({ 1, .65f, .35f, 1 });
            torch->setAmbient({ 0, 0, 0, 1 }); torch->setConstantAttenuation(1);
            torch->setLinearAttenuation(.01f); torch->setQuadraticAttenuation(0);
            MWRender::setBloodPointLight(*state, 0, *torch, 100, osg::Matrixf(camera->getViewMatrix()));
            const auto torchLit = sample();
            check(torchLit.x() > dark.x() + 40, "Blood ignored a nearby native point light");
            torch->setPosition({ 500, -30, 40, 1 });
            MWRender::setBloodPointLight(*state, 0, *torch, 100, osg::Matrixf(camera->getViewMatrix()));
            check(sample().length() <= 3, "A distant point light illuminated blood outside its radius");
            {
                state->getUniform("brutalBloodPointCount")->set(0);
                MWRender::BloodTrail airborne(*root, {}, {});
                airborne.update(.1f, { { { 30, 0, 40 }, { 1, 0, 0 }, 1, true } }, 1, .14f);
                const auto countVisibleDrops = [&] {
                    viewer.frame(); glFinish();
                    std::vector<unsigned char> pixels(width * height * 3);
                    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                    check(glGetError() == GL_NO_ERROR, "OpenGL world-lit airborne blood error");
                    unsigned visible = 0;
                    for (int y = 0; y < height; ++y) for (int x = width / 2; x < width; ++x)
                    {
                        const auto offset = (y * width + x) * 3;
                        visible += pixels[offset] > 5 || pixels[offset + 1] > 5 || pixels[offset + 2] > 5;
                    }
                    return visible;
                };
                ambient->setAmbientIntensity({ 0, 0, 0, 1 });
                check(countVisibleDrops() == 0, "Airborne droplets glowed despite zero scene illumination");
                ambient->setAmbientIntensity({ .8f, .8f, .8f, 1 });
                check(countVisibleDrops() > 10, "World-lit airborne droplets were not visibly rendered in the light");
                ambient->setAmbientIntensity({ 0, 0, 0, 1 });
            }
            torch->setPosition({ -30, -30, 40, 1 });
            camera->setViewMatrixAsLookAt({ -65, -170, 65 }, { -30, 0, 40 }, { 0, 0, 1 });
            MWRender::setBloodSunLight(*state, *sun, osg::Matrixf(camera->getViewMatrix()));
            MWRender::setBloodPointLight(*state, 0, *torch, 100, osg::Matrixf(camera->getViewMatrix()));
            check((sample() - torchLit).length() <= 4,
                "Blood lighting changed with the camera instead of following the world-space light");
            osg::ref_ptr<osg::Fog> fog = new osg::Fog;
            fog->setMode(osg::Fog::LINEAR); fog->setStart(0); fog->setEnd(1); fog->setColor({ .1f, .2f, .3f, 1 });
            state->setAttributeAndModes(fog, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            state->getUniform("brutalBloodFogMode")->set(1);
            const auto fogged = sample();
            check(fogged.z() > fogged.x() + 25 && fogged.y() > fogged.x() + 10,
                "Blood ignored the native world fog instead of fading into the surrounding scene");
            std::cout << "PASS real ambient/directional/point blood lighting, darkness, shadow links, fog, radius falloff and camera-independent illumination\n";
            return 0;
        }
        if (effect == "blood-atlas")
        {
            const auto read = [&] {
                viewer.frame(); glFinish(); std::vector<unsigned char> pixels(width * height * 3);
                glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                check(glGetError() == GL_NO_ERROR, "OpenGL textured blood error"); return pixels;
            };
            const auto pixel = [&](const auto& pixels, const osg::Vec3f& point) {
                const auto window = point * camera->getViewMatrix() * camera->getProjectionMatrix()
                    * camera->getViewport()->computeWindowMatrix();
                const int x = std::clamp(int(window.x()), 0, width - 1), y = std::clamp(int(window.y()), 0, height - 1);
                const int offset = (y * width + x) * 3;
                return osg::Vec3f(pixels[offset], pixels[offset + 1], pixels[offset + 2]);
            };
            const auto stamp = [](bool floor, float x, std::uint32_t seed) {
                MWRender::BloodStamp s;
                s.mCenter = floor ? osg::Vec3f(x, 20, 0) : osg::Vec3f(x, 0, 40);
                s.mNormal = floor ? osg::Vec3f(0, 0, 1) : osg::Vec3f(0, -1, 0);
                s.mRadius = 20; s.mVisualSeed = seed;
                const osg::Vec3f right(20,0,0), up = floor ? osg::Vec3f(0,20,0) : osg::Vec3f(0,0,20);
                s.mTriangles = {s.mCenter-right-up,s.mCenter+right-up,s.mCenter+right+up,
                    s.mCenter-right-up,s.mCenter+right+up,s.mCenter-right+up};
                return s;
            };
            std::vector<osg::ref_ptr<osg::Image>> floors, walls;
            for (const char* name : {"floor06.png","floor07.png","wall01.png","wall02.png","wall03.png","wall04.png"})
            {
                auto image = osgDB::readRefImageFile(std::string(RAGDOLL_TEST_ASSET_DIR) + "/" + name);
                check(bool(image), "Original uploaded blood PNG could not be loaded");
                if (image->getOrigin() == osg::Image::BOTTOM_LEFT) image->flipVertical();
                image->setOrigin(osg::Image::TOP_LEFT); // Native Resource::ImageManager convention.
                (name[0] == 'f' ? floors : walls).push_back(image);
            }
            MWRender::BloodDecals blood(*root);
            blood.setTextures(floors, walls); blood.setCellActive("atlas", true);
            const auto baseline = read();
            blood.add("atlas", stamp(false, -30, 16)); blood.add("atlas", stamp(false, 30, 16));
            blood.update(.1f, 1200);
            const auto original = read();
            int visible = 0, occluded = 0;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
            {
                const int i = (y * width + x) * 3;
                if (original[i] != baseline[i] || original[i+1] != baseline[i+1] || original[i+2] != baseline[i+2])
                {
                    if (x < width/2) ++visible; else ++occluded;
                }
            }
            check(visible > 100 && occluded == 0, "Actual PNG decals are invisible or draw through opaque walls");
            blood.clear();
            osg::ref_ptr<osg::Image> marker = new osg::Image;
            marker->allocateImage(32,32,1,GL_RGBA,GL_UNSIGNED_BYTE); marker->setOrigin(osg::Image::TOP_LEFT);
            for (int y=0;y<32;++y) for (int x=0;x<32;++x)
            {
                const auto c = y < 16 ? (x < 16 ? osg::Vec4f(1,0,0,1) : osg::Vec4f(0,1,0,1))
                    : (x < 16 ? osg::Vec4f(0,0,1,1) : osg::Vec4f(1,1,0,1));
                marker->setColor(x < 4 || x > 27 || y < 4 || y > 27 ? osg::Vec4f() : c,x,y);
            }
            blood.setTextures({marker},{marker}); blood.setCellActive("atlas",true);
            std::array<float, 2> redShades{};
            for (unsigned seed : {8u,9u,10u})
            {
                blood.add("atlas",stamp(false,-30,seed)); blood.update(.1f,1200);
                const auto pixels=read();
                const auto upper=pixel(pixels,{-40,0,50}), lower=pixel(pixels,{-40,0,30});
                if (seed==9) check(upper.y()>upper.x()+30, "Horizontal wall mirror did not sample original artwork");
                else check(upper.x()>upper.y()+30 && lower.z()>lower.x()+30,
                    "Wall artwork was rotated or TOP_LEFT pixels were inverted");
                if (seed != 9)
                {
                    check(upper.x()>70 && upper.x()<150 && upper.z()>upper.y()+4,
                        "Actual textured blood did not render as dark burgundy instead of bright primary red");
                    redShades[seed == 8 ? 0 : 1] = upper.x();
                }
                check((pixel(pixels,{-49,0,21})-pixel(baseline,{-49,0,21})).length()<1,
                    "Transparent atlas border rendered a solid rectangle");
                const auto saved = blood.getSaveRecord();
                blood.loadSaveRecord(saved);
                check(read() == pixels, "Saving and loading changed the rendered blood shade or geometry");
                blood.clear(); blood.setCellActive("atlas",true);
            }
            check(std::abs(redShades[0]-redShades[1])>=2 && std::abs(redShades[0]-redShades[1])<20,
                "Actual blood shader ignored subtle per-decal shade variation");
            root->removeChild(geode);
            camera->setViewMatrixAsLookAt({0,0,190},{0,0,0},{0,1,0});
            blood.add("atlas",stamp(true,-30,8)); blood.add("atlas",stamp(true,30,10)); blood.update(.1f,1200);
            const auto floorPixels=read();
            const auto normal=pixel(floorPixels,{-40,30,0}), turned=pixel(floorPixels,{20,30,0});
            check(normal.x()>normal.y()+30 && turned.x()>turned.z()+30 && turned.y()>turned.z()+30,
                "Floor artwork failed its permitted 180-degree rotation");
            blood.clear();
            std::cout << "PASS actual PNG atlas, alpha silhouette, upright walls, horizontal mirror, floor 180, dark burgundy shades, saved appearance, native projection and occlusion\n";
            return 0;
        }
        if (effect == "blood")
        {
            const auto read = [&] {
                viewer.frame(); glFinish(); std::vector<unsigned char> pixels(width * height * 3);
                glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                check(glGetError() == GL_NO_ERROR, "OpenGL blood rendering error"); return pixels;
            };
            const auto baseline = read();
            MWRender::BloodDecals blood(*root);
            osg::ref_ptr<osg::Geode> flesh = new osg::Geode;
            flesh->setStateSet(MWRender::createWetGoreState());
            for (float x : { -30.f, 30.f })
            {
                osg::ref_ptr<osg::ShapeDrawable> tissue = new osg::ShapeDrawable(new osg::Sphere({ x, 0, 68 }, 8));
                tissue->setColor({ .4f, .02f, .03f, 1 }); flesh->addDrawable(tissue);
            }
            root->addChild(flesh);
            blood.setCellActive("test", true);
            for (float x : { -30.f, 30.f })
            {
                MWRender::BloodStamp stamp;
                stamp.mCenter = { x, 0, 40 }; stamp.mNormal = { 0, -1, 0 }; stamp.mRadius = 15;
                stamp.mTriangles = { { x - 15, 0, 25 }, { x + 15, 0, 25 }, { x + 15, 0, 55 },
                    { x - 15, 0, 25 }, { x + 15, 0, 55 }, { x - 15, 0, 55 } };
                blood.add("test", stamp);
            }
            blood.update(.1f, 600);
            const auto stained = read();
            int visible = 0, occluded = 0;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
            {
                const int i = (y * width + x) * 3;
                if (stained[i] != baseline[i] || stained[i + 1] != baseline[i + 1] || stained[i + 2] != baseline[i + 2])
                {
                    if (x < width / 2) { check(stained[i] > stained[i + 1] + 10, "Blood/flesh lost its material under an inherited shader"); ++visible; }
                    else ++occluded;
                }
            }
            check(visible > 500, "Blood decals invisible under native projection/alpha state");
            check(occluded == 0, "Blood decals visible through an opaque wall");
            root->removeChild(flesh);
            const auto wet = read();
            auto* stainMesh = bloodGeode(*root)->getDrawable(0)->asGeometry();
            osg::ref_ptr<osg::Array> stableVertices = stainMesh->getVertexArray();
            const auto modified = stableVertices->getModifiedCount();
            blood.update(90, 600);
            const auto dry = read();
            unsigned wetRed = 0, dryRed = 0;
            for (std::size_t i = 0; i < wet.size(); i += 3) { wetRed += wet[i]; dryRed += dry[i]; }
            check(dryRed < wetRed, "Blood no longer dries when aging is performed by the shader");
            check(stainMesh->getVertexArray() == stableVertices && stableVertices->getModifiedCount() == modified,
                "Aging uploaded another copy of the blood vertex buffer");
            blood.update(600, 600); check(read() == baseline, "Expired blood left a visible overlay");

            // Exercise an actual full GPU cache while eight wounds continue to
            // bleed. This crosses the eviction threshold repeatedly, rather
            // than checking only the first splat on a single corpse.
            camera->setProjectionMatrixAsOrtho(-600, 600, -100, 600, 1, 500);
            auto cacheProjection = camera->getProjectionMatrix();
            if (reversed) for (int row = 0; row < 4; ++row) cacheProjection(row, 2) = -cacheProjection(row, 2);
            state->getUniform("projectionMatrix")->set(osg::Matrixf(cacheProjection));
            MWRender::BloodStamp stamp;
            stamp.mNormal = { 0, -1, 0 }; stamp.mRadius = .4f;
            for (int i = 0; i < int(MWRender::BloodDecals::MaxStains + 1024); ++i)
            {
                // Distinct footprints keep this a capacity test under the
                // two-layer overlap budget instead of four stacked copies.
                const float x = -512.f + (i % 256) * 4.f, z = (i / 256) * 4.f;
                stamp.mCenter = { x, 0, z };
                stamp.mTriangles.clear();
                const auto a = stamp.mCenter + osg::Vec3f(-.4f, 0, -.4f);
                const auto b = stamp.mCenter + osg::Vec3f(.4f, 0, -.4f);
                const auto c = stamp.mCenter + osg::Vec3f(.4f, 0, .4f);
                const auto d = stamp.mCenter + osg::Vec3f(-.4f, 0, .4f);
                stamp.mTriangles = { a, b, c, a, c, d };
                blood.add("test", stamp);
            }
            blood.update(.1f, 600);
            check(blood.size() == MWRender::BloodDecals::MaxStains,
                "GPU stress did not actually fill and overflow the stain cache");
            viewer.frame();
            const auto surface = MWRender::BloodTrail::visualSurface({ { -1000, -1000, 0 }, { 1000, -1000, 0 },
                { 1000, 1000, 0 }, { -1000, -1000, 0 }, { 1000, 1000, 0 }, { -1000, 1000, 0 } });
            std::vector<std::unique_ptr<MWRender::BloodTrail>> trails;
            for (int i = 0; i < 8; ++i)
                trails.push_back(std::make_unique<MWRender::BloodTrail>(*root, surface.mRaycast,
                    [&](const auto& splat) { blood.add("test", splat); }, surface.mProjector, true));
            for (int frame = 0; frame < 180; ++frame)
            {
                for (int i = 0; i < 8; ++i)
                    trails[i]->update(.1f, { { { float(i * 10 - 40), 0, 30 }, { 0, 0, -1 }, 1, true } }, 3, frame * .1f);
                blood.update(.1f, 600);
                // Pool absorption can legitimately remove details after the
                // initial full-cache draw. Keep a near-full GPU workload while
                // checking the upper bound rather than requiring every absorbed
                // stain to be replaced by another redundant layer.
                check(blood.size() <= MWRender::BloodDecals::MaxStains
                    && blood.size() >= MWRender::BloodDecals::MaxStains * 9 / 10
                    && root->getNumChildren() <= MWRender::BloodDecals::MaxStains / MWRender::BloodDecals::ChunkStains + 10, "Full GPU blood cache grew without bound");
                viewer.frame();
                check(glGetError() == GL_NO_ERROR, "Full blood cache caused an OpenGL error");
            }
            glFinish(); trails.clear(); blood.clear();
            camera->setProjectionMatrixAsOrtho(-80, 80, -20, 100, 1, 500);
            state->getUniform("projectionMatrix")->set(osg::Matrixf(renderProjection));
            check(read() == baseline && root->getNumChildren() == 1, "Crowded blood leaked GPU/scene nodes after cleanup");
            std::cout << "PASS actual GL blood decals, native projection, inherited alpha/shader, opaque occlusion and expiry\n";
            std::cout << "PASS full bounded stain GPU cache rollover with eight simultaneous maximum-pressure blood trails\n";
            return 0;
        }
        if (effect == "wounds")
        {
            const auto read = [&] {
                viewer.frame(); glFinish();
                std::vector<unsigned char> pixels(width * height * 3);
                glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                check(glGetError() == GL_NO_ERROR, "OpenGL wound rendering error");
                return pixels;
            };
            // World surfaces have their own native shader. Wounds must work
            // both with no inherited program and with an NPC material override.
            geode->getOrCreateStateSet()->setAttributeAndModes(opaque,
                osg::StateAttribute::ON | osg::StateAttribute::PROTECTED);
            for (const bool inheritedProgram : { false, true })
            {
                if (inheritedProgram)
                    state->setAttributeAndModes(opaque, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                else
                    state->removeAttribute(osg::StateAttribute::PROGRAM);
                const auto baseline = read();
                osg::ref_ptr<osg::Geode> cuts = new osg::Geode;
                for (const float x : { -30.f, 30.f })
                {
                    auto* cap = new osg::Cylinder({ x, 0, 40 }, 12, .8f);
                    cap->setRotation(osg::Quat(osg::PI_2, osg::Vec3f(1, 0, 0)));
                    cuts->addDrawable(MWRender::createWoundDrawable(cap));
                }
                root->addChild(cuts);
                const auto wounded = read();
                int visible = 0, occluded = 0, sampleX = 0, sampleY = 0;
                for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
                {
                    const int i = (y * width + x) * 3;
                    bool changed = false;
                    for (int c = 0; c < 3; ++c) changed |= wounded[i+c] != baseline[i+c];
                    if (changed)
                    {
                        if (x < width/2)
                        {
                            check(wounded[i] > 2 * wounded[i+1] && wounded[i] > 2 * wounded[i+2],
                                "Wound inherited an NPC shader or clothing texture instead of its own material");
                            ++visible; sampleX = x; sampleY = y;
                        }
                        else ++occluded;
                    }
                }
                check(visible > 500, "Wound surface missing under inherited NPC shader/alpha state");
                check(occluded == 0, "Wound surface rendered through an opaque world surface");
                float capDepth = 0;
                glReadPixels(sampleX, sampleY, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &capDepth);
                check(capDepth > 0 && capDepth < 1, "Wound surface did not write its physical depth");
                cuts->setNodeMask(0);
                check(read() == baseline, "Hidden wound left an overlay in the frame");
                root->removeChild(cuts);
            }
            std::cout << "PASS real GL wound caps, " << (reversed ? "reversed" : "normal")
                      << " projection, opaque occlusion and depth writes\n";
            return 0;
        }
        MWRender::RagdollAppearance appearance(*root, element, {});
        const std::array<osg::Vec3f, 4> positions{ osg::Vec3f(-30,0,20), osg::Vec3f(-30,0,45),
            osg::Vec3f(30,0,20), osg::Vec3f(30,0,45) };
        std::vector<unsigned char> burning, finished;
        // Render after the normal corpse physics deadline, then after the
        // independent fire deadline. This checks the extra five visible seconds.
        const float activeTime = element == Ragdoll::DeathElement::Fire ? 12.f : 5.f;
        const float endTime = element == Ragdoll::DeathElement::Fire ? 15.f : 10.f;
        for (const float time : { activeTime, endTime })
        {
            appearance.update(time, {}, positions);
            viewer.frame(time); glFinish();
            auto& pixels = time == activeTime ? burning : finished;
            pixels.resize(width * height * 3);
            glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
            check(glGetError() == GL_NO_ERROR, "OpenGL rendering error");
        }
        int visible = 0, occluded = 0;
        for (int y=0; y<height; ++y) for (int x=0; x<width; ++x)
        {
            const int i = (y*width+x)*3;
            bool changed = false;
            for (int c=0; c<3; ++c) changed |= burning[i+c] != finished[i+c];
            if (changed) { if (x < width/2) ++visible; else ++occluded; }
        }
        check(visible > 500, "Fire/smoke invisible under the native projection or inherited NPC state");
        check(occluded == 0, "Fire rendered through an opaque wall");
        check(!appearance.hasParticles(), "Elemental particles did not end at their independent deadline");
        std::cout << "PASS real GL " << (reversed ? "reversed" : "normal") << " projection, " << visible << ' '
                  << effect << " pixels, opaque occlusion and " << endTime << " second timer\n";
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
