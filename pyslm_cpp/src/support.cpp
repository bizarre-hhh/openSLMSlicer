#include "pyslm/support.hpp"
#include "pyslm/support_geometry.hpp"
#include "pyslm/support_utils.hpp"
#include "pyslm/support_render.hpp"
#include "pyslm/spline.hpp"
#include "support_detail.hpp"
#include <iomanip>
#include <map>
#include <sstream>

namespace pyslm::support {
namespace {
const Mesh& required(const std::optional<Mesh>& mesh,const char* name){if(!mesh)throw std::invalid_argument(std::string(name)+" is not assigned");return *mesh;}
std::string triangleArgs(double area){std::ostringstream s;s<<"pa"<<std::fixed<<std::setprecision(3)<<area;return s.str();}
Path xy(const Mesh& mesh){Path result;for(auto p:mesh.vertices)result.push_back({p.x,p.y});return result;}
Mesh shifted(Mesh mesh,double z){for(auto& p:mesh.vertices)p.z+=z;return mesh;}
void invertIfNegative(Mesh& mesh){if(meshVolume(mesh)<0)for(auto& f:mesh.faces)std::swap(f[0],f[2]);}
Polygon roundedPath(const Polygon& p,int digits){
    auto round=[&](const Path& path){Path out;double scale=std::pow(10.0,digits);std::map<std::pair<std::int64_t,std::int64_t>,Vec2> first;for(auto v:path){auto id=std::make_pair(std::int64_t(std::nearbyint(v.x*scale)),std::int64_t(std::nearbyint(v.y*scale)));auto inserted=first.emplace(id,v);Vec2 q=inserted.first->second;if(out.empty()||norm(q-out.back())>0)out.push_back(q);}return out;};
    Polygon result{round(p.exterior),{}};for(auto& hole:p.interiors)result.interiors.push_back(round(hole));return result;
}
bool counterclockwise(const Path& path){double area=0;for(std::size_t i=0;i<path.size();++i){auto a=path[i],b=path[(i+1)%path.size()];area+=(b.x-a.x)*(b.y+a.y);}return area<0;}
Polygons smoothPolygonSplines(const Polygon& polygon,double smooth){
    struct Curve {bool circle=false;SplineCurve spline;Path controls;};
    auto source=detail::flatten({polygon});auto bounds=hatching::BaseHatcher::boundaryBoundingBox(source);
    double sourceScale=std::sqrt((bounds[2]-bounds[0])*(bounds[2]-bounds[0])+(bounds[3]-bounds[1])*(bounds[3]-bounds[1]));
    std::vector<Curve> curves;
    for(auto path:source){if(!counterclockwise(path))std::reverse(path.begin(),path.end());auto circle=fitClosedCircle(path,sourceScale);Curve curve;curve.circle=circle.has_value();if(circle)curve.controls=std::move(*circle);else{curve.spline=fitSpline(path,smooth);curve.controls=curve.spline.control;}curves.push_back(std::move(curve));}
    auto curveBounds=[&](){Paths samples;for(auto& c:curves){if(c.circle){auto centre=(c.controls.front()+c.controls.back())*.5;double radius=norm(c.controls.front()-centre);samples.push_back({{centre.x-radius,centre.y-radius},{centre.x+radius,centre.y+radius}});}else samples.push_back(c.controls);}return hatching::BaseHatcher::boundaryBoundingBox(samples);};
    auto scaleFor=[&](const Bounds2& b){return std::sqrt((b[2]-b[0])*(b[2]-b[0])+(b[3]-b[1])*(b[3]-b[1]));};
    double newScale=scaleFor(curveBounds());
    // Path2D.process merges controls by the scale-dependent path tolerance,
    // retaining representative input coordinates and the knot/control indexing.
    int digits=std::clamp(std::abs(int(std::log10(1e-5*newScale))),1,20);double factor=std::pow(10.0,digits);
    std::map<std::pair<std::int64_t,std::int64_t>,Vec2> first;
    for(auto& c:curves)for(auto& p:c.controls){auto key=std::make_pair(std::int64_t(std::nearbyint(p.x*factor)),std::int64_t(std::nearbyint(p.y*factor)));p=first.emplace(key,p).first->second;}
    newScale=scaleFor(curveBounds());Paths discrete;
    for(auto& c:curves){Path path;if(c.circle)path=discretizeClosedCircle(c.controls,newScale);else{c.spline.control=c.controls;path=discretizeSpline(c.spline,newScale);}if(!counterclockwise(path))std::reverse(path.begin(),path.end());discrete.push_back(std::move(path));}
    return detail::polygons(discrete);
}
}
SupportStructure::SupportStructure(const Part* object,std::optional<Mesh> mesh,std::optional<Mesh> surface,bool intersects)
    :supportObject(object),supportVolume(std::move(mesh)),supportSurface(std::move(surface)),intersectsPart(intersects){}
Polygon SupportStructure::flattenSupportRegion(const Mesh& region){auto p=support::flattenSupportRegion(region);if(p.empty())throw std::out_of_range("Support region has no flattened polygon");return p.front();}
double SupportStructure::projectedSupportArea() const{return supportSurface?detail::area(flattenSupportRegion(*supportSurface)):0.0;}
double SupportStructure::supportArea() const {if(!supportSurface)return 0;auto areas=meshFaceAreas(*supportSurface);return std::accumulate(areas.begin(),areas.end(),0.0);}
Mesh BlockSupportBase::geometry() const{return required(supportVolume,"Support volume");}
double BlockSupportBase::volume() const{return meshVolume(required(supportVolume,"Support volume"));}
Mesh BlockSupportBase::supportBoundary() const{
    auto& mesh=required(supportVolume,"Support volume");auto weights=getFaceZProjectionWeight(mesh);std::vector<std::size_t> faces;
    for(std::size_t i=0;i<weights.size();++i)if(weights[i]>(1.0-1e-4))faces.push_back(i);
    auto boundary=selectMeshFaces(mesh,faces,false);
    // update_faces followed by remove_unreferenced_vertices preserves the
    // original order of the surviving vertices and of the selected faces.
    std::vector<bool> used(mesh.vertices.size());for(auto f:boundary.faces)for(auto i:f)used.at(i)=true;
    std::vector<std::uint32_t> remap(mesh.vertices.size());boundary.vertices.clear();
    for(std::size_t i=0;i<used.size();++i)if(used[i]){remap[i]=static_cast<std::uint32_t>(boundary.vertices.size());boundary.vertices.push_back(mesh.vertices[i]);}
    for(auto& f:boundary.faces)for(auto& i:f)i=remap[i];
    for(auto& [name,values]:mesh.faceAttributes){if(values.size()!=mesh.faces.size())throw std::invalid_argument("Face attribute length differs from mesh face count");auto& selected=boundary.faceAttributes[name];for(auto i:faces)selected.push_back(values[i]);}
    return boundary;
}
Mesh BlockSupportBase::triangulateSections(const std::vector<std::optional<PlanarSection>>& sections){
    std::vector<Mesh> meshes;
    for(auto& section:sections){if(!section)continue;auto polygons=detail::polygons(section->paths);if(polygons.empty())continue;auto tri=triangulatePolygonEarcut(polygons,true);Mesh mesh;mesh.faces=std::move(tri.faces);for(auto p:tri.vertices)mesh.vertices.push_back({p.x,p.y,0});mesh=processMesh(mesh);detail::transform(mesh,section->to3D);meshes.push_back(std::move(mesh));}
    return GridBlockSupport::concatenateMeshes(meshes);
}
std::vector<std::size_t> BaseSupportGenerator::findOverhangPoints(const Part& part){
    const auto& mesh=part.geometry();std::vector<std::vector<std::uint32_t>> adjacent(mesh.vertices.size());
    for(auto e:meshUniqueEdges(mesh)){adjacent[e[0]].push_back(e[1]);adjacent[e[1]].push_back(e[0]);}
    auto normals=meshVertexNormals(mesh);std::vector<std::size_t> result;
    for(std::size_t i=0;i<adjacent.size();++i){bool above=true;for(auto j:adjacent[i])if(mesh.vertices[j].z-mesh.vertices[i].z<=-POINT_OVERHANG_TOLERANCE){above=false;break;}if(above&&normals[i].z<0)result.push_back(i);}
    return result;
}
std::vector<std::array<std::uint32_t,2>> BaseSupportGenerator::findOverhangEdges(const Part& part,double angle,double edgeAngle){
    const auto& mesh=part.geometry();auto edges=meshUniqueEdges(mesh);auto adjacent=meshFaceAdjacency(mesh);auto normals=meshFaceNormals(mesh);
    auto theta=getSupportAngles(part,Vec3{0,0,1});std::vector<double> adjacentAngles;
    for(auto f:adjacent)adjacentAngles.push_back(std::acos(std::clamp(dot(normals[f[0]],normals[f[1]]),-1.0,1.0))*180/detail::pi);
    std::vector<std::array<std::uint32_t,2>> result;
    for(std::size_t i=0;i<edges.size();++i){auto d=mesh.vertices[edges[i][0]]-mesh.vertices[edges[i][1]];double a=std::asin(d.z/norm(d))*180/detail::pi;if(std::abs(a)<edgeAngle){if(i>=adjacent.size())throw std::out_of_range("Original findOverhangEdges indexes face adjacency by unique edge index");auto f=adjacent[i];if(adjacentAngles[i]>angle&&theta[f[0]]>89&&theta[f[1]]>89)result.push_back(edges[i]);}}
    return result;
}
double BlockSupportGenerator::gradThreshold(double distance,double angle){return 5.0*std::tan(angle*detail::pi/180.0)*distance;}
void BlockSupportGenerator::filterSupportRegion(const Mesh&)const{throw std::logic_error("Not Implemented (original PySLM filterSupportRegion)");}
void BlockSupportGenerator::generateIntersectionHeightMap()const{throw std::logic_error("Not Implemented (original PySLM generateIntersectionHeightMap)");}
std::vector<BlockSupportBase> BlockSupportGenerator::identifySupportRegions(const Part& part,double angle,bool selfIntersecting)const{
    auto regions=getOverhangMeshes(part,angle,false);std::vector<BlockSupportBase> result;
    for(auto& region:regions){
        Polygon projection;try{projection=SupportStructure::flattenSupportRegion(region);}catch(const std::exception&){continue;}
        auto offsets=detail::simplifyBuffer(projection,simplifyPolygonFactor,false,-outerSupportEdgeGap);
        double offsetArea=0;for(auto& p:offsets)offsetArea+=detail::area(p);if(offsetArea<minimumAreaThreshold)continue;
        auto extruded=extrudeTriangulation(xy(region),region.faces,100);
        std::size_t index=0;for(auto& p:extruded.vertices)if(p.z>1.0){if(index>=region.vertices.size())throw std::runtime_error("Extruded upper surface count differs from original support surface");p.z=region.vertices[index++].z-.01;}
        auto bounds=meshBounds(extruded);auto cut=boolIntersect(part.geometry(),extruded);const double cutVolume=meshVolume(cut);
        if(cutVolume<intersectionVolumeTolerance){if(useApproxBasePlateSupport){result.emplace_back(&part,extruded,region,false);continue;}}
        else if(!selfIntersecting)continue;
        auto normals=meshFaceNormals(cut);std::vector<std::size_t> upperFaces;
        for(std::size_t i=0;i<normals.size();++i)if(std::acos(std::clamp(normals[i].z,-1.0,1.0))*180/detail::pi<89.95)upperFaces.push_back(i);
        auto upperCut=selectMeshFaces(cut,upperFaces,false);
        auto renderBounds=bounds;renderBounds[2]-=1;renderBounds[5]+=1;
        auto upperImage=projectHeightMap(shifted(region,1000),rayProjectionResolution,false,renderBounds);
        auto lowerImage=flipHeightMapVertical(projectHeightMap(shifted(upperCut,1000),rayProjectionResolution,true,renderBounds));
        if(upperImage.width!=lowerImage.width||upperImage.height!=lowerImage.height)throw std::runtime_error("Support projection image dimensions differ");
        auto heightMap=upperImage;for(std::size_t i=0;i<heightMap.values.size();++i)if(lowerImage.values[i]>1.01)heightMap.values[i]=lowerImage.values[i];
        heightMap=padHeightMap(transposeHeightMap(heightMap),2,1);
        auto gradients=heightMapGradientMagnitude(heightMap,gaussianBlurSigma);auto mask=heightMap;for(auto& value:mask.values)value=value>999?1.0:0.0;
        auto outlines=findHeightMapContours(gradients,gradThreshold(rayProjectionResolution,overhangAngle),&mask);
        for(auto& path:outlines)for(auto& p:path){p.x=p.x*rayProjectionResolution+bounds[0];p.y=p.y*rayProjectionResolution+bounds[1];}
        auto enclosed=detail::polygons(outlines);Polygons candidates;
        for(auto& outline:enclosed){
            auto rounded=roundedPath(outline,4);
            auto smoothed=splineSimplificationFactor?smoothPolygonSplines(rounded,*splineSimplificationFactor):Polygons{rounded};
            if(smoothed.size()>1)throw std::runtime_error("MultiPolygon error please submit a bug report");
            for(auto& shape:smoothed){auto q=detail::simplifyBuffer(shape,simplifyPolygonFactor*rayProjectionResolution,true,-innerSupportEdgeGap);candidates.insert(candidates.end(),q.begin(),q.end());}
        }
        for(auto& poly:candidates){
            if(detail::area(poly)<minimumAreaThreshold)continue;
            auto tri=triangulateShapelyPolygon(poly,triangleArgs(triangulationSpacing));
            std::vector<Vec3> upper,lower;std::vector<bool> upperHit;std::size_t numUpper=0,numLower=0;
            for(auto p:tri.vertices){Vec3 origin{p.x,p.y,-1e-7};auto hit=firstRayIntersection(region,origin,{0,0,1});upperHit.push_back(hit.has_value());if(hit){origin.z=hit->z+upperProjectionOffset;++numUpper;}upper.push_back(origin);lower.push_back({origin.x,origin.y,0});}
            if(cutVolume>intersectionVolumeTolerance)for(std::size_t i=0;i<upper.size();++i){auto hit=firstRayIntersection(upperCut,upper[i],{0,0,-1});if(hit){lower[i].z=hit->z-lowerProjectionOffset;++numLower;}}
            if(numUpper!=upper.size()||numLower!=numUpper){if(numLower!=0)continue;for(auto& p:lower)p.z=0;}
            Mesh surface;surface.vertices=upper;surface.faces=tri.faces;surface=processMesh(surface);
            auto block=extrudeTriangulation(xy(surface),surface.faces,100);std::size_t low=0,high=0;
            for(auto& p:block.vertices){if(p.z<1){if(low>=upper.size())throw std::runtime_error("Support upper extrusion indexing mismatch");p=upper[low++];}else if(p.z>1){if(high>=lower.size())throw std::runtime_error("Support lower extrusion indexing mismatch");p=lower[high++];}}
            invertIfNegative(block);block=processMesh(block);
            auto clean=boolDiff(block,cutVolume<intersectionVolumeTolerance?part.geometry():cut);
            result.emplace_back(&part,std::move(clean),region,true);
        }
    }
    return result;
}
} // namespace pyslm::support
