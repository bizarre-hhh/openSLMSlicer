#ifndef SUPPORT_TYPES_H
#define SUPPORT_TYPES_H

#include "model_transform.h"
#include "slice_engine.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Automatic support generation shared types.
//
// A support group is derived from one STL model and always belongs to that
// model. The generated triangles are stored in local build coordinates:
//   X/Y are relative to the owner model's world XY center at generation time,
//   Z is the absolute build-plate height in millimeters.
// The viewport resolves local triangles to world coordinates with the owner's
// current world position, so translating a model in X/Y moves its supports
// with it. Rotating/scaling it or changing its Z position deletes them.

using SupportId = std::uint64_t;

// Support parts use their own id range so they never collide with model ids
// (models start at 1) and stay below 2^53 for .slmslice JSON numbers.
inline constexpr SupportId kSupportIdBase = 1000000000ull;

inline constexpr double kSupportOverhangAngleDeg = 45.0;
inline constexpr std::size_t kMaxSupportTrianglesPerModel = 500000;

enum class SupportState : std::uint8_t
{
    None = 0,      // no entry (never generated or deleted)
    Generated = 1, // supports exist for the model
    NotNeeded = 2  // generated and determined that the model needs no support
};

struct SupportGroup
{
    SupportId id = 0;
    std::uint64_t owner_model_id = 0;
    std::string name; // display name used for slice parts / export
    SupportState state = SupportState::None;
    double overhang_angle_deg = kSupportOverhangAngleDeg;
    double volume_mm3 = 0.0;
    // Local build coordinates (see file header). Empty for SupportState::NotNeeded.
    std::vector<SliceTriangle> local_triangles;
    // Model transform snapshot used to invalidate supports after editing.
    // Only rotation/scale are compared; X/Y translation is allowed because the
    // geometry follows the owner model. Z position uses the world center Z
    // snapshot below (local translation Z is not equivalent after rotation).
    ModelTransform source_transform;
    // World bbox-center Z of the owner at generation time. A change means the
    // model moved vertically and its supports are no longer valid.
    double source_world_center_z_mm = 0.0;
};

using SupportProgressCallback = std::function<bool(std::size_t completed, std::size_t total)>;

#endif
