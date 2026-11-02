#include "pyslm/hatching.hpp"
#include "clipper2/clipper.h"
#include <geos_c.h>
#include <algorithm>
#include <limits>
#include <mutex>
#include <numeric>
#include <stdexcept>

namespace pyslm::hatching {
namespace {
constexpr double pi=3.141592653589793238462643383279502884;
constexpr double scale=double(BaseHatcher::CLIPPER_SCALEFACTOR);
using namespace Clipper2Lib;
std::mutex islandCacheMutex;
std::optional<Path> islandBoundaryCache;

void positive(double value,const char* name) {
    if (!std::isfinite(value) || value<std::numeric_limits<double>::epsilon())
        throw std::invalid_argument(std::string(name)+" must be greater than zero and finite");
}
std::size_t rangeCount(double start,double end,double step) {
    positive(step,"Hatch spacing");
    if (!std::isfinite(start)||!std::isfinite(end)) throw std::invalid_argument("Non-finite hatch extent");
    const double n=std::max(0.0,std::ceil((end-start)/step));
    if (n>100000000.0) throw std::length_error("Hatch request exceeds 100 million lines");
    return static_cast<std::size_t>(n);
}
// np.arange(dtype=float32) computes length in double but uses the float32
// difference between start+step and start for the actual increment.
std::vector<double> arange(double start,double end,double step,bool float32) {
    const auto n=rangeCount(start,end,step);
    std::vector<double> out(n);
    if (float32) {
        const float a=static_cast<float>(start);
        const float delta=static_cast<float>(start+step)-a;
        for(std::size_t i=0;i<n;++i) out[i]=static_cast<float>(a+static_cast<float>(i)*delta);
    } else {
        const double delta=(start+step)-start;
        for(std::size_t i=0;i<n;++i) out[i]=start+double(i)*delta;
    }
    return out;
}
struct HatchFrame { Vec2 centre; double radius; };
HatchFrame frame(const Paths& paths) {
    auto b=BaseHatcher::boundaryBoundingBox(paths);
    Vec2 centre{(b[0]+b[2])*0.5,(b[1]+b[3])*0.5};
    Vec2 d{b[2]-centre.x,b[3]-centre.y};
    return {centre,std::sqrt(d.x*d.x+d.y*d.y)};
}
HatchLines rotate(HatchLines points,double angle,Vec2 origin) {
    const double c=std::cos(angle),s=std::sin(angle);
    for(auto& line:points) for(auto& p:line) {
        double x=c*p.x-s*p.y,y=s*p.x+c*p.y;
        p.x=x+origin.x; p.y=y+origin.y;
    }
    return points;
}
Path64 nativePath(const Path& path) {
    Path64 p; p.reserve(path.size());
    for(auto v:path) {
        if (!std::isfinite(v.x)||!std::isfinite(v.y) || std::abs(v.x)>4e13 || std::abs(v.y)>4e13)
            throw std::invalid_argument("Invalid polygon coordinate for Clipper scale");
        p.emplace_back(v.x*scale,v.y*scale);
    }
    return p;
}
Path64 nativePath(const std::vector<Vec3>& path) {
    Path64 p; p.reserve(path.size());
    for(auto v:path) {
        if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z))
            throw std::invalid_argument("Invalid open-path coordinate");
        p.emplace_back(v.x*scale,v.y*scale,v.z);
    }
    return p;
}
Paths64 nativePaths(const Paths& paths) {
    Paths64 out; out.reserve(paths.size());
    for(auto& path:paths) out.push_back(nativePath(path));
    return out;
}
Paths pythonPaths(const Paths64& paths) {
    Paths out; out.reserve(paths.size());
    for(auto& p:paths) {
        Path q; q.reserve(p.size());
        for(auto v:p) q.push_back({double(v.x)/scale,double(v.y)/scale});
        out.push_back(std::move(q));
    }
    return out;
}
void zCallback(const Point64& a,const Point64& b,const Point64& c,const Point64& d,Point64& p) {
    p.z=std::max({a.z,b.z,c.z,d.z});
}
void appendContours(std::vector<LayerGeometry>& geometry,const Paths& paths,const char* subtype) {
    for(auto path:paths) {
        if(path.empty()) continue;
        path.push_back(path.front());
        ContourGeometry g;g.coords=std::move(path);g.subType=subtype;
        geometry.push_back(std::move(g));
    }
}
double layerAngle(double angle) {
    angle=std::fmod(angle,180.0);if(angle<0.0) angle+=180.0;
    return angle>90.0?angle-180.0:angle;
}
// Shared with sorting.cpp; follows the pinned NumPy scalar argsort quicksort.
std::vector<std::size_t> argsort(const std::vector<double>& values);
void sortLines(HatchLines& lines) {
    std::vector<double> values;values.reserve(lines.size());
    for(auto& l:lines) values.push_back(l[0].z);
    auto indices=argsort(values);
    HatchLines out;out.reserve(lines.size());
    for(auto i:indices) out.push_back(lines[i]);
    lines=std::move(out);
}

