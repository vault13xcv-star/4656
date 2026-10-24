// Optional Linux/EGL regression for the real OpenMW projection/depth contract.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <apps/openmw/mwrender/ragdollappearance.hpp>
#include <apps/openmw/mwrender/brutalblood.hpp>
#include <array>
#include <components/sceneutil/depth.hpp>
#include <iostream>
#include <osg/AlphaFunc>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Program>
#include <osg/ShapeDrawable>
#include <osg/Shader>
#include <osgViewer/GraphicsWindow>
#include <osgViewer/Viewer>
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
            blood.update(600, 600); check(read() == baseline, "Expired blood left a visible overlay");
            std::cout << "PASS actual GL blood decals, native projection, inherited alpha/shader, opaque occlusion and expiry\n";
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
