#include "pyslm/visualise.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace pyslm::visualise {
namespace {
Vec3 atZ(Vec2 point,double z){return {point.x,point.y,z};}
std::vector<Vec3> atZ(const Path& points,double z){std::vector<Vec3> result;result.reserve(points.size());for(auto p:points)result.push_back(atZ(p,z));return result;}
std::vector<std::array<Vec3,2>> scanVectors(const LayerGeometry& geometry,double z,bool includePoints=false) {
    std::vector<std::array<Vec3,2>> result;
    if(geometry.kind==LayerGeometryType::Hatch) {
        if(geometry.coords.size()%2)throw std::invalid_argument("Hatch coordinates must contain endpoint pairs");
        for(std::size_t i=0;i<geometry.coords.size();i+=2)result.push_back({atZ(geometry.coords[i],z),atZ(geometry.coords[i+1],z)});
    } else if(geometry.kind==LayerGeometryType::Polygon) {
        for(std::size_t i=1;i<geometry.coords.size();++i)result.push_back({atZ(geometry.coords[i-1],z),atZ(geometry.coords[i],z)});
    } else if(includePoints&&geometry.kind==LayerGeometryType::Pnts) {
        for(auto point:geometry.coords)result.push_back({atZ(point,z),atZ(point,z)});
    }
    return result;
}
void addArrows(PlotScene& scene,const std::vector<std::array<Vec3,2>>& vectors) {
    for(const auto& vector:vectors){const auto midpoint=(vector[0]+vector[1])*0.5;PlotPrimitive arrow;arrow.kind=PlotPrimitiveKind::Arrow;arrow.arrow={midpoint-(vector[1]-vector[0])*1e-4,midpoint};arrow.color="black";scene.primitives.push_back(std::move(arrow));}
}
PlotPrimitive orderLine(PlotScene& scene,const std::vector<std::array<Vec3,2>>& vectors) {
    PlotPrimitive line;line.kind=PlotPrimitiveKind::Polyline;line.orderLine=true;line.lineWidth=1.5;
    static const std::array<const char*,10> colors{"#1f77b4","#ff7f0e","#2ca02c","#d62728","#9467bd","#8c564b","#e377c2","#7f7f7f","#bcbd22","#17becf"};line.color=colors[scene.defaultColorIndex++%colors.size()];
    for(const auto& vector:vectors)line.points.push_back((vector[0]+vector[1])*0.5);return line;
}
PlotScene startScene(bool is3D,bool equalAspect,PlotScene* handle) {
    if(handle){if(handle->plot3D!=is3D)throw std::invalid_argument("Plot handle dimension differs from requested projection");return *handle;}
    PlotScene scene;scene.plot3D=is3D;scene.equalAspect=equalAspect;return scene;
}
PlotScene finish(PlotScene scene,PlotScene* handle){if(handle)*handle=scene;return scene;}
std::vector<double> sequential(std::size_t count){std::vector<double> values(count);std::iota(values.begin(),values.end(),0.0);return values;}
std::vector<double> hatchColors(const std::vector<LayerGeometry>& geometries,const std::vector<std::array<Vec3,2>>& vectors,const PlotOptions& options) {
    std::vector<double> values;
    if(!geometries.empty()&&(options.index=="mid"||options.index=="bid")) {
        for(const auto& geometry:geometries)values.insert(values.end(),geometry.coords.size()/2,options.index=="mid"?geometry.mid:geometry.bid);
    } else if(!geometries.empty()&&options.index=="coords") {
        for(const auto& geometry:geometries)for(std::size_t repeat=0;repeat<geometry.coords.size()/2;++repeat)for(auto p:geometry.coords){values.push_back(p.x);values.push_back(p.y);}
    } else if(!geometries.empty()&&options.index=="subType"&&!geometries[0].subType.empty())throw std::invalid_argument("Non-numeric subtype cannot be a color array");
    else if(options.index=="type")throw std::invalid_argument("Geometry type is a method, not a numeric color property in the reference");
    else if(options.index=="length") {
        for(const auto& vector:vectors){const auto delta=vector[1]-vector[0];values.push_back(std::sqrt(delta.x*delta.x+delta.y*delta.y));}
    } else if(options.indexCallback) {
        for(const auto& geometry:geometries){const auto colors=options.indexCallback(geometry);values.insert(values.end(),colors.begin(),colors.end());}
    } else values=sequential(vectors.size());
    return values;
}
}