struct GeosContext {
    GEOSContextHandle_t handle=GEOS_init_r();
    ~GeosContext(){GEOS_finish_r(handle);}
};
struct GeosDeleter {
    GEOSContextHandle_t h;
    void operator()(GEOSGeometry* p) const {if(p) GEOSGeom_destroy_r(h,p);}
};
using Geom=std::unique_ptr<GEOSGeometry,GeosDeleter>;
Geom own(GEOSContextHandle_t h,GEOSGeometry* g) {
    if(!g) throw std::runtime_error("GEOS geometry operation failed");
    return Geom(g,GeosDeleter{h});
}
GEOSGeometry* ring(GEOSContextHandle_t h,const Path& path) {
    if(path.size()<3) throw std::invalid_argument("Polygon ring requires at least 3 points");
    bool closed=path.front().x==path.back().x&&path.front().y==path.back().y;
    auto n=path.size()+(closed?0:1);
    auto* seq=GEOSCoordSeq_create_r(h,static_cast<unsigned int>(n),2);
    if(!seq) throw std::runtime_error("GEOS coordinate allocation failed");
    for(std::size_t i=0;i<n;++i) {
        const auto p=path[i%path.size()];
        GEOSCoordSeq_setX_r(h,seq,static_cast<unsigned int>(i),p.x);
        GEOSCoordSeq_setY_r(h,seq,static_cast<unsigned int>(i),p.y);
    }
    return GEOSGeom_createLinearRing_r(h,seq);
}
Geom polygon(GEOSContextHandle_t h,const Path& shell) {
    return own(h,GEOSGeom_createPolygon_r(h,ring(h,shell),nullptr,0));
}
Path coordinates(GEOSContextHandle_t h,const GEOSGeometry* geometry) {
    Path out;if(!geometry)return out;
    const auto* sequence=GEOSGeom_getCoordSeq_r(h,geometry);unsigned int n=0;
    if(!sequence||!GEOSCoordSeq_getSize_r(h,sequence,&n))throw std::runtime_error("GEOS coordinate extraction failed");
    out.reserve(n);for(unsigned int i=0;i<n;++i){Vec2 p;GEOSCoordSeq_getX_r(h,sequence,i,&p.x);GEOSCoordSeq_getY_r(h,sequence,i,&p.y);out.push_back(p);}
    return out;
}
ClosedPolygon closedPolygon(GEOSContextHandle_t h,const GEOSGeometry* p) {
    ClosedPolygon result;result.exterior=coordinates(h,GEOSGetExteriorRing_r(h,p));
    for(int i=0;i<GEOSGetNumInteriorRings_r(h,p);++i)result.interiors.push_back(coordinates(h,GEOSGetInteriorRingN_r(h,p,i)));
    return result;
}
Geom polygon(GEOSContextHandle_t h,const ClosedPolygon& p) {
    std::vector<GEOSGeometry*> holes;for(auto& hole:p.interiors)holes.push_back(ring(h,hole));
    return own(h,GEOSGeom_createPolygon_r(h,ring(h,p.exterior),holes.data(),static_cast<unsigned int>(holes.size())));
}
Geom largestPolygon(GEOSContextHandle_t h,Geom p) {
    int type=GEOSGeomTypeId_r(h,p.get());
    if(type==GEOS_POLYGON)return p;
    int count=GEOSGetNumGeometries_r(h,p.get());
    double maximum=-1;const GEOSGeometry* selected=nullptr;
    for(int i=0;i<count;++i) {
        const auto* candidate=GEOSGetGeometryN_r(h,p.get(),i);double area=0;GEOSArea_r(h,candidate,&area);
        if(area>maximum){maximum=area;selected=candidate;}
    }
    if(!selected||GEOSGeomTypeId_r(h,selected)!=GEOS_POLYGON)throw std::invalid_argument("Unable to recover polygon");
    return own(h,GEOSGeom_clone_r(h,selected));
}
bool repairedPerimeter(GEOSContextHandle_t h,const GEOSGeometry* p,double reference) {
    if(GEOSisValid_r(h,p)!=1)return false;
    double perimeter=0;GEOSLength_r(h,p,&perimeter);return std::abs(perimeter-reference)<=1e-8+0.5*std::abs(reference);
}
Geom repairPolygon(GEOSContextHandle_t h,Geom original,const Path& originalPath) {
    if(GEOSisValid_r(h,original.get())==1)return original;
    double perimeter=0;GEOSLength_r(h,original.get(),&perimeter);
    auto basic=largestPolygon(h,own(h,GEOSBuffer_r(h,original.get(),1e-12,16)));
    if(repairedPerimeter(h,basic.get(),perimeter))return basic;
    auto b=BaseHatcher::polygonBoundingBox(originalPath);
    double distance=.002*((b[2]-b[0])+(b[3]-b[1]))*.5;
    auto ringBuffer=own(h,GEOSBuffer_r(h,GEOSGetExteriorRing_r(h,original.get()),distance,16));
    if(GEOSGetNumInteriorRings_r(h,ringBuffer.get())==1) {
        auto p=polygon(h,coordinates(h,GEOSGetInteriorRingN_r(h,ringBuffer.get(),0)));
        auto reconstructed=own(h,GEOSBuffer_r(h,p.get(),distance,16));
        if(repairedPerimeter(h,reconstructed.get(),perimeter))return reconstructed;
    }
    Path unique;for(auto p:originalPath)if(unique.empty()||norm(p-unique.back())>1e-8)unique.push_back(p);
    if(unique.size()>=3) {
        auto deduplicated=polygon(h,unique);
        if(repairedPerimeter(h,deduplicated.get(),perimeter))return deduplicated;
    }
    auto buffered=own(h,GEOSBuffer_r(h,original.get(),distance,16));
    auto unbuffered=own(h,GEOSBuffer_r(h,buffered.get(),-distance,16));
    if(GEOSGeomTypeId_r(h,unbuffered.get())!=GEOS_POLYGON)return largestPolygon(h,std::move(unbuffered));
    if(repairedPerimeter(h,unbuffered.get(),perimeter))return unbuffered;
    throw std::invalid_argument("Unable to recover polygon");
}
Geom multiPolygon(GEOSContextHandle_t h,const Paths& paths) {
    std::vector<Geom> raw;
    raw.reserve(paths.size());
    for(auto& p:paths) {
        Geom valid(nullptr,GeosDeleter{h});
        if(p.size()>=4)try{valid=repairPolygon(h,polygon(h,p),p);}catch(const std::invalid_argument&){}
        raw.push_back(std::move(valid));
    }
    std::vector<std::vector<std::size_t>> contains(paths.size());
    std::vector<unsigned int> degree(paths.size(),0);
    for(std::size_t i=0;i<paths.size();++i) for(std::size_t j=0;j<paths.size();++j) if(i!=j&&raw[i]&&raw[j]) {
        char yes=GEOSContains_r(h,raw[i].get(),raw[j].get());
        if(yes==2) throw std::runtime_error("GEOS containment failed");
        if(yes) {contains[i].push_back(j);++degree[j];}
    }
    std::vector<std::size_t> roots;
    for(std::size_t i=0;i<paths.size();++i) if(raw[i]&&degree[i]%2==0) roots.push_back(i);
    if(!degree.empty()&&*std::max_element(degree.begin(),degree.end())>1) {
        std::vector<double> keys;for(auto r:roots)keys.push_back(double(degree[r]));
        auto indices=argsort(keys);auto unsorted=roots;
        for(std::size_t i=0;i<roots.size();++i)roots[i]=unsorted[indices[i]];
    }
    std::vector<GEOSGeometry*> pieces;
    for(auto i:roots) {
        std::vector<GEOSGeometry*> holes;
        for(auto j:contains[i]) if(degree[j]==degree[i]+1)holes.push_back(GEOSGeom_clone_r(h,GEOSGetExteriorRing_r(h,raw[j].get())));
        auto* p=GEOSGeom_createPolygon_r(h,GEOSGeom_clone_r(h,GEOSGetExteriorRing_r(h,raw[i].get())),holes.data(),static_cast<unsigned int>(holes.size()));
        if(!p) {for(auto* piece:pieces) GEOSGeom_destroy_r(h,piece);throw std::runtime_error("GEOS polygon failed");}
        pieces.push_back(p);
    }
    return own(h,GEOSGeom_createCollection_r(h,GEOS_MULTIPOLYGON,pieces.data(),static_cast<unsigned int>(pieces.size())));
}
} // namespace

