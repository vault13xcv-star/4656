// Optional Linux/EGL regression for the real OpenMW projection/depth contract.
#include <apps/openmw/mwrender/ragdollappearance.hpp>
#include <components/sceneutil/depth.hpp>
#include <osg/AlphaFunc>
#include <osg/Geometry>
#include <osg/Geode>
#include <osg/Group>
#include <osg/Program>
#include <osg/Shader>
#include <osgViewer/Viewer>
#include <osgViewer/GraphicsWindow>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
    void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
    constexpr int width = 320, height = 240;
}
int main(int argc, char**)
{
    try
    {
        const bool reversed = argc > 1;
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
        MWRender::RagdollAppearance appearance(*root, Ragdoll::DeathElement::Fire, {});
        const std::array<osg::Vec3f, 4> positions{ osg::Vec3f(-30,0,20), osg::Vec3f(-30,0,45),
            osg::Vec3f(30,0,20), osg::Vec3f(30,0,45) };
        std::vector<unsigned char> burning, finished;
        for (const float time : { 2.f, 5.f })
        {
            appearance.update(time, {}, positions);
            viewer.frame(time); glFinish();
            auto& pixels = time == 2 ? burning : finished;
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
        check(!appearance.hasFire(), "Fire did not end after five seconds");
        std::cout << "PASS real GL " << (reversed ? "reversed" : "normal")
                  << " projection, " << visible << " flame/smoke pixels, opaque occlusion and five second timer\n";
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
