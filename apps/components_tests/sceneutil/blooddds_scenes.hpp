// Native DDS/material checks. These scenes do not require Morrowind assets.
#include <apps/openmw/mwrender/bloodsurfacevisitor.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/vfs/filesystemarchive.hpp>
#include <components/vfs/manager.hpp>
#include <osgDB/WriteFile>
#include <cstdlib>
#include <memory>
#include <chrono>

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
            for (const auto* name : {"floor (1).dds","floor (2).dds","floor (3).dds","wall (1).dds","wall (2).dds","walll.dds"})
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
            require(mFloor.size()==3 && mWall.size()==3,"Uploaded wall/floor variant count is not three each");
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

    void gore(osgViewer::Viewer& viewer,bool reversed)
    {
        Assets original;
        MWRender::BrutalTextures::Images images;
        unsigned imageIndex=0;
        for(const char* name:{"burnt.dds","poisoned.dds","cut_limb.dds","cut_body_torso.dds","sliced_body.dds"})
        {
            auto image=original.mImages.getImage(VFS::Path::toNormalized(std::string("textures/brutal/gore/")+name));
            require(image && image!=original.mImages.getWarningImage(),std::string("Gore DDS unavailable: ")+name);
            images[imageIndex++]=image;
        }
        Scene scene(viewer,reversed,false);
        MWRender::installBrutalTextures(*scene.mRoot,images);
        auto* textures=MWRender::findBrutalTextures(*scene.mRoot);
        const auto add=[&](osg::Geometry* mesh,osg::Texture2D& texture) {
            osg::ref_ptr<osg::Geode> geode=new osg::Geode;
            geode->setStateSet(MWRender::createTexturedGoreState(texture));
            geode->addDrawable(mesh);scene.mRoot->addChild(geode);
            MWRender::attachBloodLighting(*geode,*scene.mRoot);
            return geode;
        };
        for(unsigned kind=0;kind<2;++kind)
        {
            osg::ref_ptr<osg::Cylinder> shape=new osg::Cylinder({kind?-105.f:-175.f,0,50},29,.8f);
            shape->setRotation(osg::Quat(osg::PI_2,osg::Vec3f(1,0,0)));
            add(MWRender::makeWoundGeometry(*shape,kind!=0,textures),
                *textures->mTextures[kind?MWRender::BrutalTextures::Torso:MWRender::BrutalTextures::Limb]);
        }
        std::vector<std::unique_ptr<MWRender::RagdollAppearance>> effects;
        for(unsigned kind=0;kind<2;++kind)
        {
            osg::ref_ptr<osg::Image> skin=new osg::Image;skin->allocateImage(64,64,1,GL_RGBA,GL_UNSIGNED_BYTE);
            for(int y=0;y<64;++y)for(int x=0;x<64;++x)skin->setColor({.7f,.4f,.2f,1},x,y);
            osg::ref_ptr<osg::Texture2D> texture=new osg::Texture2D(skin);
            osg::ref_ptr<osg::Cylinder> shape=new osg::Cylinder({kind?35.f:-35.f,0,50},27,.8f);
            shape->setRotation(osg::Quat(osg::PI_2,osg::Vec3f(1,0,0)));
            auto node=add(MWRender::makeWoundGeometry(*shape,false,textures),*texture);
            node->getStateSet()->setTextureAttributeAndModes(0,texture,osg::StateAttribute::ON);
            auto effect=std::make_unique<MWRender::RagdollAppearance>(*scene.mRoot,
                kind?Ragdoll::DeathElement::Poison:Ragdoll::DeathElement::Fire,MWRender::RagdollAppearance::Settings{});
            effect->update(2,{node},{});effects.push_back(std::move(effect));
        }
        for(unsigned side=0;side<2;++side)
        {
            const float x=side?175.f:105.f;
            osg::ref_ptr<osg::Box> shape=new osg::Box({x,0,50},34,20,90);
            osg::ref_ptr<osg::Geode> source=new osg::Geode;
            source->addDrawable(MWRender::makeWoundGeometry(*shape,false,textures));
            MWRender::GoreSliceMapping mapping;
            mapping.mOrigin={x,0,5};mapping.mForward={1,0,0};mapping.mHeight=90;
            mapping.mTextures=textures;mapping.mLightingRoot=scene.mRoot;
            const auto halves=MWRender::splitGoreMesh(*source,{x,0,50},{0,1,0},&mapping);
            unsigned caps=0;
            for(unsigned i=0;i<halves[side]->getNumChildren();++i)
            {
                auto* node=halves[side]->getChild(i);
                if(node->getName()=="Brutal Morrowind textured longitudinal cut")
                {scene.mRoot->addChild(node);++caps;}
            }
            require(caps==1,"Longitudinal uploaded-material fixture has no physical cap");
        }
        const auto energy=[](const std::vector<unsigned char>& pixels) {
            unsigned long sum=0;for(auto value:pixels)sum+=value;return sum;
        };
        const auto bright=scene.read("gore-materials");
        require(energy(bright)>30000,"Uploaded wound and corpse textures did not render");
        scene.mAmbient->setAmbientIntensity({.02f,.02f,.02f,1});
        require(energy(scene.read("gore-dark"))<energy(bright)/8,"New gore textures remained bright in darkness");
        scene.mAmbient->setAmbientIntensity({0,0,0,1});
        const auto black=scene.read();
        require(std::all_of(black.begin(),black.end(),[](auto value){return value<=1;}),"New gore textures emitted light without a light source");
        osg::ref_ptr<osg::Light> sun=new osg::Light;
        sun->setPosition({0,-1,0,0});sun->setDiffuse({1,1,1,1});scene.mRoot->setSunlight(sun);
        require(energy(scene.read("gore-sun"))>energy(bright)/3,"New gore textures did not receive real sunlight");
        sun->setDiffuse({0,0,0,1});
        osg::ref_ptr<osg::Light> torch=new osg::Light;
        torch->setDiffuse({1,.65f,.35f,1});torch->setAmbient({0,0,0,1});
        torch->setConstantAttenuation(1);torch->setLinearAttenuation(.001f);torch->setQuadraticAttenuation(0);
        osg::ref_ptr<SceneUtil::LightSource> light=new SceneUtil::LightSource;
        light->setLight(torch);light->setRadius(600);
        osg::ref_ptr<osg::MatrixTransform> transform=new osg::MatrixTransform(osg::Matrixf::translate(0,-60,50));
        transform->addChild(light);scene.mRoot->addChild(transform);
        require(energy(scene.read("gore-torch"))>energy(bright)/8,"New gore textures did not receive a real point light");
        light->setEmpty(true);
        require(energy(scene.read())<100,"New gore textures retained an extinguished torch's light");
        std::cout<<"PASS five actual uploaded gore/corpse DDS, physical caps, ambient/dark/sun/torch lighting and no emission\n";
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
            osg::ref_ptr<osg::Light> sun=new osg::Light;
            sun->setPosition(floor?osg::Vec4f(0,0,1,0):osg::Vec4f(0,-1,0,0));
            sun->setDiffuse({1,1,1,1});scene.mRoot->setSunlight(sun);
            const auto sunny=scene.read(floor?"floor-sun":"wall-sun");
            unsigned long ambientRed=0,sunnyRed=0;
            for (std::size_t i=0;i<bright.size();i+=3){ambientRed+=bright[i];sunnyRed+=sunny[i];}
            require(sunnyRed<ambientRed*.8 && sunnyRed>ambientRed*.4,"Direct sunshine still washes blood out instead of darkening its material response");
            sun->setDiffuse({0,0,0,1});
            scene.mAmbient->setAmbientIntensity({.02f,.02f,.02f,1});
            const auto dark=scene.read(floor?"floor-dark":"wall-dark");
            unsigned long litSum=0,darkSum=0;
            for(std::size_t i=0;i<bright.size();++i){litSum+=bright[i];darkSum+=dark[i];}
            require(darkSum<litSum/8,"DDS blood retained a fixed brightness in a dark room");
            scene.mAmbient->setAmbientIntensity({0,0,0,1});
            const auto black=scene.read();
            require(std::all_of(black.begin(),black.end(),[](auto v){return v<=1;}),"DDS blood glowed without any scene light");
        }
        // Legacy procedural pool geometry must never reach the renderer.
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
            require(blood.size()==0,"Procedural blood pool geometry is still rendered");
            const auto image=scene.read(textured?"stable-pool":"");
            if(!textured)reference=image;
            else require(image==reference,"DDS textures changed the stable untextured pool material");
        }
        std::cout<<"PASS six uploaded DXT5 files, native ImageManager, visible floor/wall DDS, darkness and removed legacy untextured pools\n";
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

    void densityBenchmark(osgViewer::Viewer& viewer, bool reversed, bool verify=false)
    {
        Assets assets;
        for (unsigned count : {64u, 512u, 2048u})
        {
            Scene scene(viewer,reversed,true); scene.camera(true,48);
            MWRender::BloodDecals blood(*scene.mRoot);
            blood.setTextures(assets.mFloor,assets.mWall); blood.setCellActive("density",true);
            const auto start=std::chrono::steady_clock::now();
            for (unsigned i=0;i<count;++i)
            {
                const float x=float((i*17)%41)*.15f-3, y=float((i*13)%37)*.15f-3;
                blood.add("density",quad(true,{x,y,0},20+(i%5),i*6151+8));
            }
            blood.update(.1f,600);
            const double addMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            scene.read();
            const auto render=std::chrono::steady_clock::now();
            for (int i=0;i<5;++i) {viewer.frame(); glFinish();}
            const double frameMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-render).count()/5;
            std::cout<<"DENSITY incoming="<<count<<" retained="<<blood.size()<<" add_ms="<<addMs<<" render_ms="<<frameMs<<std::endl;
            if (verify) require(blood.size()<80,"Overlapping uploaded DDS splatters caused unbounded local overdraw");
        }
    }
}
