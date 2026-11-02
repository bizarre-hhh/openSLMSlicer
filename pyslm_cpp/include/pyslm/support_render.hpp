#pragma once
#include "core.hpp"
#include <optional>
#include <string>

namespace pyslm::support {
struct HeightMap {
    std::size_t width=0,height=0;
    std::vector<double> values;
    bool float32=false;
    double& at(std::size_t x,std::size_t y) { return values.at(y*width+x); }
    double at(std::size_t x,std::size_t y) const { return values.at(y*width+x); }
    bool empty() const noexcept { return values.empty(); }
};
PYSLM_API HeightMap projectHeightMap(const Mesh& mesh,double resolution=0.05,
    bool flipDir=false,std::optional<Bounds3> bbox=std::nullopt);
PYSLM_API std::string openGLRendererInfo();
PYSLM_API HeightMap transposeHeightMap(const HeightMap& input);
PYSLM_API HeightMap flipHeightMapVertical(const HeightMap& input);
PYSLM_API HeightMap padHeightMap(const HeightMap& input,std::size_t border=2,double value=1.0);
// np.gradient(edge_order=1), sqrt(g0*g0+g1*g1), ndimage gaussian_filter
// sigma pixels, truncate=4, mode=reflect. Float operations retain input order.
PYSLM_API HeightMap heightMapGradientMagnitude(const HeightMap& input,double sigma=1.0);
// skimage.measure.find_contours defaults: fully_connected='low',
// positive_orientation='low'. Returned Vec2 is (row, column), not (column,row).
PYSLM_API Paths findHeightMapContours(const HeightMap& image,double level,
    const HeightMap* mask=nullptr);
} // namespace pyslm::support
