// Native DDS/material checks. These scenes do not require Morrowind assets.
#include <apps/openmw/mwrender/bloodsurfacevisitor.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/vfs/filesystemarchive.hpp>
#include <components/vfs/manager.hpp>
#include <osgDB/WriteFile>
#include <cstdlib>
#include <memory>

namespace BloodDDS
{
    using Images = std::vector<osg::ref_ptr<osg::Image>>;
    struct Assets
    {
        VFS::Manager mVfs;
        Resource::ImageManager mImages{&mVfs, 1};
        Images mFloor, mWall;
        Assets()
        {
            mVfs.addArchive(std::make_unique<VFS::FileSystemArchive>(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR)/"files/data"));
            mVfs.buildIndex();
            for (const auto* name : {"floor (6).dds","floor (7).dds","wall (1).dds","wall (2).dds","wall (3).dds","wall (4).dds"})
            {
                auto image=mImages.getImage(VFS::Path::toNormalized(std::string("textures/brutal/blood/")+name));
                require(image && image!=mImages.getWarningImage(),std::string("Uploaded DDS failed to load: ")+name);
                require(image->isCompressed() && image->getPixelFormat()==GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,
                    "DDS did not retain its native DXT5 data through ImageManager");
                require(image->getOrigin()==osg::Image::TOP_LEFT,"Runtime DDS origin convention changed");
                unsigned empty=0,ink=0;
                for (int y=0;y<image->t();y+=11) for (int x=0;x<image->s();x+=11)
                {
                    const auto c=image->getColor(x,y);
                    empty+=c.a()<.01f; ink+=c.a()>.1f;
                }
                require(empty>0 && ink>0,"DXT5 software sampling lost its alpha silhouette");
                (std::string(name).starts_with("floor")?mFloor:mWall).push_back(image);
            }
        }
    };

    struct Scene
    {
        osg::ref_ptr<SceneUtil::LightManager> mRoot=new SceneUtil::LightManager(SceneUtil::LightSettings{});
        osg::ref_ptr<osg::LightModel> mAmbient=new osg::LightModel;
        osgViewer::Viewer& mViewer;
        bool mReversed;
        Scene(osgViewer::Viewer& viewer,bool reversed,bool floor):mViewer(viewer),mReversed(reversed)
        {
            MWRender::installNativeBloodLighting(*mRoot,false,true,0,false);
            mAmbient->setAmbientIntensity({1,1,1,1});
            mRoot->getOrCreateStateSet()->setAttributeAndModes(mAmbient,osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE);
            camera(floor); mViewer.setSceneData(mRoot);
        }
        ~Scene(){mViewer.setSceneData(nullptr);}
        void camera(bool floor,float extent=224)
        {
            auto* c=mViewer.getCamera();
            c->setClearColor({0,0,0,1});
            c->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
            if(floor)c->setViewMatrixAsLookAt({0,0,1500},{0,0,0},{0,1,0});
            else c->setViewMatrixAsLookAt({0,-1500,50},{0,0,50},{0,0,1});
            c->setProjectionMatrixAsOrtho(-extent,extent,-extent*.75,extent*.75,1,5000);
            auto projection=c->getProjectionMatrix();
            if(mReversed)for(int row=0;row<4;++row)projection(row,2)=-projection(row,2);
            mRoot->getOrCreateStateSet()->addUniform(new osg::Uniform("projectionMatrix",osg::Matrixf(projection)));
        }
        std::vector<unsigned char> read(const std::string& name={})
        {
            mViewer.frame();mViewer.frame();glFinish();
            std::vector<unsigned char> pixels(Width*Height*3);
            glReadPixels(0,0,Width,Height,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
            require(glGetError()==GL_NO_ERROR,"DDS scene caused an OpenGL error");
            if(!name.empty())if(const char* out=std::getenv("BRUTAL_TEST_OUTPUT"))
            {
                osg::ref_ptr<osg::Image> screenshot=new osg::Image;
                screenshot->readPixels(0,0,Width,Height,GL_RGB,GL_UNSIGNED_BYTE);
                osgDB::writeImageFile(*screenshot,(std::filesystem::path(out)/(name+(mReversed?"-reversed":"-normal")+".png")).string());
            }
            return pixels;
        }
    };

    MWRender::BloodStamp quad(bool floor,osg::Vec3f center,float radius,unsigned seed,bool pool=false)
    {
        MWRender::BloodStamp stamp;
        stamp.mCenter=center;stamp.mRadius=radius;stamp.mNormal=floor?osg::Vec3f(0,0,1):osg::Vec3f(0,-1,0);
        stamp.mVisualSeed=seed;stamp.mPool=pool;
        const osg::Vec3f right(radius,0,0),up=floor?osg::Vec3f(0,radius,0):osg::Vec3f(0,0,radius);
        stamp.mTriangles={center-right-up,center+right-up,center+right+up,center-right-up,center+right+up,center-right+up};
        return stamp;
    }

    void materials(osgViewer::Viewer& viewer,bool reversed)
    {
        Assets assets;
        for(bool floor:{false,true})
        {
            Scene scene(viewer,reversed,floor);
            MWRender::BloodDecals blood(*scene.mRoot);blood.setTextures(assets.mFloor,assets.mWall);blood.setCellActive("dds",true);
            for(unsigned i=0;i<4;++i)blood.add("dds",quad(floor,{float(i)*100-150,0,floor?0.f:50.f},42,i*4+(i%2)));
            blood.update(.1f,600);
            const auto bright=scene.read(floor?"floor-dds":"wall-dds");
            unsigned pixels=0;
            for(std::size_t i=0;i<bright.size();i+=3)pixels+=bright[i]>bright[i+1]+8&&bright[i]>bright[i+2]+8;
            require(pixels>80,"Uploaded DDS produced no substantial visible blood in the framebuffer");
            scene.mAmbient->setAmbientIntensity({.02f,.02f,.02f,1});
            const auto dark=scene.read(floor?"floor-dark":"wall-dark");
            unsigned long litSum=0,darkSum=0;
            for(std::size_t i=0;i<bright.size();++i){litSum+=bright[i];darkSum+=dark[i];}
            require(darkSum<litSum/8,"DDS blood retained a fixed brightness in a dark room");
            scene.mAmbient->setAmbientIntensity({0,0,0,1});
            const auto black=scene.read();
            require(std::all_of(black.begin(),black.end(),[](auto v){return v<=1;}),"DDS blood glowed without any scene light");
        }
        // Pool output must be identical with or without the supplied artwork.
        Scene scene(viewer,reversed,true);
        std::vector<unsigned char> reference;
        for(bool textured:{false,true})
        {
            MWRender::BloodDecals blood(*scene.mRoot);
            if(textured)blood.setTextures(assets.mFloor,assets.mWall);
            blood.setCellActive("pool",true);
            auto pool=quad(true,{},40,8,true);pool.mTriangles.clear();
            for(int i=0;i<16;++i)
            {
                const float a=i*6.2831853f/16,b=(i+1)*6.2831853f/16;
                pool.mTriangles.insert(pool.mTriangles.end(),{osg::Vec3f(),osg::Vec3f(std::cos(a)*36,std::sin(a)*36,0),osg::Vec3f(std::cos(b)*36,std::sin(b)*36,0)});
            }
            blood.add("pool",pool);blood.update(.1f,600);
            const auto image=scene.read(textured?"stable-pool":"");
            if(!textured)reference=image;
            else require(image==reference,"DDS textures changed the stable untextured pool material");
        }
        std::cout<<"PASS six uploaded DXT5 files, native ImageManager, visible floor/wall DDS, darkness and unchanged untextured pools\n";
    }

    void spray(osgViewer::Viewer& viewer,bool reversed)
    {
        Assets assets;
        const std::vector<osg::Vec3f> floor{{-3000,-3000,0},{3000,-3000,0},{3000,3000,0},{-3000,-3000,0},{3000,3000,0},{-3000,3000,0}};
        const auto surface=MWRender::BloodTrail::collisionSurface(floor);
        std::array<float,2> reach{};
        for(int quantity=0;quantity<2;++quantity)
        {
            Scene scene(viewer,reversed,true);scene.camera(true,1100);
            MWRender::BloodDecals blood(*scene.mRoot);blood.setTextures(assets.mFloor,assets.mWall);blood.setCellActive("physics",true);
            unsigned splashes=0;
            MWRender::BloodTrail trail(*scene.mRoot,surface.mRaycast,[&](const auto& stamp){
                if(stamp.mPool)return;
                ++splashes;reach[quantity]=std::max(reach[quantity],stamp.mCenter.length());blood.add("physics",stamp);
            },surface.mProjector);
            const float amount=quantity?3.f:1.f;
            for(unsigned frame=0;frame<420;++frame)
            {
                const auto wounds=frame<180?std::vector<MWRender::BloodTrail::Source>{{{0,0,30},{1,0,1},1,true,true}}:std::vector<MWRender::BloodTrail::Source>{};
                trail.update(1.f/60,wounds,amount,frame/60.f);blood.update(1.f/60,600);
                require(trail.particleCount()<=MWRender::BloodTrail::MaxParticles,"Sprite budget was exceeded");
            }
            require(trail.particleCount()==0&&splashes>50,"Flying drops vanished without leaving persistent impact splatters");
            const auto image=scene.read(quantity?"outdoor-300":"outdoor-100");
            unsigned visible=0;for(std::size_t i=0;i<image.size();i+=3)visible+=image[i]>image[i+1]+3&&image[i]>image[i+2]+3;
            require(visible>10,"Collision-backed exterior droplets left no visible uploaded DDS splatters");
            std::cout<<"Outdoor amount="<<amount<<" splashes="<<splashes<<" distance="<<reach[quantity]<<" pixels="<<visible<<'\n';
        }
        require(reach[1]>reach[0]*1.3f,"300% blood increased concentration without increasing throw distance");
        std::cout<<"PASS stable collision receivers, swept sprite impacts and larger outdoor spread at 300%\n";
    }
}
