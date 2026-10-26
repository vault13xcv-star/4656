#ifndef OPENMW_ESM3_BRUTALBLOODSTATE_HPP
#define OPENMW_ESM3_BRUTALBLOODSTATE_HPP
#include <array>
#include <cstddef>
#include <cstdint>
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
        std::uint32_t mVisualSeed = 0;
        std::array<float, 3> mCenter{}, mNormal{};
        std::vector<std::array<float, 3>> mTriangles;
    };
    struct BrutalBloodState
    {
        static constexpr std::size_t MaxStains = 32768;
        static constexpr std::size_t MaxVerticesPerStain = 192;
        // Preserve preview.2's maximum possible geometry allocation even with
        // four times as many normal (24-vertex) projected texture patches.
        static constexpr std::size_t MaxVertices = 8192 * MaxVerticesPerStain;
        std::vector<BrutalBloodStain> mStains;
        void load(ESMReader& reader);
        void save(ESMWriter& writer) const;
    };
}
#endif
