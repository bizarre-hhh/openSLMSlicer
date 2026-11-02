#pragma once

#include "core.hpp"
#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace pyslm::visualise {

enum class PlotPrimitiveKind { Segments,Polyline,Points,Polygons,Arrow };
struct PlotPrimitive {
    PlotPrimitiveKind kind=PlotPrimitiveKind::Polyline;
    std::vector<Vec3> points;
    std::vector<std::array<Vec3,2>> segments;
    std::vector<std::vector<Vec3>> polygons;
    std::array<Vec3,2> arrow{}; // annotation start (xytext), end (xy)
    std::vector<double> colorValues;
    std::string color,fillColor,colormap;
    double lineWidth=0.7,pointSize=1.0;
    bool filled=false,dashed=false,orderLine=false,jump=false;
};
struct PlotScene {
    bool plot3D=false,equalAspect=true;
    // Order of actual artist creation; grouped geometry order is retained.
    std::vector<PlotPrimitive> primitives;
    std::vector<std::size_t> colorbarCollections;
    std::size_t defaultColorIndex=0;
};
using IndexCallback=std::function<std::vector<double>(const LayerGeometry&)>;
struct PlotOptions {
    bool plotContours=true,plotHatches=true,plotPoints=true;
    bool plot3D=false,plotArrows=false,plotOrderLine=false,plotColorbar=false;
    std::string index;
    IndexCallback indexCallback;
};
struct SequentialPlotOptions { bool plotArrows=false,plotOrderLine=false,plotJumps=false; };
struct PolygonPlotOptions {
    double zPos=0.0,lineWidth=0.7;
    std::string lineColor="k",fillColor="r";
    bool plot3D=false,plotFilled=false;
};
struct HeatMap {
    std::size_t width=0,height=0;
    std::vector<double> values; // row-major, image origin lower
    double resolution=0.25;
    Vec2 coordinateOffset; // subtracted in-place from input exposure XY
    std::string colormap="hot",interpolation="nearest";
    double at(std::size_t x,std::size_t y) const { return values.at(y*width+x); }
};

// A nonnull handle accumulates primitives into the supplied scene and returns
// a value copy of the resulting scene. No Qt/Matplotlib runtime is required.
PYSLM_API PlotScene plot(const Layer& layer,double zPos=0.0,const PlotOptions& options={},PlotScene* handle=nullptr);
PYSLM_API PlotScene plotLayers(const std::vector<Layer>& layers,const PlotOptions& options={},PlotScene* handle=nullptr);
PYSLM_API PlotScene plotSequential(const Layer& layer,const SequentialPlotOptions& options={},PlotScene* handle=nullptr);
PYSLM_API PlotScene plotPolygon(const Paths& polygons,const PolygonPlotOptions& options={},PlotScene* handle=nullptr);
PYSLM_API PlotScene plotPolygon(const Polygons& polygons,const PolygonPlotOptions& options={},PlotScene* handle=nullptr);
// Original plotHeatMap mutates exposure XY before slicing. Negative NumPy
// indices wrap once; invalid indices throw. Energy is accumulated in input order.
PYSLM_API HeatMap plotHeatMap(const Part& part,double z,std::vector<Vec3>& exposurePoints,double resolution=0.25);

} // namespace pyslm::visualise
