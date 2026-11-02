#pragma once
#include "pyslm/core.hpp"
#include "pyslm/hatching.hpp"
#include "clipper2/clipper.h"
#include <geos_c.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <numeric>
#include <stdexcept>

namespace pyslm::support::detail {
constexpr double pi=3.141592653589793238462643383279502884;
struct Context {GEOSContextHandle_t h=GEOS_init_r();~Context(){GEOS_finish_r(h);}};
struct Delete {GEOSContextHandle_t h;void operator()(GEOSGeometry* p)const{if(p)GEOSGeom_destroy_r(h,p);}};
using Geom=std::unique_ptr<GEOSGeometry,Delete>;
inline Geom own(GEOSContextHandle_t h,GEOSGeometry* p){if(!p)throw std::runtime_error("GEOS support operation failed");return Geom(p,Delete{h});}
inline GEOSGeometry* ring(GEOSContextHandle_t h,const Path& p){
    if(p.size()<3)throw std::invalid_argument("Support polygon has fewer than 3 points");
    const bool closed=p.front().x==p.back().x&&p.front().y==p.back().y;auto n=p.size()+(closed?0:1);
    auto* seq=GEOSCoordSeq_create_r(h,unsigned(n),2);
    for(std::size_t i=0;i<n;++i){GEOSCoordSeq_setX_r(h,seq,unsigned(i),p[i%p.size()].x);GEOSCoordSeq_setY_r(h,seq,unsigned(i),p[i%p.size()].y);}
    return GEOSGeom_createLinearRing_r(h,seq);
}
inline Geom native(GEOSContextHandle_t h,const Polygon& p){
    std::vector<GEOSGeometry*> holes;for(auto& q:p.interiors)holes.push_back(ring(h,q));
    return own(h,GEOSGeom_createPolygon_r(h,ring(h,p.exterior),holes.data(),unsigned(holes.size())));
}
inline Path coordinates(GEOSContextHandle_t h,const GEOSGeometry* p){
    Path out;const auto* seq=GEOSGeom_getCoordSeq_r(h,p);unsigned n=0;GEOSCoordSeq_getSize_r(h,seq,&n);
    for(unsigned i=0;i<n;++i){Vec2 v;GEOSCoordSeq_getX_r(h,seq,i,&v.x);GEOSCoordSeq_getY_r(h,seq,i,&v.y);out.push_back(v);}return out;
}
inline Polygons extract(GEOSContextHandle_t h,const GEOSGeometry* p){
    Polygons out;if(GEOSisEmpty_r(h,p)==1)return out;
    if(GEOSGeomTypeId_r(h,p)==GEOS_POLYGON){Polygon q;q.exterior=coordinates(h,GEOSGetExteriorRing_r(h,p));for(int i=0;i<GEOSGetNumInteriorRings_r(h,p);++i)q.interiors.push_back(coordinates(h,GEOSGetInteriorRingN_r(h,p,i)));out.push_back(std::move(q));}
    else if(GEOSGeomTypeId_r(h,p)>=GEOS_MULTIPOINT){for(int i=0;i<GEOSGetNumGeometries_r(h,p);++i){auto q=extract(h,GEOSGetGeometryN_r(h,p,i));out.insert(out.end(),q.begin(),q.end());}}
    return out;
}
inline Polygons buffer(const Polygon& p,double distance){Context c;auto g=native(c.h,p);auto b=own(c.h,GEOSBuffer_r(c.h,g.get(),distance,16));return extract(c.h,b.get());}
inline Polygons simplify(const Polygon& p,double tolerance,bool preserve=true){Context c;auto g=native(c.h,p);auto b=own(c.h,preserve?GEOSTopologyPreserveSimplify_r(c.h,g.get(),tolerance):GEOSSimplify_r(c.h,g.get(),tolerance));return extract(c.h,b.get());}
inline Polygons simplifyBuffer(const Polygon& p,double tolerance,bool preserve,double distance){Context c;auto original=native(c.h,p);auto simplified=own(c.h,preserve?GEOSTopologyPreserveSimplify_r(c.h,original.get(),tolerance):GEOSSimplify_r(c.h,original.get(),tolerance));auto offset=own(c.h,GEOSBuffer_r(c.h,simplified.get(),distance,16));return extract(c.h,offset.get());}
inline Polygons combine(const Polygon& a,const Polygon& b,bool intersection){Context c;auto x=native(c.h,a),y=native(c.h,b);auto r=own(c.h,intersection?GEOSIntersection_r(c.h,x.get(),y.get()):GEOSUnion_r(c.h,x.get(),y.get()));return extract(c.h,r.get());}
inline Polygons wallIntersection(const Polygon& a,const Polygon& b,const Polygons& fill,double thickness,double initialBuffer=0){
    Context c;auto x=native(c.h,a),y=native(c.h,b);if(initialBuffer!=0){x=own(c.h,GEOSBuffer_r(c.h,x.get(),initialBuffer,16));y=own(c.h,GEOSBuffer_r(c.h,y.get(),initialBuffer,16));}
    auto united=own(c.h,GEOSUnion_r(c.h,x.get(),y.get()));auto walls=own(c.h,GEOSBuffer_r(c.h,united.get(),thickness,16));
    Geom inside(nullptr,Delete{c.h});if(fill.size()==1)inside=native(c.h,fill.front());else{std::vector<GEOSGeometry*> pieces;for(auto& p:fill)pieces.push_back(native(c.h,p).release());inside=own(c.h,GEOSGeom_createCollection_r(c.h,GEOS_MULTIPOLYGON,pieces.data(),unsigned(pieces.size())));}
    auto clipped=own(c.h,GEOSIntersection_r(c.h,walls.get(),inside.get()));return extract(c.h,clipped.get());
}
inline double area(const Polygon& p){Context c;auto g=native(c.h,p);double a=0;GEOSArea_r(c.h,g.get(),&a);return a;}
inline Paths flatten(const Polygons& p){Paths out;for(auto& q:p){out.push_back(q.exterior);out.insert(out.end(),q.interiors.begin(),q.interiors.end());}return out;}
inline Polygons polygons(const Paths& p){Polygons result;for(auto& q:hatching::pathsToClosedPolygons(p))result.push_back({std::move(q.exterior),std::move(q.interiors)});return result;}
inline Vec3 transform(const Matrix4& m,Vec3 p){return {m[0]*p.x+m[1]*p.y+m[2]*p.z+m[3],m[4]*p.x+m[5]*p.y+m[6]*p.z+m[7],m[8]*p.x+m[9]*p.y+m[10]*p.z+m[11]};}
inline void transform(Mesh& mesh,const Matrix4& m){for(auto& p:mesh.vertices)p=transform(m,p);}
inline std::vector<double> arange(double start,double stop,double step,bool f32=false){
    if(!(step>0.0)||!std::isfinite(step))throw std::invalid_argument("Support spacing must be positive");
    auto n=std::max(0.0,std::ceil((stop-start)/step));if(n>1e8)throw std::length_error("Support array is too large");
    std::vector<double> out;out.reserve(std::size_t(n));
    if(f32){float a=float(start),d=float(start+step)-a;for(std::size_t i=0;i<std::size_t(n);++i)out.push_back(float(a+float(i)*d));}
    else {double d=(start+step)-start;for(std::size_t i=0;i<std::size_t(n);++i)out.push_back(start+double(i)*d);}
    return out;
}
inline Clipper2Lib::Paths64 clipper(const Paths& p,double scale){Clipper2Lib::Paths64 out;for(auto& q:p){Clipper2Lib::Path64 r;for(auto v:q)r.emplace_back(v.x*scale,v.y*scale);out.push_back(std::move(r));}return out;}
inline Paths unclipper(const Clipper2Lib::Paths64& p,double scale){Paths out;for(auto& q:p){Path r;for(auto v:q)r.push_back({double(v.x)/scale,double(v.y)/scale});out.push_back(std::move(r));}return out;}
inline Paths offset(const Paths& paths,double distance,Clipper2Lib::JoinType join=Clipper2Lib::JoinType::Round,Clipper2Lib::EndType end=Clipper2Lib::EndType::Polygon,double scale=1e5){
    Clipper2Lib::ClipperOffset op;op.AddPaths(clipper(paths,scale),join,end);Clipper2Lib::Paths64 result;op.Execute(distance*scale,result);return unclipper(result,scale);
}
inline Paths clip(const Paths& a,const Paths& b,Clipper2Lib::ClipType type,Clipper2Lib::FillRule fill=Clipper2Lib::FillRule::EvenOdd,double scale=1e3){
    Clipper2Lib::Clipper64 op;op.AddSubject(clipper(a,scale));op.AddClip(clipper(b,scale));Clipper2Lib::Paths64 out;op.Execute(type,fill,out);return unclipper(out,scale);
}
inline void close(Path& p){if(!p.empty()&&(p.front().x!=p.back().x||p.front().y!=p.back().y))p.push_back(p.front());}
inline Polygons treeClip(const Paths& a,const Paths& b,Clipper2Lib::ClipType type,Clipper2Lib::FillRule fill=Clipper2Lib::FillRule::EvenOdd,double scale=1e3,bool clipFirst=false){
    Clipper2Lib::Clipper64 op;if(clipFirst){op.AddClip(clipper(b,scale));op.AddSubject(clipper(a,scale));}else{op.AddSubject(clipper(a,scale));op.AddClip(clipper(b,scale));}Clipper2Lib::PolyTree64 tree;op.Execute(type,fill,tree);
    Polygons result;
    auto scaled=[&](const Clipper2Lib::Path64& path){Path result;double inverse=1.0/scale;for(auto p:path)result.push_back({double(p.x)*inverse,double(p.y)*inverse});return result;};
    std::function<void(const Clipper2Lib::PolyPath64&)> visit=[&](const auto& node){
        if(!node.Polygon().empty()&&!node.IsHole()){
            Polygon p;p.exterior=scaled(node.Polygon());close(p.exterior);
            for(std::size_t i=0;i<node.Count();++i)if(node[i]->IsHole()){auto hole=scaled(node[i]->Polygon());close(hole);p.interiors.push_back(std::move(hole));}result.push_back(std::move(p));
        }
        for(std::size_t i=0;i<node.Count();++i)visit(*node[i]);
    };visit(tree);return result;
}
inline double numpySum(const double* values,std::size_t n){
    if(n<8){double result=-0.0;for(std::size_t i=0;i<n;++i)result+=values[i];return result;}
    if(n<=128){std::array<double,8> partial;for(std::size_t i=0;i<8;++i)partial[i]=values[i];std::size_t i=8;for(;i<n-(n%8);i+=8)for(std::size_t j=0;j<8;++j)partial[j]+=values[i+j];double result=((partial[0]+partial[1])+(partial[2]+partial[3]))+((partial[4]+partial[5])+(partial[6]+partial[7]));for(;i<n;++i)result+=values[i];return result;}
    std::size_t split=n/2;split-=split%8;return numpySum(values,split)+numpySum(values+split,n-split);
}
inline Path sample(const Path& p,const std::vector<double>& distances){
    if(p.empty())return {};std::vector<double> cumulative(p.size(),0),magnitudes(p.size(),0);for(std::size_t i=1;i<p.size();++i){auto v=p[i]-p[i-1];magnitudes[i]=std::sqrt(v.x*v.x+v.y*v.y);cumulative[i]=cumulative[i-1]+magnitudes[i];}
    Path out;for(auto d:distances){auto it=std::lower_bound(cumulative.begin()+1,cumulative.end(),d);std::size_t i=std::min(p.size()-1,std::size_t(it-cumulative.begin()));if(i==0){out.push_back(p.front());continue;}auto direction=p[i]-p[i-1];if(magnitudes[i]>1e-12)direction=direction/magnitudes[i];out.push_back(p[i-1]+direction*(d-cumulative[i-1]));}return out;
}
inline double length(const Path& p){std::vector<double> lengths;for(std::size_t i=1;i<p.size();++i){auto v=p[i]-p[i-1];lengths.push_back(std::sqrt(v.x*v.x+v.y*v.y));}return numpySum(lengths.data(),lengths.size());}
inline Path teeth(const Path& p,const Path& profile,int axis,double direction){
    const double total=length(p);if(profile.empty()||!(profile.back().x>0))throw std::invalid_argument("Tooth period must be positive");
    std::vector<double> x,y;const int cycles=int(std::ceil(total/profile.back().x));
    for(int i=0;i<cycles;++i)for(auto v:profile){double d=std::clamp(v.x+i*profile.back().x,0.0,total);if(x.empty()||x.back()!=d){x.push_back(d);y.push_back(v.y);}}
    auto result=sample(p,x);for(std::size_t i=0;i<result.size();++i)result[i][axis]+=direction*y[i];return result;
}
} // namespace pyslm::support::detail