PlotScene plot(const Layer& layer,double zPos,const PlotOptions& options,PlotScene* handle) {
    auto scene=startScene(options.plot3D,!options.plot3D,handle);const double z=options.plot3D?zPos:0.0;
    if(options.plotHatches) {
        const auto geometries=layer.getHatchGeometry();
        if(!geometries.empty()) {
            PlotPrimitive collection;collection.kind=PlotPrimitiveKind::Segments;collection.colormap="rainbow";collection.lineWidth=0.5;
            for(const auto& geometry:geometries){const auto vectors=scanVectors(geometry,z);collection.segments.insert(collection.segments.end(),vectors.begin(),vectors.end());}
            collection.colorValues=hatchColors(geometries,collection.segments,options);
            if(options.plotArrows&&!options.plot3D)addArrows(scene,collection.segments);
            const auto collectionIndex=scene.primitives.size();scene.primitives.push_back(collection);
            if(!options.plot3D&&options.plotOrderLine)scene.primitives.push_back(orderLine(scene,collection.segments));
            if(options.plotColorbar)scene.colorbarCollections.push_back(collectionIndex);
        }
    }
    if(options.plotContours) {
        bool styleSet=false;std::string color;double width=0.0;
        for(const auto& geometry:layer.getContourGeometry()) {
            if(geometry.subType=="inner"){color="#f57900";width=1.0;styleSet=true;}
            else if(geometry.subType=="outer"){color="#204a87";width=1.4;styleSet=true;}
            else if(geometry.subType.empty()){color="k";width=0.7;styleSet=true;}
            else if(!styleSet)throw std::logic_error("Reference has no contour color for an unrecognized first subtype");
            if(options.plotArrows&&!options.plot3D)addArrows(scene,scanVectors(geometry,z));
            PlotPrimitive line;line.kind=PlotPrimitiveKind::Polyline;line.points=atZ(geometry.coords,z);line.color=color;line.lineWidth=width;scene.primitives.push_back(std::move(line));
        }
    }
    if(options.plotPoints) {
        const auto geometries=layer.getPointsGeometry();
        if(!geometries.empty()) {
            PlotPrimitive scatter;scatter.kind=PlotPrimitiveKind::Points;scatter.colormap="viridis";scatter.pointSize=options.plot3D?20.0:36.0;
            for(const auto& geometry:geometries){const auto points=atZ(geometry.coords,z);scatter.points.insert(scatter.points.end(),points.begin(),points.end());if(options.indexCallback){const auto colors=options.indexCallback(geometry);scatter.colorValues.insert(scatter.colorValues.end(),colors.begin(),colors.end());}}
            if(!options.indexCallback)scatter.colorValues=sequential(scatter.points.size());scene.primitives.push_back(std::move(scatter));
        }
    }
    return finish(std::move(scene),handle);
}

PlotScene plotLayers(const std::vector<Layer>& layers,const PlotOptions& options,PlotScene* handle) {
    auto scene=startScene(true,false,handle);PlotOptions selected;selected.plot3D=true;selected.plotContours=options.plotContours;selected.plotHatches=options.plotHatches;selected.plotPoints=options.plotPoints;
    for(const auto& layer:layers)plot(layer,static_cast<double>(layer.z)/1000.0,selected,&scene);return finish(std::move(scene),handle);
}

