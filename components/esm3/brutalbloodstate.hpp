#ifndef OPENMW_ESM3_BRUTALBLOODSTATE_HPP
#define OPENMW_ESM3_BRUTALBLOODSTATE_HPP
#include <array>
#include <string>
#include <vector>
namespace ESM
{
    class ESMReader;
    class ESMWriter;
    struct BrutalBloodStain
    {
        std::string mCell;
        float mAge = 0, mRadius = 1;
        bool mPool = false;
        std::array<float, 3> mCenter{}, mNormal{};
        std::vector<std::array<float, 3>> mTriangles;
    };
    struct BrutalBloodState
    {
        std::vector<BrutalBloodStain> mStains;
        void load(ESMReader& reader);
        void save(ESMWriter& writer) const;
    };
}
#endif
