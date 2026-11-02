#include "pyslm/analysis.hpp"
#include "pyslm/geometry.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace pyslm::analysis {
namespace {
void checkHatch(const LayerGeometry& geometry) {
    if(geometry.coords.size()%2) throw std::invalid_argument("Hatch coordinates must contain start/end pairs");
}
double divide(double numerator,double denominator) {
    if(denominator==0.0) throw std::domain_error("Division by zero in scan time calculation");
    return numerator/denominator;
}
const Vec2& firstPoint(const LayerGeometry& geometry) {
    if(geometry.coords.empty()) throw std::out_of_range("Layer geometry has no first coordinate");
    return geometry.coords.front();
}
}

double getLayerGeometryJumpDistance(const LayerGeometry& geometry) {
    double length=0.0;
    if(geometry.kind==LayerGeometryType::Hatch) {
        checkHatch(geometry);
        for(std::size_t i=1;i+1<geometry.coords.size();i+=2) length+=norm(geometry.coords[i+1]-geometry.coords[i]);
    } else if(geometry.kind==LayerGeometryType::Pnts) {
        for(std::size_t i=1;i<geometry.coords.size();++i) length+=norm(geometry.coords[i]-geometry.coords[i-1]);
    }
    return length;
}
double getLayerGeometryPathLength(const LayerGeometry& geometry) {
    double length=0.0;
    if(geometry.kind==LayerGeometryType::Hatch) {
        checkHatch(geometry);
        for(std::size_t i=0;i<geometry.coords.size();i+=2) length+=norm(geometry.coords[i+1]-geometry.coords[i]);
    } else if(geometry.kind==LayerGeometryType::Polygon) {
        for(std::size_t i=1;i<geometry.coords.size();++i) length+=norm(geometry.coords[i]-geometry.coords[i-1]);
    } else if(geometry.kind!=LayerGeometryType::Pnts) throw std::invalid_argument("Invalid LayerGeometry kind");
    return length;
}
std::size_t getLayerGeomTotalJumps(const LayerGeometry& geometry) {
    if(geometry.kind==LayerGeometryType::Hatch) { checkHatch(geometry); return geometry.coords.size()/2; }
    if(geometry.kind==LayerGeometryType::Polygon || geometry.kind==LayerGeometryType::Pnts) return 1;
    return 0;
}
double getIntraLayerGeometryJumpLength(const Layer& layer) {
    double length=0.0;
    for(std::size_t i=0;i<layer.geometry.size();++i) {
        const auto& point=firstPoint(layer.geometry[i]);
        if(i) length+=norm(firstPoint(layer.geometry[i-1])-point);
    }
    return length;
}
double getLayerJumpLength(const Layer& layer) {
    double length=getIntraLayerGeometryJumpLength(layer);
    for(const auto& geometry:layer.geometry) length+=getLayerGeometryJumpDistance(geometry);
    return length;
}
double getLayerPathLength(const Layer& layer) {
    double length=0.0;
    for(const auto& geometry:layer.geometry) length+=getLayerGeometryPathLength(geometry);
    return length;
}
double getEffectiveLaserSpeed(const BuildStyle& style) {
    if(!(style.laserSpeed<1e-7)) return style.laserSpeed;
    if(style.pointExposureTime<1e-7) throw std::invalid_argument("Build style requires valid point exposure time and point distance");
    const double pointJumpTime=style.jumpSpeed>1e-7?style.pointDistance*1e-3/style.jumpSpeed:0.0;
    return divide(style.pointDistance*1e-3,style.pointExposureTime*1e-6+pointJumpTime+style.pointDelay*1e-6);
}
double getLayerGeometryTime(const LayerGeometry& geometry,const std::vector<Model>& models,bool includeJumpTime,double jumpSpeed,double jumpDelay) {
    const auto& style=geometry::getBuildStyleById(models,geometry.mid,geometry.bid);
    double time=0.0;
    if(geometry.kind==LayerGeometryType::Hatch || geometry.kind==LayerGeometryType::Polygon)
        time=divide(getLayerGeometryPathLength(geometry),getEffectiveLaserSpeed(style));
    else if(geometry.kind==LayerGeometryType::Pnts) time=static_cast<double>(geometry.coords.size())*static_cast<double>(style.pointExposureTime)*1e-6;
    else throw std::invalid_argument("Invalid LayerGeometry kind");
    if(includeJumpTime) {
        time+=divide(getLayerGeometryJumpDistance(geometry),style.jumpSpeed>1e-6?style.jumpSpeed:jumpSpeed);
        time+=getLayerGeomTotalJumps(geometry)*(style.jumpDelay>1e-6?style.jumpDelay:jumpDelay)*1e-6;
    }
    return time;
}
double getLayerTime(const Layer& layer,const std::vector<Model>& models,bool includeJumpTime,double laserJumpSpeed) {
    double time=0.0;
    for(const auto& geometry:layer.geometry) time+=getLayerGeometryTime(geometry,models,includeJumpTime);
    if(includeJumpTime) {
        time+=divide(getIntraLayerGeometryJumpLength(layer),laserJumpSpeed);
        // Deliberately no 1e-6 multiplier: the reference sums raw microsecond
        // integers here. Correcting this would break the pinned baseline.
        for(const auto& geometry:layer.geometry) time+=geometry::getBuildStyleById(models,geometry.mid,geometry.bid).jumpDelay;
    }
    return time;
}

