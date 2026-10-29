#ifndef SUPPORT_ENGINE_H
#define SUPPORT_ENGINE_H

#include "support_types.h"

#include <string>
#include <vector>

// Pure C++ support generation layer built on top of pyslm::support.
//
// The engine works on build-plate millimeter triangles (the same data the slice
// engine consumes). It generates one solid block support volume per model using
// pyslm's projection/ray-casting support regions (supportVolume), not the
// unstable truss mesh path. The caller owns model iteration, progress,
// cancellation, persistence and rendering.
struct SupportModelInput
{
    std::string name;
    std::vector<SliceTriangle> world_triangles_mm;
    // Owner model world XY center, used to store supports in local coordinates.
    double model_center_x_mm = 0.0;
    double model_center_y_mm = 0.0;
};

struct SupportGenerationOutput
{
    SupportState state = SupportState::NotNeeded;
    std::vector<SliceTriangle> local_triangles;
    double volume_mm3 = 0.0;
    std::size_t triangle_count = 0;
};

// Generates support geometry for one model.
//
// Returns false when the library or the geometry fails (error receives a
// UTF-8 message). A successful call with state == SupportState::NotNeeded means
// the model does not need supports; state == SupportState::Generated means
// local_triangles is non-empty.
//
// pyslm's support region identification uses a hidden OpenGL context internally
// on first use, so call this from a process with a usable desktop OpenGL
// environment.
bool GenerateSupportForModel(const SupportModelInput& input,
                             SupportGenerationOutput& output,
                             std::string& error);

// Coordinate helpers shared by the viewport and tests.
std::vector<SliceTriangle> SupportLocalTriangles(
    const std::vector<SliceTriangle>& world_triangles,
    double center_x_mm, double center_y_mm);
std::vector<SliceTriangle> SupportWorldTriangles(
    const std::vector<SliceTriangle>& local_triangles,
    double center_x_mm, double center_y_mm);

// Absolute mesh volume of a closed triangle soup (mm^3). Returns 0 for empty or
// non-finite input.
double SupportMeshVolumeMm3(const std::vector<SliceTriangle>& triangles);

#endif