// Internal sorting entry point has external linkage to share one ordering policy.
std::vector<std::size_t> numpyArgsort(const std::vector<double>& values);
namespace {std::vector<std::size_t> argsort(const std::vector<double>& v){return numpyArgsort(v);}}

std::vector<ClosedPolygon> pathsToClosedPolygons(const Paths& paths) {
    GeosContext context;auto h=context.handle;auto poly=multiPolygon(h,paths);
    std::vector<ClosedPolygon> result;int n=GEOSGetNumGeometries_r(h,poly.get());
    for(int i=0;i<n;++i)result.push_back(closedPolygon(h,GEOSGetGeometryN_r(h,poly.get(),i)));
    return result;
}
Paths getContoursFromPolygon(const ClosedPolygon& p) {
    Paths result{p.exterior};result.insert(result.end(),p.interiors.begin(),p.interiors.end());return result;
}
std::pair<Paths,Paths> getContourRingsFromPolygon(const ClosedPolygon& p){return {Paths{p.exterior},p.interiors};}
Paths poly2Paths(const ClosedPolygon& p){return getContoursFromPolygon(p);}
Paths poly2Paths(const std::vector<ClosedPolygon>& polygons) {
    Paths result;for(auto& p:polygons)result.push_back(p.exterior);return result;
}
std::vector<ClosedPolygon> simplifyBoundaries(const std::vector<ClosedPolygon>& polygons,double tolerance) {
    if(polygons.empty())throw std::out_of_range("Cannot simplify empty polygon collection");
    GeosContext context;auto h=context.handle;std::vector<ClosedPolygon> result;
    for(auto& p:polygons){auto poly=polygon(h,p);auto simplified=own(h,GEOSTopologyPreserveSimplify_r(h,poly.get(),tolerance));result.push_back(closedPolygon(h,simplified.get()));}
    return result;
}