double TimeNode::getChildrenTime() const {
    double result=0.0;
    for(const auto& child:children) { result+=child.time; result+=child.getChildrenTime(); }
    return result;
}
ScanVectorIterator::ScanVectorIterator(std::vector<Layer> layers):layers_(std::move(layers)) {}
ScanVectors ScanVectorIterator::reshapeVectors(const Paths& vectorList) {
    if(vectorList.empty())throw std::invalid_argument("Reference vstack requires at least one scan vector group");
    ScanVectors result;
    Path combined;
    for(const auto& path:vectorList) combined.insert(combined.end(),path.begin(),path.end());
    if(combined.size()%2) throw std::invalid_argument("Scan vectors must contain start/end pairs");
    for(std::size_t i=0;i<combined.size();i+=2) result.push_back({combined[i],combined[i+1]});
    return result;
}
Paths ScanVectorIterator::getLayerVectors(const Layer& layer) {
    Paths groups;
    for(const auto& geometry:layer.geometry) {
        if(geometry.kind==LayerGeometryType::Polygon) {
            Path path;
            for(std::size_t i=1;i<geometry.coords.size();++i) {
                path.push_back(geometry.coords[i-1]); path.push_back(geometry.coords[i]);
            }
            groups.push_back(std::move(path));
        } else if(geometry.kind==LayerGeometryType::Hatch) groups.push_back(geometry.coords);
    }
    return groups;
}
ScanVectors ScanVectorIterator::initialise() {
    Paths all;
    for(const auto& layer:layers_) {
        auto groups=getLayerVectors(layer); all.insert(all.end(),groups.begin(),groups.end());
    }
    return reshapeVectors(all);
}
bool ScanVectorIterator::next(ScanVector& vector) {
    while(true) {
        if(index_<layerVectors_.size()){vector=layerVectors_[index_++];return true;}
        if(layerIndex_>=layers_.size())return false;
        layerVectors_=reshapeVectors(getLayerVectors(layers_[layerIndex_]));index_=0;++layerIndex_;
    }
}
void ScanVectorIterator::reset() { index_=0;layerIndex_=0; }

