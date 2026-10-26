#ifndef OPENMW_MWRENDER_BRUTALTEXTURES_HPP
#define OPENMW_MWRENDER_BRUTALTEXTURES_HPP

#include <osg/Group>
#include <osg/Image>
#include <osg/Texture2D>
#include <osg/UserDataContainer>
#include <array>
#include <vector>

namespace MWRender
{
    // Immutable assets belong to the scene, never to an individual corpse.
    class BrutalTextures : public osg::Object
    {
    public:
        enum Kind { Burnt, Poisoned, Limb, Torso, Slice, Count };
        using Images = std::array<osg::ref_ptr<osg::Image>, Count>;
        BrutalTextures() = default;
        BrutalTextures(const BrutalTextures& rhs, const osg::CopyOp& copy)
            : osg::Object(rhs, copy), mImages(rhs.mImages), mTextures(rhs.mTextures), mMasks(rhs.mMasks)
            , mCoatings(rhs.mCoatings), mLimbRim(rhs.mLimbRim), mSliceRows(rhs.mSliceRows) {}
        META_Object(MWRender, BrutalTextures)
        Images mImages;
        std::array<osg::ref_ptr<osg::Texture2D>, Count> mTextures;
        std::array<osg::ref_ptr<osg::Texture2D>, Count> mMasks;
        std::array<osg::ref_ptr<osg::Image>, 2> mCoatings;
        std::array<osg::Vec2f, 64> mLimbRim;
        // Canonical bottom-to-top rows, with separate left/right atlas bounds.
        std::array<std::vector<osg::Vec2f>, 2> mSliceRows;
    };

    inline BrutalTextures* findBrutalTextures(osg::Node& root)
    {
        auto paths = root.getParentalNodePaths();
        if (paths.empty()) paths.push_back({&root});
        for (const auto& path : paths)
            for (auto it=path.rbegin();it!=path.rend();++it)
                if (auto* data=(*it)->getUserDataContainer())
                    for (unsigned i=0;i<data->getNumUserObjects();++i)
                        if (auto* assets=dynamic_cast<BrutalTextures*>(data->getUserObject(i))) return assets;
        return nullptr;
    }

    void installBrutalTextures(osg::Group& root, const BrutalTextures::Images& images);
}
#endif
