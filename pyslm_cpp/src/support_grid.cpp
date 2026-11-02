#include "pyslm/support.hpp"
#include "pyslm/support_geometry.hpp"
#include "pyslm/support_utils.hpp"
#include "support_detail.hpp"
#include <map>
#include <limits>
#include <set>

namespace pyslm::hatching {std::vector<std::size_t> numpyArgsort(const std::vector<double>&);}
namespace pyslm::support {
// Shared native outline traversal is implemented alongside support geometry.
std::vector<Path3D> meshOutline(const Mesh& mesh,int digits);
namespace {
using namespace Clipper2Lib;
const Mesh& requiredVolume(const GridBlockSupport& support){if(!support.supportVolume)throw std::invalid_argument("Support volume is not assigned");return *support.supportVolume;}
Polygon bbox(const Path& path){auto b=hatching::BaseHatcher::polygonBoundingBox(path);return {{{b[0],b[1]},{b[0],b[3]},{b[2],b[3]},{b[2],b[1]},{b[0],b[1]}},{}};}
Paths flattenedShapely(const Polygons& polygons){if(polygons.size()==1)return detail::flatten(polygons);Paths result;for(auto& p:polygons)result.push_back(p.exterior);return result;}
Path extended(const Path& p,int axis,double value){if(p.empty())return {};Path q{p.front()};q.insert(q.end(),p.begin(),p.end());q.push_back(p.back());q.push_back(p.front());q[0][axis]=q[q.size()-1][axis]=q[q.size()-2][axis]=value;return q;}
Matrix4 inverseRigid(const Matrix4& m){Matrix4 out{m[0],m[4],m[8],0,m[1],m[5],m[9],0,m[2],m[6],m[10],0,0,0,0,1};Vec3 t=detail::transform(out,{m[3],m[7],m[11]});out[3]=-t.x;out[7]=-t.y;out[11]=-t.z;return out;}
HatchLines joinHatches(HatchLines a,const HatchLines& b){a.insert(a.end(),b.begin(),b.end());return a;}
Paths thicken(const HatchLines& hatches,double width,double scale=1e5){Paths p;for(auto& l:hatches)p.push_back({{l[0].x,l[0].y},{l[1].x,l[1].y}});return detail::offset(p,width/2,JoinType::Square,EndType::Joined,scale);}
Path resample(const Path& p,double step){double length=detail::length(p);if(step>=length)return {p.front(),p.back()};std::size_t count=std::max(std::size_t(1),std::size_t(std::ceil(length/step)));std::vector<double> d;d.reserve(count);double increment=count>1?length/double(count-1):0;for(std::size_t i=0;i<count;++i)d.push_back(double(i)*increment);if(count>1)d.back()=length;return detail::sample(p,d);}
double perimeterXY(const Path3D& p){double total=0;for(std::size_t i=1;i<p.size();++i)total+=std::hypot(p[i].x-p[i-1].x,p[i].y-p[i-1].y);return total;}
bool positiveXY(const Path3D& path){Path p;for(auto v:path)p.push_back({v.x,v.y});return IsPositive(detail::clipper({p},1e3).front());}
void subdivide(Mesh& mesh){
    auto edges=meshUniqueEdges(mesh);std::map<std::array<std::uint32_t,2>,std::uint32_t> ids;auto verts=mesh.vertices;
    for(auto e:edges){ids[e]=std::uint32_t(verts.size());verts.push_back((mesh.vertices[e[0]]+mesh.vertices[e[1]])*.5);}
    std::vector<Triangle> faces;faces.reserve(mesh.faces.size()*4);
    auto edgeId=[&](std::uint32_t a,std::uint32_t b){return ids.at({std::min(a,b),std::max(a,b)});};
    for(auto f:mesh.faces){auto a=edgeId(f[0],f[1]),b=edgeId(f[1],f[2]),c=edgeId(f[2],f[0]);faces.push_back({f[0],a,c});faces.push_back({a,f[1],b});faces.push_back({c,b,f[2]});faces.push_back({a,b,c});}
    mesh.vertices=std::move(verts);mesh.faces=std::move(faces);
}
double interpolate(const std::vector<double>& x,const std::vector<double>& y,double value){
    if(x.empty())throw std::invalid_argument("Empty boundary interpolation");if(value<x.front())return y.front();if(value>x.back())return y.back();
    auto found=std::upper_bound(x.begin(),x.end(),value);auto i=std::size_t(found-x.begin());if(i==0)return y.front();if(i>=x.size())return y.back();double fraction=(value-x[i-1])/(x[i]-x[i-1]);return y[i-1]*(1-fraction)+y[i]*fraction;
}
void validateGrid(const GridBlockSupport& grid){if(!(grid.gridSpacing[0]>0)||!(grid.gridSpacing[1]>0))throw std::invalid_argument("Grid spacing must be positive");if(grid.numSkinMeshSubdivideIterations<0)throw std::invalid_argument("Subdivision count cannot be negative");}
Polygons skinInfillTree(const GridBlockSupport& grid,const Path& boundary){
    auto outer=detail::offset({boundary},10.0/1e5,JoinType::Square);if(!grid.generateTrussGrid)return detail::polygons(outer);
    auto inner=detail::offset(outer,-grid.supportBorderDistance,JoinType::Round);double diagonal=grid.gridSpacing[0]*std::sin(grid.trussAngle*detail::pi/180);
    auto box=bbox(boundary);auto hatches=joinHatches(GridBlockSupport::generateMeshGrid(box,diagonal,grid.trussAngle),GridBlockSupport::generateMeshGrid(box,diagonal,180-grid.trussAngle));
    auto trusses=thicken(hatches,grid.trussWidth);
    if(grid.useSupportBorder&&!inner.empty()){auto trimmed=detail::clip(trusses,outer,ClipType::Intersection),border=detail::clip(outer,inner,ClipType::Difference);return detail::treeClip(trimmed,border,ClipType::Union);}
    return detail::treeClip(trusses,outer,ClipType::Intersection,FillRule::NonZero);
}
}

Polygon GridBlockSupport::holeGeometry(){return {{{-1.5,0},{0,1},{1.5,0},{0,-1},{-1.5,0}},{}};}
void GridBlockSupport::setSupportWallThickness(double value){if(!std::isfinite(value)||value<std::numeric_limits<double>::epsilon())throw std::invalid_argument("The support wall thickness must be a positive value");supportWallThickness=value;}
Paths GridBlockSupport::clipLines(const Paths& boundary,const HatchLines& lines){Paths out;for(auto& l:hatching::BaseHatcher::clipLines(boundary,lines))out.push_back({{l[0].x,l[0].y},{l[1].x,l[1].y}});return out;}
HatchLines GridBlockSupport::generateMeshGrid(const Polygon& poly,double spacing,double angle){
    if(!(spacing>0))throw std::invalid_argument("Mesh grid spacing must be positive");auto b=hatching::BaseHatcher::polygonBoundingBox(poly.exterior);Vec2 centre{(b[0]+b[2])*.5,(b[1]+b[3])*.5};
    double radius=std::ceil(norm(Vec2{b[2]-centre.x,b[3]-centre.y})/spacing)*spacing;
    auto x=detail::arange(-radius,radius,spacing,true);double radians=angle*detail::pi/180,c=std::cos(radians),s=std::sin(radians);HatchLines lines;
    for(std::size_t i=0;i<x.size();++i){HatchLine l;std::array<double,2> ys{-radius,radius};for(int j=0;j<2;++j)l[j]={c*x[i]-s*ys[j]+centre.x,s*x[i]+c*ys[j]+centre.y,double(i)};lines.push_back(l);}return lines;
}
Mesh GridBlockSupport::concatenateMeshes(const std::vector<Mesh>& meshes){
    Mesh result;for(auto& m:meshes){auto offset=std::uint32_t(result.vertices.size());result.vertices.insert(result.vertices.end(),m.vertices.begin(),m.vertices.end());for(auto f:m.faces){for(auto& id:f)id+=offset;result.faces.push_back(f);}for(auto& item:m.faceAttributes){auto& dest=result.faceAttributes[item.first];dest.insert(dest.end(),item.second.begin(),item.second.end());}}return result;
}
std::vector<std::optional<PlanarSection>> GridBlockSupport::sectionMultiplane(const Mesh& mesh,Vec3 origin,Vec3 normal,const std::vector<double>& heights){std::vector<std::optional<PlanarSection>> out;for(auto h:heights){auto section=sectionMesh(mesh,origin+normal*h,normal);if(!section){PlanarSection empty;empty.to3D=inverseRigid(planeTransform(origin+normal*h,normal));section=std::move(empty);}out.push_back(std::move(section));}return out;}
std::pair<std::vector<std::optional<PlanarSection>>,std::vector<std::optional<PlanarSection>>> GridBlockSupport::generateGridSlices()const{
    validateGrid(*this);auto b=meshBounds(requiredVolume(*this));double mx=(b[0]+b[3])*.5,my=(b[1]+b[4])*.5;
    double bx=-std::ceil((mx-b[0])/gridSpacing[0])*gridSpacing[0],tx=std::ceil((b[3]-mx)/gridSpacing[0])*gridSpacing[0]+1e-8;
    double by=-std::ceil((my-b[1])/gridSpacing[1])*gridSpacing[1],ty=std::ceil((b[4]-my)/gridSpacing[1])*gridSpacing[1]+1e-8;
    return {sectionMultiplane(requiredVolume(*this),{mx,0,0},{1,0,0},detail::arange(bx,tx,gridSpacing[0])),sectionMultiplane(requiredVolume(*this),{0,my,0},{0,1,0},detail::arange(by,ty,gridSpacing[1]))};
}
Polygon GridBlockSupport::generateSliceBoundingBoxPolygon(const PlanarSection& section)const{
    auto b=meshBounds(requiredVolume(*this));const auto& m=section.to3D;Vec3 lo{b[0],b[1],b[2]},hi{b[3],b[4],b[5]};
    // Original applies inverse rotation only to the two world AABB corners.
    auto rotate=[&](Vec3 p){return Vec2{m[0]*p.x+m[4]*p.y+m[8]*p.z,m[1]*p.x+m[5]*p.y+m[9]*p.z};};auto a=rotate(lo),c=rotate(hi);
    return {{{std::min(a.x,c.x),std::min(a.y,c.y)},{std::min(a.x,c.x),std::max(a.y,c.y)},{std::max(a.x,c.x),std::max(a.y,c.y)},{std::max(a.x,c.x),std::min(a.y,c.y)},{std::min(a.x,c.x),std::min(a.y,c.y)}},{}};
}
Path GridBlockSupport::toothProfile()const{
    double a=supportTeethHeight,b=supportTeethTopLength,c=supportTeethBottomLength,d=supportTeethBaseInterval;
    Path result{{0,0},{(c-b)/2,a},{(c-b)/2+b,a},{c,0},{c+d,0}};for(auto& p:result)p.y+=-a+supportTeethUpperPenetration;return result;
}
std::optional<Polygons> GridBlockSupport::generateSliceGeometry(const PlanarSection& section)const{
    if(section.rawLines.empty())return std::nullopt;validateGrid(*this);Polygons slicePolygons;std::vector<Polygons> offsetPaths;
    auto to2D=inverseRigid(section.to3D);std::vector<std::array<Vec2,2>> lines;
    for(auto& line:section.rawLines){auto a=detail::transform(to2D,line[0]),b=detail::transform(to2D,line[1]);lines.push_back({Vec2{a.x,a.y},Vec2{b.x,b.y}});}
    auto assembled=pathFromLines2D(lines,4);
    for(auto& path:assembled.paths){
        if(path.size()<2)continue;std::vector<bool> mask;for(std::size_t i=0;i+1<path.size();++i)mask.push_back(std::abs(path[i+1].y-path[i].y)>1e-4);
        std::vector<Path> newPaths,upper,lower;
        for(std::size_t start=0;start<mask.size();){std::size_t end=start+1;while(end<mask.size()&&mask[end]==mask[start])++end;if(mask[start]){
            Path segment(path.begin()+start,path.begin()+end+1);double direction=segment.back().y<segment.front().y?-1.0:1.0;
            auto generated=detail::teeth(segment,toothProfile(),0,direction);int cycles=int(std::ceil(detail::length(segment)/toothProfile().back().x));
            if(cycles==1||(!useLowerSupportTeeth&&direction>0)||(!useUpperSupportTeeth&&direction<0))generated=segment;
            if(direction>0)lower.push_back(generated);else upper.push_back(generated);newPaths.push_back(std::move(generated));
        }start=end;}
        if(newPaths.empty())throw std::runtime_error("No nonvertical slice paths to stack");Path boundary;for(auto& p:newPaths)boundary.insert(boundary.end(),p.begin(),p.end());detail::close(boundary);
        auto filled=detail::buffer({boundary,{}},1e-5);slicePolygons.insert(slicePolygons.end(),filled.begin(),filled.end());
        if(supportWallThickness>1e-5){if(upper.empty()||lower.empty())continue;
            auto topBounds=hatching::BaseHatcher::polygonBoundingBox(upper.front()),bottomBounds=hatching::BaseHatcher::polygonBoundingBox(lower.front());
            Polygon top{extended(upper.front(),0,topBounds[0]-10),{}},bottom{extended(lower.front(),0,bottomBounds[2]+10),{}};
            try{offsetPaths.push_back(detail::wallIntersection(top,bottom,filled,supportWallThickness,1e-8));}catch(const std::exception&){continue;}
        }
    }
    if(slicePolygons.empty())return std::nullopt;
    auto paths=detail::flatten(slicePolygons);auto outer=detail::offset(paths,10.0/1e5,JoinType::Square);
    if(!generateTrussGrid)return detail::polygons(outer);
    auto inner=detail::offset(paths,-supportBorderDistance,JoinType::Round);if(inner.empty())return std::nullopt;
    auto box=generateSliceBoundingBoxPolygon(section);double diagonal=gridSpacing[0]*std::sin(trussAngle*detail::pi/180);
    auto hatches=joinHatches(generateMeshGrid(box,diagonal,trussAngle),generateMeshGrid(box,diagonal,180-trussAngle));
    auto trimmed=clipLines(outer,hatches);auto trusses=detail::offset(trimmed,trussWidth/2,JoinType::Square,EndType::Joined);
    auto solution=detail::clip(trusses,outer,ClipType::Intersection,FillRule::NonZero);
    if(supportWallThickness>1e-5){Paths walls;for(auto& p:offsetPaths){auto q=flattenedShapely(p);walls.insert(walls.end(),q.begin(),q.end());}solution=detail::clip(walls,solution,ClipType::Union);}
    if(useSupportBorder){auto border=detail::clip(outer,inner,ClipType::Difference);return detail::treeClip(solution,border,ClipType::Union);}
    return detail::treeClip(solution,{},ClipType::Union);
}
Paths GridBlockSupport::generateSupportSkinInfill(const Path& boundary,bool returnPolyNodes)const{
    if(returnPolyNodes)return detail::flatten(skinInfillTree(*this,boundary));
    auto outer=detail::offset({boundary},10.0/1e5,JoinType::Square);if(!generateTrussGrid)return outer;
    auto inner=detail::offset(outer,-supportBorderDistance,JoinType::Round);double diagonal=gridSpacing[0]*std::sin(trussAngle*detail::pi/180);
    auto box=bbox(boundary);auto hatches=joinHatches(generateMeshGrid(box,diagonal,trussAngle),generateMeshGrid(box,diagonal,180-trussAngle));
    auto trusses=thicken(hatches,trussWidth);
    if(useSupportBorder&&!inner.empty()){
        auto trimmed=detail::clip(trusses,outer,ClipType::Intersection),border=detail::clip(outer,inner,ClipType::Difference);
        return detail::clip(trimmed,border,ClipType::Union);
    }
    return detail::clip(trusses,outer,ClipType::Intersection,FillRule::NonZero);
}
Polygons GridBlockSupport::generateSliceGeometryDepr(const Polygons& input)const{
    Polygons result;detail::Context c;
    for(auto& p:input){auto b=hatching::BaseHatcher::polygonBoundingBox(p.exterior);std::vector<GEOSGeometry*> holes;int i=1;
        for(auto x:detail::arange(b[0],b[2],2.25)){++i;for(auto y:detail::arange(b[1],b[3],3)){auto hole=holeGeometry();for(auto& v:hole.exterior){v.x+=x;v.y+=y+(i%2?0:1.5);}holes.push_back(detail::native(c.h,hole).release());}}
        auto collection=detail::own(c.h,GEOSGeom_createCollection_r(c.h,GEOS_GEOMETRYCOLLECTION,holes.data(),unsigned(holes.size())));auto united=detail::own(c.h,GEOSUnaryUnion_r(c.h,collection.get()));auto original=detail::native(c.h,p);auto cut=detail::own(c.h,GEOSDifference_r(c.h,original.get(),united.get()));
        if(useSupportBorder){auto inner=detail::own(c.h,GEOSBuffer_r(c.h,original.get(),-supportBorderDistance,16));auto border=detail::own(c.h,GEOSDifference_r(c.h,original.get(),inner.get()));cut=detail::own(c.h,GEOSUnion_r(c.h,border.get(),cut.get()));}
        auto q=detail::extract(c.h,cut.get());result.insert(result.end(),q.begin(),q.end());
    }
    // The legacy Python method returns a processed Path2D; its discrete
    // traversal makes every ring CCW before polygons_full builds containment.
    auto paths=detail::flatten(result);for(auto& path:paths){double area=0;for(std::size_t i=0;i<path.size();++i){auto a=path[i],b=path[(i+1)%path.size()];area+=(b.x-a.x)*(b.y+a.y);}if(area>=0)std::reverse(path.begin(),path.end());}
    return detail::polygons(paths);
}
std::pair<Mesh,Mesh> GridBlockSupport::generateSupportSlices()const{
    auto sections=generateGridSlices();std::vector<Mesh> x,y;std::size_t order=0;
    auto generate=[&](const std::vector<std::optional<PlanarSection>>& list,int type,std::vector<Mesh>& target){for(auto& section:list){if(!section)continue;auto polygons=generateSliceGeometry(*section);if(!polygons)continue;std::vector<Mesh> pieces;
        for(auto& p:*polygons){auto tri=triangulatePolygonFromPaths(p.exterior,p.interiors,"pa4.000");Mesh mesh;mesh.faces=std::move(tri.faces);for(auto v:tri.vertices)mesh.vertices.push_back({v.x,v.y,0});pieces.push_back(std::move(mesh));}
        auto mesh=processMesh(concatenateMeshes(pieces));if(mesh.faces.empty())continue;
        mesh.faceAttributes["order"]=std::vector<double>(mesh.faces.size(),double(order++));mesh.faceAttributes["type"]=std::vector<double>(mesh.faces.size(),double(type));detail::transform(mesh,section->to3D);target.push_back(std::move(mesh));}};
    generate(sections.first,1,x);generate(sections.second,2,y);return {concatenateMeshes(x),concatenateMeshes(y)};
}
std::vector<Mesh> GridBlockSupport::borderGeometry()const{return generateSupportSkins();}
std::pair<Mesh,Mesh> GridBlockSupport::sliceGeometry()const{return generateSupportSlices();}
Mesh GridBlockSupport::geometry()const{
    auto slices=generateSupportSlices();auto skins=useSupportSkin?generateSupportSkins():std::vector<Mesh>{};
    if(mergeMesh)throw std::logic_error("Currently performing a boolean merge between shell meshes is not available - please use the non-merged mesh");
    double maxId=0;auto found=slices.second.faceAttributes.find("order");if(found!=slices.second.faceAttributes.end()&&!found->second.empty())maxId=*std::max_element(found->second.begin(),found->second.end());++maxId;
    for(std::size_t i=0;i<skins.size();++i){skins[i].faceAttributes["order"]=std::vector<double>(skins[i].faces.size(),maxId+double(i));skins[i].faceAttributes["type"]=std::vector<double>(skins[i].faces.size(),0);}
    std::vector<Mesh> all{slices.first,slices.second};all.insert(all.end(),skins.begin(),skins.end());return concatenateMeshes(all);
}
std::vector<Mesh> GridBlockSupport::generateSupportSkins(std::map<std::string,Paths>* diagnostics)const{
    validateGrid(*this);auto mesh=requiredVolume(*this);if(meshVolume(mesh)<0)for(auto& f:mesh.faces)std::swap(f[0],f[2]);mergeMeshVertices(mesh,3);
    auto weights=getFaceZProjectionWeight(mesh,false),areas=meshFaceAreas(mesh);std::vector<std::size_t> ids;
    for(std::size_t i=0;i<areas.size();++i)if(areas[i]>1e-6&&weights[i]<BlockSupportGenerator::supportSkinSideTolerance)ids.push_back(i);
    auto surfaces=splitMeshComponents(selectMeshFaces(mesh,ids,false));std::vector<Mesh> significant;
    for(auto& s:surfaces){auto a=meshFaceAreas(s);if(std::accumulate(a.begin(),a.end(),0.0)>5)significant.push_back(std::move(s));}
    if(significant.size()!=2)return {};Mesh top=significant[0],bottom=significant[1];if(meshBounds(bottom)[5]>meshBounds(top)[5])std::swap(top,bottom);
    auto topPaths=meshOutline(top,3),bottomPaths=meshOutline(bottom,3);if(topPaths.size()!=bottomPaths.size())return {};
    std::vector<std::pair<std::size_t,std::size_t>> pairs;for(std::size_t i=0;i<topPaths.size();++i)for(std::size_t j=0;j<bottomPaths.size();++j){double b=perimeterXY(bottomPaths[j]);if(std::abs(perimeterXY(topPaths[i])-b)/b<pairTolerance)pairs.push_back({i,j});}
    std::vector<Mesh> result;
    for(auto pair:pairs){
        auto topPath=topPaths[pair.first],bottomPath=bottomPaths[pair.second];if(!positiveXY(topPath))std::reverse(topPath.begin(),topPath.end());if(!positiveXY(bottomPath))std::reverse(bottomPath.begin(),bottomPath.end());
        std::size_t nearest=0;double best=1e100;for(std::size_t i=0;i<bottomPath.size();++i){double d=std::hypot(bottomPath[i].x-topPath.front().x,bottomPath[i].y-topPath.front().y);if(d<best){best=d;nearest=i;}}
        std::rotate(bottomPath.begin(),bottomPath.begin()+nearest,bottomPath.end());bottomPath.push_back(bottomPath.front());
        std::vector<double> arc,topX,topY;Path topProfile,bottomProfile;double distance=0;
        for(std::size_t i=0;i<topPath.size();++i){if(i)distance+=std::hypot(topPath[i].x-topPath[i-1].x,topPath[i].y-topPath[i-1].y);arc.push_back(distance);topX.push_back(topPath[i].x);topY.push_back(topPath[i].y);topProfile.push_back({distance,topPath[i].z});}
        distance=0;for(std::size_t i=0;i<bottomPath.size();++i){if(i)distance+=std::hypot(bottomPath[i].x-bottomPath[i-1].x,bottomPath[i].y-bottomPath[i-1].y);bottomProfile.push_back({distance,bottomPath[i].z});}std::reverse(bottomProfile.begin(),bottomProfile.end());
        Path topTeeth=useUpperSupportTeeth?detail::teeth(topProfile,toothProfile(),1,1):topProfile,bottomTeeth=useLowerSupportTeeth?detail::teeth(bottomProfile,toothProfile(),1,1):bottomProfile;
        if(diagnostics){(*diagnostics)["topPolyVerts"].push_back(topProfile);(*diagnostics)["bottomPolyVerts"].push_back(bottomProfile);(*diagnostics)["teethFinalTop"].push_back(topTeeth);(*diagnostics)["teethFinalBottom"].push_back(bottomTeeth);}
        Path boundary=topTeeth;boundary.insert(boundary.end(),bottomTeeth.begin(),bottomTeeth.end());if(diagnostics)(*diagnostics)["myPolyVertsBeforeResample"].push_back(boundary);boundary=resample(boundary,.25);if(diagnostics)(*diagnostics)["myPolyVerts"].push_back(boundary);
        auto infill=generateSupportSkinInfill(boundary,false);Polygons polygons;
        if(diagnostics)(*diagnostics)["infillSolution"]=infill;
        if(supportWallThickness>1e-5){auto topBounds=hatching::BaseHatcher::polygonBoundingBox(topProfile),bottomBounds=hatching::BaseHatcher::polygonBoundingBox(bottomProfile);
            Polygon a{extended(topProfile,1,topBounds[3]+10),{}},b{extended(bottomProfile,1,bottomBounds[1]-10),{}};Polygons walls;
            walls=detail::wallIntersection(a,b,Polygons{{boundary,{}}},supportWallThickness);
            if(diagnostics)(*diagnostics)["isectPolyC"]=detail::flatten(walls);
            Paths closed=infill;for(auto& p:closed)p.push_back(p.front());polygons=detail::treeClip(closed,flattenedShapely(walls),ClipType::Union,FillRule::NonZero,1e3,true);
        }else polygons=skinInfillTree(*this,boundary);
        auto rings=sortExteriorInteriorRings(polygons,true);if(diagnostics){(*diagnostics)["exterior"]=rings.first;(*diagnostics)["interior"]=rings.second;}if(rings.first.size()>1)throw std::runtime_error("Error: exterior count > 1. Increase the support border distance to resolve this issue.");if(rings.first.empty())throw std::out_of_range("No support skin exterior");
        auto tri=triangulatePolygonFromPaths(rings.first.front(),rings.second,"pa4.000");Mesh flat;flat.faces=std::move(tri.faces);for(auto p:tri.vertices)flat.vertices.push_back({p.x,p.y,0});flat=processMesh(flat,true);
        for(int i=0;i<numSkinMeshSubdivideIterations;++i)subdivide(flat);
        for(auto& v:flat.vertices)v={interpolate(arc,topX,v.x),interpolate(arc,topY,v.x),v.y};result.push_back(processMesh(flat));
    }return result;
}
std::pair<Paths,Paths> GridBlockSupport::slice(const std::vector<Mesh>& supports,double z){
    Paths interior,border;
    for(auto& mesh:supports){auto order=mesh.faceAttributes.find("order"),type=mesh.faceAttributes.find("type");if(order==mesh.faceAttributes.end()||type==mesh.faceAttributes.end())continue;
        auto section=meshPlane(mesh,{0,0,z},{0,0,1});std::map<double,std::vector<std::size_t>> groups;
        for(std::size_t i=0;i<section.second.size();++i)groups[order->second.at(section.second[i])].push_back(i);
        for(auto& group:groups){auto& indices=group.second;int kind=int(type->second.at(section.second[indices.front()]));Paths entities;
            if(kind==1||kind==2){Path p;std::vector<double> keys;for(auto i:indices)for(auto v:section.first[i]){p.push_back({v.x,v.y});keys.push_back(kind==1?v.y:v.x);}auto sorted=hatching::numpyArgsort(keys);for(std::size_t i=0;i+1<sorted.size();i+=2)entities.push_back({p[sorted[i]],p[sorted[i+1]]});}
            else {std::vector<std::array<Vec2,2>> lines;for(auto i:indices)lines.push_back({Vec2{section.first[i][0].x,section.first[i][0].y},Vec2{section.first[i][1].x,section.first[i][1].y}});auto loaded=pathFromLines2D(lines,5);for(auto& entity:loaded.entities){Path p;for(auto i:entity)p.push_back(loaded.vertices[i]);entities.push_back(std::move(p));}}
            if(entities.empty())continue;Path current=entities.front();entities.erase(entities.begin());Paths connected{current};
            while(!entities.empty()){double best=1e8;std::size_t selected=entities.size()-1;bool reverse=false;auto end=current.back();for(std::size_t i=0;i<entities.size();++i){double a=norm(entities[i].front()-end),b=norm(entities[i].back()-end);if(a<best){best=a;selected=i;reverse=false;}if(b<best){best=b;selected=i;reverse=true;}}
                // The original appends the previous entity a second time here.
                connected.push_back(current);current=entities[selected];if(reverse)std::reverse(current.begin(),current.end());entities.erase(entities.begin()+selected);
            }connected.push_back(current);auto& dest=kind==0?border:interior;dest.insert(dest.end(),connected.begin(),connected.end());
        }
    }return {interior,border};
}
GridBlockSupportGenerator::GridBlockSupportGenerator(){useLowerSupportTeeth=true;supportWallThickness=3.0;}
std::vector<GridBlockSupport> GridBlockSupportGenerator::identifySupportRegions(const Part& part,double angle,bool selfIntersecting)const{
    auto blocks=BlockSupportGenerator::identifySupportRegions(part,angle,selfIntersecting);std::vector<GridBlockSupport> result;
    for(auto& block:blocks){GridBlockSupport grid(block.supportObject,block.supportVolume,block.supportSurface,block.intersectsPart);static_cast<GridSupportParameters&>(grid)=static_cast<const GridSupportParameters&>(*this);grid.setSupportWallThickness(supportWallThickness);grid.supportTeethTopLength=supportTeethHeight;result.push_back(std::move(grid));}return result;
}
} // namespace pyslm::support
