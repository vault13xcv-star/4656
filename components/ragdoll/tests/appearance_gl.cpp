// Optional Linux/EGL regression for the real OpenMW projection/depth contract.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <apps/openmw/mwrender/ragdollappearance.hpp>
#include <apps/openmw/mwrender/brutalblood.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <components/sceneutil/depth.hpp>
#include <iostream>
#include <memory>
#include <osg/AlphaFunc>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
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
            for (unsigned seed : {8u,9u,10u})
            {
                blood.add("atlas",stamp(false,-30,seed)); blood.update(.1f,1200);
                const auto pixels=read();
                const auto upper=pixel(pixels,{-40,0,50}), lower=pixel(pixels,{-40,0,30});
                if (seed==9) check(upper.y()>upper.x()+30, "Horizontal wall mirror did not sample original artwork");
                else check(upper.x()>upper.y()+30 && lower.z()>lower.x()+30,
                    "Wall artwork was rotated or TOP_LEFT pixels were inverted");
                check((pixel(pixels,{-49,0,21})-pixel(baseline,{-49,0,21})).length()<1,
                    "Transparent atlas border rendered a solid rectangle");
                blood.clear(); blood.setCellActive("atlas",true);
            }
            root->removeChild(geode);
            camera->setViewMatrixAsLookAt({0,0,190},{0,0,0},{0,1,0});
            blood.add("atlas",stamp(true,-30,8)); blood.add("atlas",stamp(true,30,10)); blood.update(.1f,1200);
            const auto floorPixels=read();
            const auto normal=pixel(floorPixels,{-40,30,0}), turned=pixel(floorPixels,{20,30,0});
            check(normal.x()>normal.y()+30 && turned.x()>turned.z()+30 && turned.y()>turned.z()+30,
                "Floor artwork failed its permitted 180-degree rotation");
            blood.clear();
            std::cout << "PASS actual PNG atlas, alpha silhouette, upright walls, horizontal mirror, floor 180, native projection and occlusion\n";
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
            auto* stainMesh = root->getChild(root->getNumChildren() - 1)->asGeode()->getDrawable(0)->asGeometry();
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
            MWRender::BloodStamp stamp;
            stamp.mNormal = { 0, -1, 0 }; stamp.mRadius = .4f;
            for (int i = 0; i < int(MWRender::BloodDecals::MaxStains + 1024); ++i)
            {
                const float x = -64.f + i % 128, z = float((i / 128) % 64);
                stamp.mCenter = { x, 0, z };
                stamp.mTriangles.clear();
                for (int edge = 0; edge < 16; ++edge)
                {
                    const float a = edge * 6.2831853f / 16, b = (edge + 1) * 6.2831853f / 16;
                    stamp.mTriangles.insert(stamp.mTriangles.end(), { stamp.mCenter,
                        stamp.mCenter + osg::Vec3f(std::cos(a) * .4f, 0, std::sin(a) * .4f),
                        stamp.mCenter + osg::Vec3f(std::cos(b) * .4f, 0, std::sin(b) * .4f) });
                }
                blood.add("test", stamp);
            }
            const auto ray = MWRender::BloodTrail::collisionRaycast({ { -1000, -1000, 0 }, { 1000, -1000, 0 },
                { 1000, 1000, 0 }, { -1000, -1000, 0 }, { 1000, 1000, 0 }, { -1000, 1000, 0 } });
            std::vector<std::unique_ptr<MWRender::BloodTrail>> trails;
            for (int i = 0; i < 8; ++i)
                trails.push_back(std::make_unique<MWRender::BloodTrail>(*root, ray,
                    [&](const auto& splat) { blood.add("test", splat); }));
            for (int frame = 0; frame < 180; ++frame)
            {
                for (int i = 0; i < 8; ++i)
                    trails[i]->update(.1f, { { { float(i * 10 - 40), 0, 30 }, { 0, 0, -1 }, 1, true } }, 3, frame * .1f);
                blood.update(.1f, 600);
                check(blood.size() == MWRender::BloodDecals::MaxStains
                    && root->getNumChildren() <= MWRender::BloodDecals::MaxStains / MWRender::BloodDecals::ChunkStains + 10, "Full GPU blood cache grew without bound");
                viewer.frame();
                check(glGetError() == GL_NO_ERROR, "Full blood cache caused an OpenGL error");
            }
            glFinish(); trails.clear(); blood.clear();
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
