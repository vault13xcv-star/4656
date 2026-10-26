#ifndef BRUTAL_GORE_TEXTURE_TESTS_HPP
#define BRUTAL_GORE_TEXTURE_TESTS_HPP
#include <apps/openmw/mwrender/brutaltextures.hpp>
#include <apps/openmw/mwrender/brutalgore.hpp>
#include <apps/openmw/mwrender/ragdollappearance.hpp>
#include <osgDB/ReadFile>
#include <filesystem>

namespace BrutalTextureTests
{
    inline void check(bool condition,const char* message)
    {if(!condition)throw std::runtime_error(message);}

    inline osg::ref_ptr<osg::Geometry> box(float from,float to)
    {
        osg::ref_ptr<osg::Geometry> mesh=new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> positions=new osg::Vec3Array;
        for(unsigned axis=0;axis<3;++axis)for(int side:{-1,1})
        {
            std::array<osg::Vec3f,4> corners;
            for(unsigned i=0;i<4;++i)
            {
                osg::Vec3f p;p[axis]=float(side);
                p[(axis+1)%3]=(i==1||i==2)?1.f:-1.f;p[(axis+2)%3]=i>=2?1.f:-1.f;
                p.x()*=10;p.y()*=12;p.z()=from+(p.z()+1)*.5f*(to-from);corners[i]=p;
            }
            for(unsigned i:{0u,1u,2u,0u,2u,3u})positions->push_back(corners[i]);
        }
        mesh->setVertexArray(positions);mesh->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,positions->size()));
        return mesh;
    }

    inline void run()
    {
        MWRender::BrutalTextures::Images images;
        unsigned index=0;
        for(const char* name:{"burnt.dds","poisoned.dds","cut_limb.dds","cut_body_torso.dds","sliced_body.dds"})
        {
            const auto path=std::filesystem::path(BRUTAL_SOURCE_DIR)/"files/data/textures/brutal/gore"/name;
            images[index]=osgDB::readImageFile(path.string());
            check(images[index] && images[index]->data(),"An uploaded gore DDS did not load");++index;
        }
        osg::ref_ptr<osg::Group> root=new osg::Group;
        MWRender::installBrutalTextures(*root,images);
        auto* assets=MWRender::findBrutalTextures(*root);
        check(assets,"Shared gore textures are not attached to the scene");
        for(unsigned kind:{unsigned(MWRender::BrutalTextures::Limb),unsigned(MWRender::BrutalTextures::Slice)})
        {
            check(assets->mMasks[kind].valid(),"Opaque DDS backdrop has no exterior mask");
            const auto* mask=assets->mMasks[kind]->getImage();
            check(mask->data()[0]==0,"Opaque square DDS backdrop was retained");
            if(kind==MWRender::BrutalTextures::Limb)
                check(mask->data()[mask->t()/2*mask->s()+mask->s()/2]==255,"Backdrop masking erased the central bone");
        }
        for(const auto& image:assets->mCoatings)
        {
            check(image.valid(),"An uploaded corpse coating was not prepared");
            unsigned different=0;
            for(int i=0;i<image->s();++i)
            {
                check((image->getColor(0,i)-image->getColor(image->s()-1,i)).length()<.007f,
                    "A corpse coating has a horizontal repeat seam");
                check((image->getColor(i,0)-image->getColor(i,image->t()-1)).length()<.007f,
                    "A corpse coating has a vertical repeat seam");
                different+=(image->getColor(i,80)-image->getColor((i+image->s()/4)%image->s(),80)).length()>.03f;
            }
            check(different>image->s()/3,"Stochastic coating repeats identical quarter-sized tiles");
        }
        for(auto element:{Ragdoll::DeathElement::Fire,Ragdoll::DeathElement::Poison})
        {
            osg::ref_ptr<osg::Geode> skin=new osg::Geode;
            osg::ref_ptr<osg::Image> original=new osg::Image;original->allocateImage(32,32,1,GL_RGBA,GL_UNSIGNED_BYTE);
            for(int y=0;y<32;++y)for(int x=0;x<32;++x)original->setColor({.8f,.3f,.1f,.6f},x,y);
            osg::ref_ptr<osg::Texture2D> texture=new osg::Texture2D(original);
            skin->getOrCreateStateSet()->setTextureAttributeAndModes(0,texture,osg::StateAttribute::ON);
            root->addChild(skin);
            {
                MWRender::RagdollAppearance effect(*root,element,{});
                effect.update(2,{skin},{});
                const auto* painted=dynamic_cast<const osg::Texture2D*>(root->getChild(0)->getStateSet()->getTextureAttribute(0,osg::StateAttribute::TEXTURE));
                check(painted && painted!=texture,"Uploaded corpse texture was never applied");
                const auto* coating=assets->mCoatings[element==Ragdoll::DeathElement::Fire?0:1].get();
                const auto actual=painted->getImage()->getColor(0,0),expected=coating->getColor(0,0);
                check(std::abs(actual.r()-expected.r())<.005f && std::abs(actual.g()-expected.g())<.005f,
                    "Corpse uses a procedural placeholder instead of its uploaded coating");
                check(std::abs(actual.a()-.6f)<.005f,"Corpse coating destroyed the original clothing alpha");
                check(original->getColor(0,0).r()>.79f,"Corpse coating mutated a shared original image");
            }
            check(root->getChild(0)==skin,"Corpse material cleanup did not restore the original skin");
            root->removeChild(skin);
        }
        for(float radius:{.5f,2.f,30.f})for(bool torso:{false,true})
        {
            osg::ref_ptr<osg::Cylinder> shape=new osg::Cylinder({},radius,.8f);
            const auto mesh=MWRender::makeWoundGeometry(*shape,torso,assets);
            const auto* p=static_cast<const osg::Vec3Array*>(mesh->getVertexArray());
            const auto* uv=static_cast<const osg::Vec2Array*>(mesh->getTexCoordArray(0));
            check(p->size()>100 && p->size()==uv->size(),"Cut cap geometry or UV channel is missing");
            for(unsigned i=0;i<p->size();++i)
            {
                const float x=(*p)[i].x()/radius,y=(*p)[i].y()/radius/(torso?.7f:1.f);
                check(x*x+y*y<1.001f,"A cut cap leaked a rectangular background outside its ellipse");
                check((*uv)[i].x()>=0 && (*uv)[i].x()<=1 && (*uv)[i].y()>=0 && (*uv)[i].y()<=1,
                    "A wound image repeats or samples outside its intended atlas region");
                if(!torso)
                {
                    const auto q=(*uv)[i]-osg::Vec2f(.5f,.5f);
                    check(q.length2()<.251f,"Limb UV corners sample the opaque square backdrop");
                }
            }
        }
        for(float angle:{0.f,1.2f})for(const auto& band:std::array<osg::Vec2f,3>{{{0,25},{25,75},{75,100}}})
        {
            const auto matrix=osg::Matrixf::rotate(angle,osg::Vec3f(0,0,1))*osg::Matrixf::translate(200,-150,30);
            osg::ref_ptr<osg::MatrixTransform> object=new osg::MatrixTransform(matrix);
            const auto mesh=box(band.x(),band.y());object->addChild(mesh);
            MWRender::GoreSliceMapping mapping;
            mapping.mOrigin=osg::Vec3f()*matrix;mapping.mForward=osg::Matrixf::transform3x3(osg::Vec3f(0,1,0),matrix);
            mapping.mUp={0,0,1};mapping.mHeight=100;mapping.mTextures=assets;mapping.mLightingRoot=root;
            const auto halves=MWRender::splitGoreMesh(*mesh,mapping.mOrigin,
                osg::Matrixf::transform3x3(osg::Vec3f(1,0,0),matrix),&mapping);
            for(unsigned side=0;side<2;++side)
            {
                struct Inspect:osg::NodeVisitor
                {
                    osg::Geometry* cap=nullptr;
                    Inspect():osg::NodeVisitor(TRAVERSE_ALL_CHILDREN){}
                    void apply(osg::Geode& geode)override
                    {if(geode.getName()=="Brutal Morrowind textured longitudinal cut")cap=geode.getDrawable(0)->asGeometry();traverse(geode);}
                } inspect;
                halves[side]->accept(inspect);check(inspect.cap,"Longitudinal cut has no uploaded texture cap");
                const auto* p=static_cast<const osg::Vec3Array*>(inspect.cap->getVertexArray());
                const auto* uv=static_cast<const osg::Vec2Array*>(inspect.cap->getTexCoordArray(0));
                check(!p->empty(),"Anatomical cap lost its real cross-section");
                float front=0,back=0;unsigned frontCount=0,backCount=0;
                for(unsigned i=0;i<p->size();++i)
                {
                    const auto local=(*p)[i]*osg::Matrixf::inverse(matrix);
                    const auto tex=(*uv)[i];
                    check(tex.x()>=side*.5f && tex.x()<=(side+1)*.5f,"Longitudinal side sampled the other half of the atlas");
                    check(std::abs(tex.y()-std::clamp(local.z()/100,.005f,.995f))<.001f,
                        "Head/torso/pelvis cuts each repeated the entire anatomical image");
                    if(local.y()>11){front+=tex.x();++frontCount;}
                    if(local.y()<-11){back+=tex.x();++backCount;}
                }
                check(frontCount && backCount,"Slice fixture lost its front/back boundaries");
                check(side?front/frontCount>back/backCount:front/frontCount<back/backCount,
                    "Anatomical texture put the spine on the front of the NPC");
            }
        }
        {
            const auto sheet=box(20,20);
            MWRender::GoreSliceMapping mapping;mapping.mTextures=assets;mapping.mHeight=100;mapping.mLightingRoot=root;
            const auto halves=MWRender::splitGoreMesh(*sheet,{0,0,20},{1,0,0},&mapping);
            for(const auto& half:halves)for(unsigned i=0;i<half->getNumChildren();++i)
                check(half->getChild(i)->getName()!="Brutal Morrowind textured longitudinal cut",
                    "A zero-thickness decorative mesh generated an invalid anatomical cap");
        }
        std::cout<<"PASS five uploaded DDS materials, periodic nonrepeating coatings, alpha/instance isolation, scaled cut caps and posed anatomical left/right atlas mapping\n";
    }
}
#endif
