#pragma once
#include "types.hpp"
#include <optional>
namespace pyslm {
struct SplineCurve {
    Path control;
    std::vector<double> knots;
    int degree=3;
    bool closed=false;
    int fitStatus=0;
};
// Trimesh points_to_spline_entity with expanded entity control indices:
// closed input makes both end controls equal to their original mean.
PYSLM_API SplineCurve fitSpline(const Path& points,double smooth=0.002);
PYSLM_API Path discretizeSpline(const SplineCurve& curve,double scale=1.0,
                                std::optional<std::size_t> count=std::nullopt);
PYSLM_API std::optional<Path> fitClosedCircle(const Path& points,double scale);
PYSLM_API Path discretizeClosedCircle(const Path& controls,double scale=1.0);
}
