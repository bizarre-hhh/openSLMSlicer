#include <cstddef>
#include <numeric>
#include <vector>
#include "../third_party/x86-simd-sort/src/x86simdsort-static-incl.h"

namespace pyslm::hatching {
// Compile this translation unit with /arch:AVX2 (MSVC) or -mavx2 -mpopcnt
// (GCC/Clang), matching the pinned NumPy X86_V3 sorting dispatch.
std::vector<std::size_t> numpyArgsortAvx2(const std::vector<double>& values) {
    std::vector<std::size_t> indices(values.size());
    std::iota(indices.begin(),indices.end(),0);
    if(!indices.empty())x86simdsortStatic::argsort(values.data(),indices.data(),values.size(),true);
    return indices;
}
} // namespace pyslm::hatching