Iterator::Iterator(std::vector<Model> models,std::vector<Layer> layers):models_(std::move(models)),layers_(std::move(layers)) {}
void Iterator::generateCache() const {
    tree_=TimeNode{};
    tree_.children.reserve(layers_.size());
    for(std::size_t layerId=0;layerId<layers_.size();++layerId) {
        tree_.children.emplace_back(); auto& node=tree_.children.back();
        node.parent=&tree_; node.id=layerId; node.layer=&layers_[layerId];
        node.children.reserve(layers_[layerId].geometry.size());
        for(std::size_t geomId=0;geomId<layers_[layerId].geometry.size();++geomId) {
            const auto& geom=layers_[layerId].geometry[geomId];
            node.children.emplace_back(); auto& geomNode=node.children.back();
            geomNode.parent=&node; geomNode.id=geomId; geomNode.geometry=&geom;
            geomNode.time=getLayerGeometryTime(geom,models_);
        }
    }
    cacheValid_=true;
}
const TimeNode& Iterator::tree() const { if(!cacheValid_) generateCache(); return tree_; }
void Iterator::setRecoaterTime(double seconds) { recoaterTime_=seconds; }
void Iterator::setLayers(std::vector<Layer> layers) { layers_=std::move(layers); cacheValid_=false; }
void Iterator::setModels(std::vector<Model> models) { models_=std::move(models); cacheValid_=false; }
double Iterator::getBuildTime() const {
    double result=0.0; for(const auto& node:tree().children) { result+=node.getChildrenTime(); result+=dwellTime(); }
    return result;
}
double Iterator::getLayerGeomTime(std::size_t layerId,std::size_t geometryId) const { return tree().children.at(layerId).children.at(geometryId).time; }
double Iterator::getLayerTime(std::size_t layerId) const { return tree().children.at(layerId).getChildrenTime(); }
double Iterator::getTimeByLayerGeometryId(std::size_t layerId,std::size_t geometryId) const {
    const auto& children=tree().children.at(layerId).children;
    if(geometryId>=children.size()) throw std::out_of_range("Layer geometry index is not in range");
    double result=getTimeByLayerId(layerId);
    for(std::size_t i=0;i<geometryId;++i) result+=children[i].time;
    return result;
}
const TimeNode* Iterator::getLayerGeometryNodeByTime(double time) const {
    double buildTime=0.0,layerEndTime=0.0;
    for(const auto& layer:tree().children) {
        layerEndTime+=layer.getChildrenTime()+dwellTime();
        if(buildTime<time && time<layerEndTime) {
            double endTime=buildTime;
            for(const auto& geometry:layer.children) {
                endTime+=geometry.time;
                if(buildTime<time && time<endTime) return &geometry;
                buildTime=endTime;
            }
            return &layer;
        }
        buildTime=layerEndTime;
    }
    return nullptr;
}
const LayerGeometry* Iterator::getLayerGeometryByTime(double time) const {
    const auto* node=getLayerGeometryNodeByTime(time);
    if(node && node->layer) throw std::logic_error("Reference returns a Layer, not LayerGeometry, during dwell or at geometry boundary; use getLayerGeometryNodeByTime");
    return node?node->geometry:nullptr;
}
std::optional<std::size_t> Iterator::getLayerGeometryIdByTime(double time) const {
    const auto* node=getLayerGeometryNodeByTime(time);
    return node?std::optional<std::size_t>(node->id):std::nullopt;
}
const TimeNode* Iterator::getLayerNodeByTime(double time) const {
    double end=0.0;
    for(const auto& node:tree().children) { const double start=end; end+=node.getChildrenTime()+dwellTime(); if(start<time && time<end) return &node; }
    return nullptr;
}
const Layer* Iterator::getLayerByTime(double time) const { const auto* node=getLayerNodeByTime(time); return node?node->layer:nullptr; }
std::optional<std::size_t> Iterator::getLayerIdByTime(double time) const { const auto* node=getLayerNodeByTime(time); return node?std::optional<std::size_t>(node->id):std::nullopt; }
double Iterator::getTimeByLayerId(std::size_t layerId) const {
    const auto& children=tree().children;
    if(layerId>=children.size()) throw std::out_of_range("Layer index is not in range");
    double time=0.0;
    for(std::size_t i=0;i<layerId;++i) { time+=children[i].getChildrenTime(); time+=dwellTime(); }
    return time;
}
const LayerGeometry& Iterator::getCurrentLayerGeometry() const { return getCurrentLayer().geometry.at(geometryIndex_); }
const Layer& Iterator::getCurrentLayer() const { return layers_.at(layerIndex_); }
void Iterator::seekByLayerGeometry(std::size_t layerId,std::size_t geometryId) {
    time_=layerGeomTime_=getTimeByLayerGeometryId(layerId,geometryId); layerIndex_=layerId; geometryIndex_=geometryId;
}
void Iterator::seekByLayer(std::size_t layerId) { time_=layerGeomTime_=getTimeByLayerId(layerId); layerIndex_=layerId; geometryIndex_=0; }
void Iterator::seek(double time) {
    const auto* node=getLayerGeometryNodeByTime(time);
    if(!node) throw std::out_of_range("Seek time is not within build time");
    time_=time; layerIndex_=node->parent->id; geometryIndex_=node->id;
    // Reference does not update _layerGeomTime here.
}
void Iterator::reset() { time_=0.0; layerGeomTime_=0.0; layerIndex_=0; geometryIndex_=0; }
bool Iterator::next(const LayerGeometry*& geometry) {
    const auto& layer=layers_.at(layerIndex_);
    if(!layer.geometry.empty() && geometryIndex_+1<layer.geometry.size()) {
        geometry=&layer.geometry.at(geometryIndex_++);
        layerGeomTime_=getTimeByLayerGeometryId(layerIndex_,geometryIndex_);
        return true;
    }
    if(layerIndex_+1<layers_.size()) {
        ++layerIndex_; geometryIndex_=0;
        geometry=&layers_[layerIndex_].geometry.at(geometryIndex_);
        layerGeomTime_=getTimeByLayerGeometryId(layerIndex_,geometryIndex_);
        return true;
    }
    geometry=nullptr; return false;
}

