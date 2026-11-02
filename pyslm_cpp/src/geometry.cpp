#include "pyslm/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <iomanip>
#include <locale>
#include <sstream>
#include <set>
#include <stdexcept>

namespace pyslm {

void BuildStyle::setStyle(int id, int focus, double power, int exposureTime,
                         int exposureDistance, double speed, int laser,
                         LaserMode mode, const std::string& styleName,
                         const std::string& styleDescription) {
    bid=id; laserFocus=focus; laserPower=power; pointExposureTime=exposureTime;
    pointDistance=exposureDistance; laserSpeed=speed; laserId=laser;
    laserMode=mode; name=styleName; description=styleDescription;
}

Bounds2 LayerGeometry::boundingBox() const {
    if(coords.empty()) throw std::invalid_argument("Cannot compute bounding box of empty coordinates");
    Bounds2 result{coords[0].x,coords[0].y,coords[0].x,coords[0].y};
    for(const auto p:coords) {
        // np.min/np.max propagate NaNs.
        if(std::isnan(p.x)) result[0]=result[2]=p.x;
        else { result[0]=std::min(result[0],p.x); result[2]=std::max(result[2],p.x); }
        if(std::isnan(p.y)) result[1]=result[3]=p.y;
        else { result[1]=std::min(result[1],p.y); result[3]=std::max(result[3],p.y); }
    }
    return result;
}

namespace {
std::vector<LayerGeometry> selectGeometry(const Layer& layer, LayerGeometryType kind) {
    std::vector<LayerGeometry> result;
    for(const auto& geom:layer.geometry) if(geom.kind==kind) result.push_back(geom);
    return result;
}
}
std::vector<LayerGeometry> Layer::getContourGeometry() const { return selectGeometry(*this,LayerGeometryType::Polygon); }
std::vector<LayerGeometry> Layer::getHatchGeometry() const { return selectGeometry(*this,LayerGeometryType::Hatch); }
std::vector<LayerGeometry> Layer::getPointsGeometry() const { return selectGeometry(*this,LayerGeometryType::Pnts); }
std::vector<LayerGeometry> Layer::getGeometry(ScanMode mode) const {
    if(mode!=ScanMode::ContourFirst && mode!=ScanMode::HatchFirst) return geometry;
    auto result=mode==ScanMode::ContourFirst?getContourGeometry():getHatchGeometry();
    auto second=mode==ScanMode::ContourFirst?getHatchGeometry():getContourGeometry();
    result.insert(result.end(),second.begin(),second.end());
    auto points=getPointsGeometry(); result.insert(result.end(),points.begin(),points.end());
    return result;
}
} // namespace pyslm

