#include "benchmarkCommon.h"

//-------------------------------------
namespace Benchmark {

    //---------------------------------
    uint32_t
    PremultiplyColor(uint32_t color) {
        const uint32_t alpha  = color >> 24;
        uint32_t       result = alpha << 24;
        for (uint32_t shift = 0; shift < 24; shift += 8) {
            result |= Round255(((color >> shift) & 0xff) * alpha) << shift;
        }

        return result;
    }

} // end of namespace