bool isValidHatchArray(const Path& v) noexcept{return v.size()%2==0;}
HatchLines to3DHatchArray(const Path& v) {
    if(!isValidHatchArray(v)) throw std::invalid_argument("Hatch vector coordinate count must be even");
    HatchLines out;out.reserve(v.size()/2);
    for(std::size_t i=0;i<v.size();i+=2) out.push_back({Vec3{v[i].x,v[i].y,0},Vec3{v[i+1].x,v[i+1].y,0}});
    return out;
}
Path from3DHatchArray(const HatchLines& v) {
    Path out;out.reserve(v.size()*2);
    for(auto& line:v) for(auto p:line) out.push_back({p.x,p.y});
    return out;
}
std::vector<std::vector<Vec3>> paths2clipper(const Paths& paths) {
    std::vector<std::vector<Vec3>> out;
    for(auto& path:paths) {
        std::vector<Vec3> p;p.reserve(path.size());
        for(std::size_t i=0;i<path.size();++i) p.push_back({path[i].x,path[i].y,double(i)});
        out.push_back(std::move(p));
    }
    return out;
}
Paths clipper2Paths(const std::vector<std::vector<Vec3>>& paths,double s,bool close) {
    positive(s,"Scale factor");Paths out;
    for(auto& path:paths) {
        Path p;for(auto v:path) p.push_back({v.x/s,v.y/s});
        if(close&&!p.empty()) p.push_back(p.front());
        out.push_back(std::move(p));
    }
    return out;
}
Paths simplifyBoundaries(const Paths& paths,double tolerance) {
    if(paths.empty())throw std::out_of_range("Cannot simplify empty path collection");
    if(tolerance<=0) return paths;
    Paths result;
    for(auto& p:paths) {
        if(p.size()<2) {result.push_back(p);continue;}
        std::vector<bool> keep(p.size());keep.front()=keep.back()=true;
        std::vector<std::pair<std::size_t,std::size_t>> stack{{0,p.size()-1}};
        while(!stack.empty()) {
            auto [start,end]=stack.back();stack.pop_back();
            Vec2 d=p[end]-p[start];
            const double a=-std::atan2(d.x,d.y),si=std::sin(a),co=std::cos(a);
            const double segmentDistance=p[start].y*si+p[start].x*co;
            double maxDistance=-1.0;std::size_t maxIndex=start;
            for(auto i=start+1;i<end;++i) {
                auto d0=p[i]-p[start],d1=p[i]-p[end];
                double distance=dot(d0,d)>0&&-dot(d1,d)>0?
                    std::abs(p[i].x*co+p[i].y*si-segmentDistance):std::min(norm(d0),norm(d1));
                if(distance>maxDistance){maxDistance=distance;maxIndex=i;}
            }
            if(maxDistance>tolerance){keep[maxIndex]=true;stack.push_back({maxIndex,end});stack.push_back({start,maxIndex});}
        }
        Path out;for(std::size_t i=0;i<p.size();++i) if(keep[i]) out.push_back(p[i]);
        result.push_back(std::move(out));
    }
    return result;
}
Paths BaseHatcher::offsetBoundary(const Paths& paths,double offset) {
    if(!std::isfinite(offset)) throw std::invalid_argument("Non-finite polygon offset");
    ClipperOffset op;op.AddPaths(nativePaths(paths),JoinType::Round,EndType::Polygon);
    Paths64 out;op.Execute(offset*scale,out);
    return pythonPaths(out);
}
std::vector<Paths> BaseHatcher::offsetPolygons(const std::vector<Paths>& polygons,double offset) {
    std::vector<Paths> out;for(auto& p:polygons) out.push_back(offsetBoundary(p,offset));return out;
}
Bounds2 BaseHatcher::polygonBoundingBox(const Path& p){return polygonBoundingBox(Paths{p});}
Bounds2 BaseHatcher::polygonBoundingBox(const Paths& paths) {
    const double inf=std::numeric_limits<double>::infinity();
    Bounds2 b{inf,inf,-inf,-inf};
    if(paths.empty()) throw std::invalid_argument("Cannot compute empty boundary bounds");
    for(auto& path:paths) {
        if(path.empty()) throw std::invalid_argument("Cannot compute empty path bounds");
        for(auto p:path) {b[0]=std::min(b[0],p.x);b[1]=std::min(b[1],p.y);b[2]=std::max(b[2],p.x);b[3]=std::max(b[3],p.y);}
    }
    return b;
}
Bounds2 BaseHatcher::boundaryBoundingBox(const Paths& paths){return polygonBoundingBox(paths);}
HatchLines BaseHatcher::clipLines(const Paths& paths,const HatchLines& lines) {
    if(lines.empty()) return {};
    Clipper64 op;op.SetZCallback(zCallback);
    Paths64 subjects;subjects.reserve(lines.size());
    for(auto& l:lines) subjects.push_back(nativePath(std::vector<Vec3>{l[0],l[1]}));
    op.AddOpenSubject(subjects);op.AddClip(nativePaths(paths));
    Paths64 closed,opened;
    if(!op.Execute(ClipType::Intersection,FillRule::NonZero,closed,opened)) throw std::runtime_error("Clipper line clipping failed");
    HatchLines out;out.reserve(opened.size());
    for(auto& p:opened) {
        if(p.size()!=2) throw std::runtime_error("Clipper returned a non-segment hatch path");
        out.push_back({Vec3{double(p[0].x)/scale,double(p[0].y)/scale,double(p[0].z)},
                       Vec3{double(p[1].x)/scale,double(p[1].y)/scale,double(p[1].z)}});
    }
    return out;
}
std::vector<std::vector<Vec3>> BaseHatcher::clipContourLines(const Paths& paths,const std::vector<std::vector<Vec3>>& contours) {
    Clipper64 op;op.SetZCallback(zCallback);Paths64 subjects;
    for(auto& p:contours) subjects.push_back(nativePath(p));
    op.AddOpenSubject(subjects);op.AddClip(nativePaths(paths));
    Paths64 closed,opened;op.Execute(ClipType::Intersection,FillRule::NonZero,closed,opened);
    std::vector<std::vector<Vec3>> out;
    for(auto& p:opened) {
        std::vector<Vec3> q;for(auto v:p) q.push_back({double(v.x)/scale,double(v.y)/scale,double(v.z)});
        out.push_back(std::move(q));
    }
    return out;
}
HatchLines BaseHatcher::generateHatching(const Paths& paths,double spacing,double angle) const {
    auto f=frame(paths);auto xs=arange(-f.radius,f.radius,spacing,true);
    HatchLines out;out.reserve(xs.size());
    for(std::size_t i=0;i<xs.size();++i) out.push_back({Vec3{xs[i],-f.radius,double(i)},Vec3{xs[i],f.radius,double(i)}});
    return rotate(std::move(out),angle*pi/180.0,f.centre);
}
std::optional<Layer> Hatcher::hatch(const Paths& boundary) const {
    if(boundary.empty()) return std::nullopt;
    if(numOuterContours<0||numInnerContours<0) throw std::invalid_argument("Contour count cannot be negative");
    Layer layer;std::vector<LayerGeometry> contours,hatches;
    double delta=1e-6-spotCompensation;
    for(int i=0;i<numOuterContours;++i) {
        if(i>0) delta-=contourOffset;
        appendContours(contours,offsetBoundary(boundary,delta),"outer");
    }
    for(int i=0;i<numInnerContours;++i) {
        if((numOuterContours==0&&i>0)||numOuterContours>0) delta-=contourOffset;
        appendContours(contours,offsetBoundary(boundary,delta),"inner");
    }
    if(numInnerContours+numOuterContours>0) delta-=volumeOffsetHatch;
    auto current=offsetBoundary(boundary,delta);
    if(hatchingEnabled&&!current.empty()) {
        auto clipped=clipLines(current,generateHatching(current,hatchDistance,layerAngle(hatchAngle+layerAngleIncrement)));
        if(!clipped.empty()) {
            sortLines(clipped);HatchGeometry g;g.coords=from3DHatchArray(clipped);
            if(hatchSortMethod) g.coords=hatchSortMethod->sort(g.coords);
            hatches.push_back(std::move(g));
        }
    }
    auto append=[&](const std::vector<LayerGeometry>& group){layer.geometry.insert(layer.geometry.end(),group.begin(),group.end());};
    if(scanContourFirst){append(contours);append(hatches);}else{append(hatches);append(contours);}
    return layer;
}
HatchLines StripeHatcher::generateHatching(const Paths& paths,double spacing,double angle) const {
    positive(stripeWidth,"Stripe width");auto f=frame(paths);
    const auto count=static_cast<std::size_t>(2*f.radius/stripeWidth)+1;
    HatchLines out;std::size_t order=0;
    for(std::size_t i=0;i<count;++i) {
        double x=-f.radius+double(i)*stripeWidth-stripeOverlap;
        double end=x+stripeWidth+stripeOverlap;
        double offset=double(i%2)*stripeOffset*spacing;
        auto ys=arange(-f.radius+offset,f.radius+offset,spacing,true);
        for(auto y:ys) {out.push_back({Vec3{x,y,double(order)},Vec3{end,y,double(order)}});++order;}
    }
    return rotate(std::move(out),-angle*pi/180.0,f.centre);
}
HatchLines BasicIslandHatcher::generateHatching(const Paths& paths,double spacing,double angle) const {
    positive(islandWidth,"Island width");auto f=frame(paths);
    const auto count=static_cast<std::size_t>(2*f.radius/islandWidth)+1;
    HatchLines out;std::size_t order=0;
    for(std::size_t i=0;i<count;++i) for(std::size_t j=0;j<count;++j) {
        double sx=-f.radius+double(i)*islandWidth-islandOverlap,ex=sx+islandWidth+islandOverlap;
        double sy=-f.radius+double(j)*islandWidth-islandOverlap,ey=sy+islandWidth+islandOverlap;
        bool odd=(i+j)%2!=0;
        double offset=double((i+j)%2)*islandOffset*spacing;
        auto values=arange((odd?sy:sx)+offset,(odd?ey:ex)+offset,spacing,true);
        for(auto v:values) {
            if(odd) out.push_back({Vec3{sx,v,double(order)},Vec3{ex,v,double(order)}});
            else out.push_back({Vec3{v,sy,double(order)},Vec3{v,ey,double(order)}});
            ++order;
        }
    }
    return rotate(std::move(out),angle*pi/180.0,f.centre);
}
Path InnerHatchRegion::transformCoordinates2D(const Path& coords) const {
    auto m=rotationMatrix2D();Path out;out.reserve(coords.size());
    for(auto p:coords) out.push_back({m[0]*p.x+m[1]*p.y+origin.x,m[2]*p.x+m[3]*p.y+origin.y});
    return out;
}
HatchLines InnerHatchRegion::transformCoordinates(const HatchLines& coords) const{return rotate(coords,orientation,origin);}
std::array<double,4> InnerHatchRegion::rotationMatrix2D() const {
    double c=std::cos(orientation),s=std::sin(orientation);return {c,-s,s,c};
}
std::array<double,9> InnerHatchRegion::rotationMatrix3D() const {
    double c=std::cos(orientation),s=std::sin(orientation);return {c,-s,0,s,c,0,0,0,1};
}
Island::Island(Vec2 p,double r,double w,double overlap,double spacing) {
    origin=p;orientation=r;islandWidth=w;islandOverlap=overlap;hatchDistance=spacing;
}
Path Island::localBoundary() const {
    std::lock_guard<std::mutex> lock(islandCacheMutex);
    if(!islandBoundaryCache) {
        double s=-islandOverlap,e=islandWidth+islandOverlap;
        islandBoundaryCache=Path{{s,s},{s,e},{e,e},{e,s},{s,s}};
    }
    return *islandBoundaryCache;
}
void Island::resetBoundaryCache(){std::lock_guard<std::mutex> lock(islandCacheMutex);islandBoundaryCache.reset();}
Path Island::boundary() const{return transformCoordinates2D(localBoundary());}
HatchLines Island::generateInternalHatch(bool odd) const {
    positive(hatchDistance,"Hatch distance");double s=-islandOverlap,e=islandWidth+islandOverlap;
    auto values=arange(s,e,hatchDistance,false);HatchLines out;out.reserve(values.size());
    for(std::size_t i=0;i<values.size();++i) {
        double v=values[i];
        if(odd)out.push_back({Vec3{s,v,double(i)},Vec3{e,v,double(i)}});
        else out.push_back({Vec3{v,s,double(i)},Vec3{v,e,double(i)}});
    }
    return out;
}
HatchLines Island::hatch() const{return transformCoordinates(generateInternalHatch((posId[0]+posId[1])%2!=0));}
std::vector<Island> IslandHatcher::generateIslands(const Paths& paths,double angle) const {
    positive(islandWidth,"Island width");auto f=frame(paths);double a=angle*pi/180,c=std::cos(a),s=std::sin(a);
    const auto count=static_cast<std::size_t>(2*f.radius/islandWidth)+1;
    std::vector<Island> islands;islands.reserve(count*count);
    for(std::size_t i=0;i<count;++i)for(std::size_t j=0;j<count;++j) {
        double x=-f.radius+double(i)*islandWidth,y=-f.radius+double(j)*islandWidth;
        Island island({c*x-s*y+f.centre.x,s*x+c*y+f.centre.y},a,islandWidth,islandOverlap,hatchDistance);
        island.id=static_cast<int>(islands.size());island.posId={int(i),int(j)};islands.push_back(std::move(island));
    }
    return islands;
}
std::pair<std::vector<std::size_t>,std::vector<std::size_t>> IslandHatcher::intersectIslands(const Paths& paths,std::vector<Island>& islands) const {
    Paths boundaries;for(auto& island:islands)boundaries.push_back(island.boundary());
    auto flags=classifyIslandBoundaries(paths,boundaries);
    std::vector<std::size_t> clipped,untouched;
    for(std::size_t i=0;i<islands.size();++i) {
        bool intersect=flags[i].first,overlap=flags[i].second;
        if(overlap){clipped.push_back(i);islands[i].setRequiresClipping(true);}
        if(intersect){islands[i].setIntersecting(true);if(!overlap)untouched.push_back(i);}
    }
    return {clipped,untouched};
}
std::vector<std::pair<bool,bool>> classifyIslandBoundaries(const Paths& paths,const Paths& boundaries){
    GeosContext context;auto h=context.handle;auto poly=multiPolygon(h,paths);std::vector<std::pair<bool,bool>> result;
    for(auto& boundary:boundaries){auto p=polygon(h,boundary);auto overlap=GEOSOverlaps_r(h,poly.get(),p.get()),intersect=GEOSIntersects_r(h,poly.get(),p.get());if(overlap==2||intersect==2)throw std::runtime_error("GEOS island predicates failed");result.push_back({bool(intersect),bool(overlap)});}return result;
}
Paths IslandHatcher::clipIslands(const Paths&,const Paths&) const {
    throw std::invalid_argument("Original clipIslands uses open clip paths, unsupported by Clipper2; use clipContourLines or closed polygon intersection");
}
std::optional<Layer> IslandHatcher::hatch(const Paths& boundary) const {
    Layer layer;if(boundary.empty()) return layer;
    if(numOuterContours<0||numInnerContours<0) throw std::invalid_argument("Contour count cannot be negative");
    double delta=-spotCompensation;
    for(int i=0;i<numOuterContours;++i){delta-=contourOffset;appendContours(layer.geometry,offsetBoundary(boundary,delta),"outer");}
    for(int i=0;i<numInnerContours;++i){delta-=contourOffset;appendContours(layer.geometry,offsetBoundary(boundary,delta),"inner");}
    delta-=volumeOffsetHatch;auto current=offsetBoundary(boundary,delta);
    if(current.empty()) return layer;
    auto islands=generateIslands(current,hatchAngle);intersectIslands(current,islands);
    std::stable_sort(islands.begin(),islands.end(),[](auto& a,auto& b){return a.posId<b.posId;});
    HatchLines clip,unclip;std::size_t order=0;
    for(auto& island:islands) {
        auto lines=island.hatch();for(auto& line:lines)for(auto& v:line)v.z+=double(order);
        if(island.isIntersecting()) {
            auto& target=island.requiresClipping()?clip:unclip;
            target.insert(target.end(),lines.begin(),lines.end());
        }
        order+=lines.size();
    }
    HatchLines lines=clipLines(current,clip);lines.insert(lines.end(),unclip.begin(),unclip.end());
    if(!lines.empty()) {
        sortLines(lines);HatchGeometry g;g.coords=from3DHatchArray(lines);
        if(hatchSortMethod)g.coords=hatchSortMethod->sort(g.coords);
        layer.geometry.push_back(std::move(g));
    }
    return layer;
}
std::vector<Vec3> getExposurePoints(const Layer& layer,const std::vector<Model>& models,bool includePower) {
    std::vector<Vec3> out;
    for(auto& g:layer.geometry) {
        auto model=std::find_if(models.begin(),models.end(),[&](auto& m){return m.mid==g.mid;});
        if(model==models.end())throw std::invalid_argument("Exposure geometry references missing model");
        auto style=std::find_if(model->buildStyles.begin(),model->buildStyles.end(),[&](auto& s){return s.bid==g.bid;});
        if(style==model->buildStyles.end())throw std::invalid_argument("Exposure geometry references missing build style");
        if(style->pointDistance<1)throw std::invalid_argument("Point distance in build style must be set");
        const double distance=style->pointDistance*1e-3;
        const double energy=includePower?style->laserPower*(style->pointExposureTime*1e-6):0.0;
        if(g.kind==LayerGeometryType::Hatch&&!isValidHatchArray(g.coords))throw std::invalid_argument("Odd hatch coordinate count");
        std::size_t stride=g.kind==LayerGeometryType::Hatch?2:1;
        if(g.kind!=LayerGeometryType::Hatch&&g.kind!=LayerGeometryType::Polygon)continue;
        for(std::size_t i=0;i+1<g.coords.size();i+=stride) {
            Vec2 a=g.coords[i],b=g.coords[i+1],d=b-a;
            double length=norm(d);if(length==0)continue;
            const auto n=rangeCount(0,length,distance);
            Vec2 start=g.kind==LayerGeometryType::Hatch?b:a;
            Vec2 direction=d/length*(g.kind==LayerGeometryType::Hatch?-1.0:1.0);
            for(std::size_t j=0;j<n;++j){auto p=start+direction*(distance*double(j));out.push_back({p.x,p.y,energy});}
        }
    }
    if(out.empty())throw std::invalid_argument("No contour or hatch exposure points to stack");
    return out;
}
} // namespace pyslm::hatching
