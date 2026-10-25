#ifndef OPENMW_MWRENDER_BLOODCOVERAGE_HPP
#define OPENMW_MWRENDER_BLOODCOVERAGE_HPP

#include <osg/Vec3f>

#include <cstddef>
#include <vector>

namespace MWRender
{
    struct BloodStamp;

    // Retain only incoming receiver geometry that is not already covered by
    // two distinct stains. Stains may contain many tessellated receiver faces;
    // a face in another plane never consumes this receiver's layer budget.
    // Work and output are bounded. Exhausting the work budget rejects the whole
    // update rather than admitting unchecked overdraw or shrinking a live pool.
    // Optional completion status distinguishes proven full coverage (an empty
    // result with true) from a budget/validation rejection (false).
    std::vector<osg::Vec3f> clipBloodCoverage(const BloodStamp& incoming,
        const std::vector<const BloodStamp*>& existing, std::size_t maxVertices = 192,
        std::size_t maxWork = 32768, bool* completed = nullptr);
}

#endif