bool ScanIterator::isLaserOn() const {
    const double end=layerGeomTime_+getLayerGeomTime(layerIndex_,geometryIndex_);
    return layerGeomTime_<time_ && time_<end;
}
Vec2 ScanIterator::getPointInLayerGeometry(double offset,const LayerGeometry& geometry) const {
    const auto& style=geometry::getBuildStyleById(models_,geometry.mid,geometry.bid);
    const double distance=offset*getEffectiveLaserSpeed(style);
    ScanVectors vectors;
    if(geometry.kind==LayerGeometryType::Polygon) {
        for(std::size_t i=1;i<geometry.coords.size();++i) vectors.push_back({geometry.coords[i-1],geometry.coords[i]});
    } else if(geometry.kind==LayerGeometryType::Hatch) {
        checkHatch(geometry);
        for(std::size_t i=0;i<geometry.coords.size();i+=2) vectors.push_back({geometry.coords[i],geometry.coords[i+1]});
    } else throw std::invalid_argument("Reference ScanIterator does not implement point exposures");
    if(vectors.empty()) throw std::out_of_range("Cannot interpolate empty scan vectors");
    std::vector<double> cumulative{0.0};
    for(const auto& vector:vectors) cumulative.push_back(cumulative.back()+norm(vector[1]-vector[0]));
    if(geometry.kind==LayerGeometryType::Polygon && distance>cumulative.back()) throw std::out_of_range("Offset distance exceeds contour path length");
    std::size_t index=0;
    for(std::size_t i=0;i<cumulative.size();++i) if(distance<cumulative[i]) { index=i; break; }
    // Python uses index - 1, wrapping to the final vector if index stays zero.
    const std::size_t vectorIndex=index?index-1:vectors.size()-1;
    const double startDistance=index?cumulative[index-1]:cumulative.back();
    const auto& vector=vectors[vectorIndex];
    const auto delta=vector[1]-vector[0];
    const double magnitude=norm(delta);
    // NumPy division by zero yields NaN/Inf instead of a Python exception.
    const double fraction=(distance-startDistance)/magnitude;
    if(geometry.kind==LayerGeometryType::Polygon && index==0)
        return geometry.coords.back()+delta*fraction;
    return vector[0]+delta*fraction;
}
Vec2 ScanIterator::getCurrentLaserPosition() const { return getPointInLayerGeometry(time_-layerGeomTime_,getCurrentLayerGeometry()); }
const BuildStyle& ScanIterator::getCurrentBuildStyle() const { const auto& g=getCurrentLayerGeometry(); return geometry::getBuildStyleById(models_,g.mid,g.bid); }
bool ScanIterator::next(ScanSample& sample) {
    if(layers_.empty()) throw std::out_of_range("ScanIterator has no layers");
    if(!(timestep>0.0) || !std::isfinite(timestep)) throw std::invalid_argument("Scan timestep must be finite and positive");
    sample.position=time_<layerGeomTime_?Vec2{}:getCurrentLaserPosition();
    sample.laserOn=isLaserOn(); sample.time=time_;
    const double duration=getLayerGeomTime(layerIndex_,geometryIndex_);
    time_+=timestep;
    if(time_>layerGeomTime_+duration) {
        const LayerGeometry* ignored=nullptr;
        if(!Iterator::next(ignored)) return false;
    }
    return true;
}
} // namespace pyslm::analysis