namespace pyslm::geometry {
namespace {
template<class Container, class Predicate>
auto& lookup(Container& list, Predicate predicate, const char* error) {
    const auto it=std::find_if(list.begin(),list.end(),predicate);
    if(it==list.end()) throw std::out_of_range(error);
    return *it;
}
void require(bool condition, const std::string& message) {
    if(!condition) throw std::invalid_argument(message);
}
}

Layer& LayerDict::at(int id) { return lookup(entries,[id](const Entry& e){return e.first==id;},"Layer id not found").second; }
const Layer& LayerDict::at(int id) const { return lookup(entries,[id](const Entry& e){return e.first==id;},"Layer id not found").second; }
Layer& LayerDict::operator[](int id) {
    for(auto& e:entries) if(e.first==id) return e.second;
    entries.emplace_back(id,Layer(0,id)); return entries.back().second;
}
LayerDict createLayerDict(const std::vector<Layer>& layers) {
    LayerDict result;
    for(const auto& layer:layers) result[layer.id]=layer;
    return result;
}
LayerDict mergeLayers(const std::vector<LayerDict>& layerLists) {
    LayerDict result;
    for(const auto& list:layerLists) for(const auto& entry:list) {
        auto found=std::find_if(result.begin(),result.end(),[&](const LayerDict::Entry& e){return e.first==entry.first;});
        if(found==result.end()) result.entries.push_back(entry);
        else found->second.geometry.insert(found->second.geometry.end(),entry.second.geometry.begin(),entry.second.geometry.end());
    }
    return result;
}
void transformLayerGeoms(LayerGeometry& geometry,const std::array<double,4>& m) {
    for(auto& p:geometry.coords) p={m[0]*p.x+m[1]*p.y,m[2]*p.x+m[3]*p.y};
}
void transformLayerGeoms(LayerGeometry& geometry,const std::array<double,9>& m) {
    for(auto& p:geometry.coords) p={m[0]*p.x+m[1]*p.y+m[2],m[3]*p.x+m[4]*p.y+m[5]};
}
void transformLayerGeoms(std::vector<LayerGeometry>& geometries,const std::array<double,4>& m) {
    for(auto& geom:geometries) transformLayerGeoms(geom,m);
}
void transformLayerGeoms(std::vector<LayerGeometry>& geometries,const std::array<double,9>& m) {
    for(auto& geom:geometries) transformLayerGeoms(geom,m);
}
void translateLayerGeoms(LayerGeometry& geometry,Vec2 translation) { for(auto& p:geometry.coords) p=p+translation; }
void translateLayerGeoms(std::vector<LayerGeometry>& geometries,Vec2 translation) { for(auto& geom:geometries) translateLayerGeoms(geom,translation); }
const Model& getModel(const std::vector<Model>& models,int mid) { return lookup(models,[mid](const Model& m){return m.mid==mid;},"Model id not found"); }
Model& getModel(std::vector<Model>& models,int mid) { return lookup(models,[mid](const Model& m){return m.mid==mid;},"Model id not found"); }
const Layer& getLayerById(const std::vector<Layer>& layers,int id) { return lookup(layers,[id](const Layer& l){return l.id==id;},"Layer id not found"); }
Layer& getLayerById(std::vector<Layer>& layers,int id) { return lookup(layers,[id](const Layer& l){return l.id==id;},"Layer id not found"); }
const BuildStyle& getBuildStyleById(const std::vector<Model>& models,int mid,int bid) { return lookup(getModel(models,mid).buildStyles,[bid](const BuildStyle& s){return s.bid==bid;},"Build style id not found"); }
BuildStyle& getBuildStyleById(std::vector<Model>& models,int mid,int bid) { return lookup(getModel(models,mid).buildStyles,[bid](const BuildStyle& s){return s.bid==bid;},"Build style id not found"); }
std::string toString(const BuildStyle& style) {
    std::ostringstream text;text.imbue(std::locale::classic());
    text<<"Build Style: (name: "<<style.name<<", id: "<<style.bid<<")\n"
        <<std::fixed<<std::setprecision(1)<<"  laser power: "<<style.laserPower
        <<" W, laser speed: "<<style.laserSpeed<<", laser id: "<<style.laserId<<"\n";
    return text.str();
}
std::string toString(const LayerGeometry& geometry) {
    if(geometry.kind==LayerGeometryType::Hatch)return "Hatch Geometry <bid, "+std::to_string(geometry.bid)+", mid, "+std::to_string(geometry.mid)+">";
    if(geometry.kind==LayerGeometryType::Polygon)return "Contour Geometry";
    if(geometry.kind==LayerGeometryType::Pnts)return "Points Geometry";
    throw std::invalid_argument("Invalid LayerGeometry kind has no reference string");
}
std::string toString(const Layer& layer) {
    std::ostringstream text;text.imbue(std::locale::classic());text<<"Layer <z = "<<std::fixed<<std::setprecision(3)<<static_cast<double>(layer.z)<<">";return text.str();
}

bool ModelValidator::validateBuildStyle(const BuildStyle& s) {
    const std::string label="BuildStyle("+std::to_string(s.bid)+").";
    require(s.bid>=1,"BuildStyle ("+std::to_string(s.bid)+") should have a positive integer id");
    require(!(s.laserPower<0),label+"laserPower must be a positive integer");
    require(!(s.laserSpeed<0),label+"laserSpeed must be a positive integer");
    require(s.pointDistance>=1,label+"pointDistance must be a positive integer (>0)");
    require(s.pointExposureTime>=1,label+"pointExposureTime must be a positive integer (>0)");
    require(s.jumpDelay>=0,label+"jumpDelay must be a positive integer ");
    // Python requires an integer even though this is exposed as a float field.
    require((!s.jumpSpeedSourceIsInteger || *s.jumpSpeedSourceIsInteger) && s.jumpSpeed>=0 && std::floor(s.jumpSpeed)==s.jumpSpeed,label+"jumpSpeed must be a positive integer (>0)");
    require(s.laserId>=1,label+"laserId must be a positive integer (>0)");
    require(static_cast<int>(s.laserMode)>=0,label+"laserMode must be a positive integer (0)");
    return true;
}
bool ModelValidator::validateModel(const Model& model) {
    require(model.topLayerId!=0,"The top layer id of Model ("+model.name+") has not been set");
    require(!model.buildStyles.empty(),"Model ("+model.name+" does not contain any build styles");
    // Reference compares BuildStyle objects to integer ids: duplicate ids pass.
    for(const auto& style:model.buildStyles) validateBuildStyle(style);
    return true;
}
bool ModelValidator::validateBuild(const std::vector<Model>& models,const std::vector<Layer>& layers,std::vector<std::string>* warnings) {
    std::map<int,const Model*> modelIndex;
    std::map<std::pair<int,int>,const BuildStyle*> styleIndex;
    std::map<int,int> top;
    for(const auto& model:models) {
        validateModel(model); modelIndex[model.mid]=&model;
        for(const auto& style:model.buildStyles) styleIndex[{model.mid,style.bid}]=&style;
    }
    for(const auto& layer:layers) {
        if(layer.geometry.empty() && warnings) warnings->push_back("Layer "+std::to_string(layer.id)+" does not contain any layer geometry");
        for(const auto& geom:layer.geometry) {
            require(modelIndex.count(geom.mid)>0,"Layer geometry has not been assigned a model");
            require(styleIndex.count({geom.mid,geom.bid})>0,"Layer geometry has not been assigned a build style");
            top[geom.mid]=layer.id;
        }
    }
    for(const auto& model:models) {
        if(top.find(model.mid)==top.end()) {
            if(warnings) warnings->push_back("Model "+model.name+" was not used in any layer");
            throw std::out_of_range("Unused model has no top layer");
        }
        require(model.topLayerId==top.at(model.mid),"Model topLayerId differs from last layer using model");
    }
    return true;
}
} // namespace pyslm::geometry