PlotScene plotSequential(const Layer& layer,const SequentialPlotOptions& options,PlotScene* handle) {
    auto scene=startScene(false,true,handle);std::vector<std::array<Vec3,2>> vectors;
    for(const auto& geometry:layer.geometry){const auto segment=scanVectors(geometry,0,true);vectors.insert(vectors.end(),segment.begin(),segment.end());}
    if(layer.geometry.empty())return finish(std::move(scene),handle);
    const auto points=layer.getPointsGeometry();
    if(!points.empty()) {
        PlotPrimitive scatter;scatter.kind=PlotPrimitiveKind::Points;scatter.color="#000";scatter.pointSize=1.0;
        for(const auto& geometry:points){const auto values=atZ(geometry.coords,0);scatter.points.insert(scatter.points.end(),values.begin(),values.end());}scene.primitives.push_back(std::move(scatter));
    }
    if(options.plotOrderLine)scene.primitives.push_back(orderLine(scene,vectors));
    PlotPrimitive collection;collection.kind=PlotPrimitiveKind::Segments;collection.segments=vectors;collection.colormap="rainbow";collection.lineWidth=1.0;
    double distance=0.0;for(const auto& vector:vectors){const auto delta=vector[1]-vector[0];distance+=std::sqrt(delta.x*delta.x+delta.y*delta.y);collection.colorValues.push_back(distance);}scene.primitives.push_back(std::move(collection));
    if(options.plotJumps) {
        PlotPrimitive jumps;jumps.kind=PlotPrimitiveKind::Segments;jumps.colormap="Greys";jumps.lineWidth=0.7;jumps.dashed=true;jumps.jump=true;
        for(std::size_t i=1;i<vectors.size();++i)jumps.segments.push_back({vectors[i-1][1],vectors[i][0]});scene.primitives.push_back(std::move(jumps));
    }
    if(options.plotArrows)addArrows(scene,vectors);return finish(std::move(scene),handle);
}

PlotScene plotPolygon(const Paths& polygons,const PolygonPlotOptions& options,PlotScene* handle) {
    auto scene=startScene(options.plot3D,false,handle);const double z=options.plot3D?options.zPos:0.0;
    // Reference wraps patches in PatchCollection(match_original=False), so
    // Matplotlib's collection defaults replace individual fill/stroke styles.
    PlotPrimitive patches;patches.kind=PlotPrimitiveKind::Polygons;patches.filled=true;patches.fillColor="#1f77b4";patches.color="#1f77b4";patches.lineWidth=1.0;
    for(const auto& polygon:polygons) {
        if(options.plotFilled&&!options.plot3D) {
            auto path=atZ(polygon,z);if(!path.empty()&&(path.front().x!=path.back().x||path.front().y!=path.back().y))path.push_back(path.front());patches.polygons.push_back(std::move(path));
        } else {PlotPrimitive line;line.kind=PlotPrimitiveKind::Polyline;line.points=atZ(polygon,z);line.color=options.lineColor;line.lineWidth=options.lineWidth;scene.primitives.push_back(std::move(line));}
    }
    if(options.plotFilled)scene.primitives.push_back(std::move(patches));return finish(std::move(scene),handle);
}
PlotScene plotPolygon(const Polygons& polygons,const PolygonPlotOptions& options,PlotScene* handle) {return plotPolygon(Part::path2DToPathList(polygons),options,handle);}

HeatMap plotHeatMap(const Part& part,double z,std::vector<Vec3>& exposurePoints,double resolution) {
    if(!(resolution>0.0)||!std::isfinite(resolution))throw std::invalid_argument("Heat map resolution must be finite and positive");
    const auto bounds=part.boundingBox();HeatMap result;result.resolution=resolution;result.coordinateOffset={bounds[0]+resolution/2,bounds[1]+resolution/2};
    for(auto& point:exposurePoints){point.x-=result.coordinateOffset.x;point.y-=result.coordinateOffset.y;}
    const auto bitmap=part.getBitmapSlice(z,resolution);result.width=bitmap.width;result.height=bitmap.height;result.values.assign(result.width*result.height,0.0);
    auto index=[](double coordinate,double pitch,std::size_t dimension){
        const double floored=std::floor(coordinate/pitch);
        if(!std::isfinite(floored)||floored<static_cast<double>(std::numeric_limits<std::int64_t>::min())||floored>=static_cast<double>(std::numeric_limits<std::int64_t>::max()))throw std::out_of_range("Exposure point exceeds heat map index range");
        auto value=static_cast<std::int64_t>(floored);if(value<0)value+=static_cast<std::int64_t>(dimension);
        if(value<0||static_cast<std::uint64_t>(value)>=dimension)throw std::out_of_range("Exposure point is outside heat map bitmap");return static_cast<std::size_t>(value);
    };
    for(const auto point:exposurePoints){const auto x=index(point.x,resolution,result.width),y=index(point.y,resolution,result.height);result.values[y*result.width+x]+=point.z;}
    const double area=resolution*resolution;for(auto& value:result.values)value/=area;return result;
}
} // namespace pyslm::visualise
