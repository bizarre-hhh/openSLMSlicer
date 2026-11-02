#include "pyslm/core.hpp"
#include <geos_c.h>
#include <libqhull_r/qhull_ra.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace pyslm {
namespace {
constexpr double mergeTolerance=1e-8;
constexpr double pi=3.1415926535897932384626433832795;
using Edge=std::array<std::uint32_t,2>;
using IndexPath=std::vector<std::uint32_t>;
Edge edgeKey(std::uint32_t a,std::uint32_t b) { return {std::min(a,b),std::max(a,b)}; }
bool same(Vec3 a,Vec3 b) { return a.x==b.x && a.y==b.y && a.z==b.z; }
bool finite(Vec3 p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
void checkMesh(const Mesh& mesh) {
    for(const auto& face:mesh.faces) for(auto i:face)
        if(i>=mesh.vertices.size()) throw std::out_of_range("Triangle references invalid vertex");
}
Bounds3 meshBounds(const Mesh& mesh) {
    if(mesh.faces.empty()) throw std::runtime_error("Mesh has no referenced triangle vertices");
    const auto p=mesh.vertices.at(mesh.faces[0][0]);
    Bounds3 bounds{p.x,p.y,p.z,p.x,p.y,p.z};
    for(const auto& f:mesh.faces) for(auto index:f) {
        const auto v=mesh.vertices.at(index);
        for(int i=0;i<3;++i) { if(std::isnan(v[i]))bounds[i]=bounds[i+3]=v[i];else {bounds[i]=std::min(bounds[i],v[i]);bounds[i+3]=std::max(bounds[i+3],v[i]);} }
    }
    return bounds;
}
double signedVolume(const Mesh& mesh) {
    // Same first integral as trimesh.triangles.mass_properties.
    double volume=0.0;
    for(const auto& f:mesh.faces) {
        const auto a=mesh.vertices[f[0]],b=mesh.vertices[f[1]],c=mesh.vertices[f[2]];
        volume+=cross(b-a,c-a).x*((a.x+b.x)+c.x);
    }
    return volume/6.0;
}
struct EdgeUse { std::size_t face; std::uint32_t start,end; };
std::map<Edge,std::vector<EdgeUse>> edgeUses(const Mesh& mesh) {
    std::map<Edge,std::vector<EdgeUse>> uses;
    for(std::size_t i=0;i<mesh.faces.size();++i) for(int j=0;j<3;++j) {
        const auto a=mesh.faces[i][j],b=mesh.faces[i][(j+1)%3];
        uses[edgeKey(a,b)].push_back({i,a,b});
    }
    return uses;
}
bool watertight(const Mesh& mesh,bool requireWinding=false) {
    if(mesh.faces.empty()) return false;
    for(const auto& item:edgeUses(mesh)) {
        const auto& uses=item.second;
        if(uses.size()!=2) return false;
        if(requireWinding && uses[0].start!=uses[1].end) return false;
    }
    return true;
}
void mergeVerticesImpl(Mesh& mesh,int digits=8) {
    if(digits<0||digits>15)throw std::invalid_argument("Mesh merge digits must be in [0,15]");
    const double factor=std::pow(10.0,digits);
    std::vector<bool> referenced(mesh.vertices.size(),mesh.faces.empty());
    for(const auto& face:mesh.faces) for(auto id:face) referenced[id]=true;
    std::map<std::array<std::int64_t,3>,std::uint32_t> seen;
    std::vector<Vec3> vertices;
    std::vector<std::uint32_t> inverse(mesh.vertices.size());
    for(std::size_t i=0;i<mesh.vertices.size();++i) if(referenced[i]) {
        const auto p=mesh.vertices[i];
        if(!finite(p)) throw std::invalid_argument("Cannot merge nonfinite vertices");
        std::array<std::int64_t,3> key{};
        for(int j=0;j<3;++j) key[j]=static_cast<std::int64_t>(std::nearbyint(p[j]*factor));
        auto found=seen.find(key);
        if(found==seen.end()) { const auto id=static_cast<std::uint32_t>(vertices.size()); seen[key]=id; inverse[i]=id; vertices.push_back(p); }
        else inverse[i]=found->second;
    }
    for(auto& face:mesh.faces) for(auto& id:face) id=inverse[id];
    mesh.vertices=std::move(vertices);
}
void fixMesh(Mesh& mesh) {
    std::set<Triangle> seen;
    std::vector<Triangle> faces;
    std::vector<std::size_t> kept;
    for(std::size_t faceId=0;faceId<mesh.faces.size();++faceId) {
        const auto& face=mesh.faces[faceId];
        const auto a=mesh.vertices[face[0]],b=mesh.vertices[face[1]],c=mesh.vertices[face[2]];
        if(!finite(a)||!finite(b)||!finite(c)) continue;
        const double lengthA=norm(b-a),lengthB=norm(c-a),twiceArea=norm(cross(b-a,c-a));
        // trimesh.triangles.extents uses the two edges starting at vertex 0.
        if(lengthA<=mergeTolerance || lengthB<=mergeTolerance ||
           twiceArea/lengthA<=mergeTolerance || twiceArea/lengthB<=mergeTolerance) continue;
        auto key=face; std::sort(key.begin(),key.end());
        if(seen.insert(key).second) { faces.push_back(face); kept.push_back(faceId); }
    }
    for(auto& attribute:mesh.faceAttributes) {
        if(attribute.second.size()!=mesh.faces.size())throw std::invalid_argument("Face attribute length must match face count");
        std::vector<double> values;values.reserve(kept.size());for(auto id:kept)values.push_back(attribute.second[id]);attribute.second=std::move(values);
    }
    mesh.faces=std::move(faces);
    // Orient connected triangles by shared edges; preserve face row order.
    const auto uses=edgeUses(mesh);
    std::vector<std::vector<std::size_t>> adjacency(mesh.faces.size());
    for(const auto& item:uses) if(item.second.size()==2) {
        const auto a=item.second[0].face,b=item.second[1].face;
        adjacency[a].push_back(b); adjacency[b].push_back(a);
    }
    std::vector<bool> visited(mesh.faces.size());
    for(std::size_t root=0;root<mesh.faces.size();++root) if(!visited[root]) {
        visited[root]=true; std::queue<std::size_t> queue; queue.push(root);
        while(!queue.empty()) {
            const auto parent=queue.front(); queue.pop();
            for(auto child:adjacency[parent]) if(!visited[child]) {
                visited[child]=true; queue.push(child);
                bool reverse=false;
                for(int i=0;i<3;++i) for(int j=0;j<3;++j)
                    if(mesh.faces[parent][i]==mesh.faces[child][j] &&
                       mesh.faces[parent][(i+1)%3]==mesh.faces[child][(j+1)%3]) reverse=true;
                if(reverse) std::reverse(mesh.faces[child].begin(),mesh.faces[child].end());
            }
        }
    }
    if(watertight(mesh) && signedVolume(mesh)<0.0)
        for(auto& face:mesh.faces) std::reverse(face.begin(),face.end());
    mergeVerticesImpl(mesh);
}

struct Geos {
    GEOSContextHandle_t context=GEOS_init_r();
    Geos() { if(!context) throw std::runtime_error("GEOS context could not be created"); }
    ~Geos() { GEOS_finish_r(context); }
    using Ptr=std::unique_ptr<GEOSGeometry,std::function<void(GEOSGeometry*)>>;
    Ptr hold(GEOSGeometry* geometry) const {
        if(!geometry) throw std::runtime_error("GEOS geometry operation failed");
        return Ptr(geometry,[ctx=context](GEOSGeometry* g){GEOSGeom_destroy_r(ctx,g);});
    }
    GEOSGeometry* ring(const Path& path) const {
        if(path.empty()) return nullptr;
        const bool close=path.front().x!=path.back().x || path.front().y!=path.back().y;
        auto* seq=GEOSCoordSeq_create_r(context,static_cast<unsigned>(path.size()+close),2);
        for(std::size_t i=0;i<path.size()+close;++i) {
            const auto p=path[i%path.size()];
            GEOSCoordSeq_setX_r(context,seq,static_cast<unsigned>(i),p.x);
            GEOSCoordSeq_setY_r(context,seq,static_cast<unsigned>(i),p.y);
        }
        return GEOSGeom_createLinearRing_r(context,seq);
    }
    Ptr polygon(const Polygon& polygon) const {
        if(polygon.exterior.empty()) return hold(GEOSGeom_createEmptyPolygon_r(context));
        auto* shell=ring(polygon.exterior);
        std::vector<GEOSGeometry*> holes;
        for(const auto& hole:polygon.interiors) holes.push_back(ring(hole));
        return hold(GEOSGeom_createPolygon_r(context,shell,holes.data(),static_cast<unsigned>(holes.size())));
    }
    Path coordinates(const GEOSGeometry* ring) const {
        Path result;
        const auto* seq=GEOSGeom_getCoordSeq_r(context,ring);
        unsigned count=0; if(!seq || !GEOSCoordSeq_getSize_r(context,seq,&count)) return result;
        result.resize(count);
        for(unsigned i=0;i<count;++i) {
            GEOSCoordSeq_getX_r(context,seq,i,&result[i].x); GEOSCoordSeq_getY_r(context,seq,i,&result[i].y);
        }
        return result;
    }
    Polygons extract(const GEOSGeometry* geometry) const {
        Polygons result;
        if(GEOSisEmpty_r(context,geometry)) return result;
        const int type=GEOSGeomTypeId_r(context,geometry);
        if(type==GEOS_POLYGON) {
            Polygon polygon;
            polygon.exterior=coordinates(GEOSGetExteriorRing_r(context,geometry));
            const int count=GEOSGetNumInteriorRings_r(context,geometry);
            for(int i=0;i<count;++i) polygon.interiors.push_back(coordinates(GEOSGetInteriorRingN_r(context,geometry,i)));
            result.push_back(std::move(polygon));
        } else if(type==GEOS_MULTIPOLYGON || type==GEOS_GEOMETRYCOLLECTION) {
            const int count=GEOSGetNumGeometries_r(context,geometry);
            for(int i=0;i<count;++i) {
                auto children=extract(GEOSGetGeometryN_r(context,geometry,i));
                result.insert(result.end(),children.begin(),children.end());
            }
        }
        return result;
    }
    Geometry2D extractGeometry(const GEOSGeometry* geometry) const {
        Geometry2D result;const int type=GEOSGeomTypeId_r(context,geometry);result.type=static_cast<Geometry2DType>(type);
        if(GEOSisEmpty_r(context,geometry))return result;
        if(type==GEOS_POLYGON)result.polygon=extract(geometry).front();
        else if(type==GEOS_POINT || type==GEOS_LINESTRING || type==GEOS_LINEARRING)result.coords=coordinates(geometry);
        else for(int i=0;i<GEOSGetNumGeometries_r(context,geometry);++i)result.parts.push_back(extractGeometry(GEOSGetGeometryN_r(context,geometry,i)));
        return result;
    }
    Ptr repair(const GEOSGeometry* polygon) const {
        if(GEOSisValid_r(context,polygon)==1) return hold(GEOSGeom_clone_r(context,polygon));
        double sourceLength=0.0; GEOSLength_r(context,polygon,&sourceLength);
        auto largest=[&](Ptr geometry) {
            if(GEOSGeomTypeId_r(context,geometry.get())!=GEOS_MULTIPOLYGON) return geometry;
            const GEOSGeometry* best=nullptr; double maxArea=-1.0;
            for(int i=0;i<GEOSGetNumGeometries_r(context,geometry.get());++i) {
                const auto* child=GEOSGetGeometryN_r(context,geometry.get(),i); double area=0;
                GEOSArea_r(context,child,&area); if(area>maxArea) { maxArea=area; best=child; }
            }
            return best?hold(GEOSGeom_clone_r(context,best)):std::move(geometry);
        };
        auto acceptable=[&](const GEOSGeometry* g) {
            double length=0.0; GEOSLength_r(context,g,&length);
            return GEOSisValid_r(context,g)==1 && std::abs(length-sourceLength)<=1e-8+0.5*std::abs(sourceLength);
        };
        auto basic=largest(hold(GEOSBuffer_r(context,polygon,1e-12,16)));
        if(acceptable(basic.get())) return basic;
        const auto* exterior=GEOSGetExteriorRing_r(context,polygon);
        const auto points=coordinates(exterior);
        double minX=points[0].x,maxX=minX,minY=points[0].y,maxY=minY;
        for(auto p:points) {minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);minY=std::min(minY,p.y);maxY=std::max(maxY,p.y);}
        const double distance=0.002*((maxX-minX)+(maxY-minY))*0.5;
        if(GEOSGetNumInteriorRings_r(context,polygon)==0) {
            auto bufferedRing=hold(GEOSBuffer_r(context,exterior,distance,16));
            if(GEOSGetNumInteriorRings_r(context,bufferedRing.get())==1) {
                const auto* inner=GEOSGetInteriorRingN_r(context,bufferedRing.get(),0);
                auto reconstructed=this->polygon(Polygon{coordinates(inner),{}});
                auto expanded=hold(GEOSBuffer_r(context,reconstructed.get(),distance,16));
                if(acceptable(expanded.get())) return expanded;
            }
            Path unique;
            for(auto point:points) if(unique.empty() || norm(point-unique.back())>1e-8) unique.push_back(point);
            if(unique.size()>=3) { auto dedupe=this->polygon(Polygon{unique,{}}); if(acceptable(dedupe.get())) return dedupe; }
        }
        auto outer=hold(GEOSBuffer_r(context,polygon,distance,16));
        auto buffered=largest(hold(GEOSBuffer_r(context,outer.get(),-distance,16)));
        if(acceptable(buffered.get())) return buffered;
        throw std::runtime_error("Unable to recover invalid polygon");
    }
};

double signedArea(const Path& path) {
    double area=0.0;
    for(std::size_t i=1;i<path.size();++i) area+=path[i-1].x*path[i].y-path[i].x*path[i-1].y;
    return area*0.5;
}
void refreshSectionPaths(PlanarSection& section) {
    section.paths.clear();
    std::vector<std::vector<std::uint32_t>> adjacency(section.vertices.size());
    std::vector<std::uint32_t> nodeOrder;
    std::map<Edge,std::size_t> entityByEdge;
    std::vector<IndexPath> entityPaths;
    auto appendNeighbor=[&](std::uint32_t a,std::uint32_t b) {
        if(adjacency[a].empty())nodeOrder.push_back(a);
        if(std::find(adjacency[a].begin(),adjacency[a].end(),b)==adjacency[a].end())adjacency[a].push_back(b);
    };
    for(std::size_t i=0;i<section.entities.size();++i) {
        const auto& entity=section.entities[i];
        if(entity.size()>2 && entity.front()==entity.back()) {entityPaths.push_back({static_cast<std::uint32_t>(i)});continue;}
        for(std::size_t j=1;j<entity.size();++j) {
            appendNeighbor(entity[j-1],entity[j]);appendNeighbor(entity[j],entity[j-1]);
            entityByEdge[edgeKey(entity[j-1],entity[j])]=i;
        }
    }
    section.isClosed=true;
    for(const auto& neighbors:adjacency)if(!neighbors.empty() && neighbors.size()!=2)section.isClosed=false;
    // networkx.cycle_basis insertion order and Paton traversal.
    std::vector<bool> remaining(section.vertices.size(),false);
    for(auto id:nodeOrder)remaining[id]=true;
    for(auto rootIt=nodeOrder.rbegin();rootIt!=nodeOrder.rend();++rootIt)if(remaining[*rootIt]) {
        const auto root=*rootIt;
        std::vector<std::uint32_t> stack{root};
        std::vector<std::int64_t> predecessor(section.vertices.size(),-1);
        std::vector<std::set<std::uint32_t>> used(section.vertices.size());
        predecessor[root]=root;
        while(!stack.empty()) {
            const auto node=stack.back();stack.pop_back();
            for(const auto next:adjacency[node]) {
                if(predecessor[next]<0) {predecessor[next]=node;stack.push_back(next);used[next].insert(node);}
                else if(next!=node && !used[node].count(next)) {
                    IndexPath cycle{next,node};auto parent=static_cast<std::uint32_t>(predecessor[node]);
                    while(!used[next].count(parent)) {cycle.push_back(parent);parent=static_cast<std::uint32_t>(predecessor[parent]);}
                    cycle.push_back(parent);used[next].insert(node);
                    if(cycle.size()<2)continue;
                    Path coordinates;for(auto id:cycle)coordinates.push_back(section.vertices[id]);coordinates.push_back(coordinates.front());
                    const bool ccw=signedArea(coordinates)>0;
                    IndexPath entityPath;
                    for(std::size_t j=0;j<=cycle.size();++j) {
                        const auto id=static_cast<std::uint32_t>(entityByEdge.at(edgeKey(cycle[j%cycle.size()],cycle[(j+1)%cycle.size()])));
                        if(std::find(entityPath.begin(),entityPath.end(),id)==entityPath.end())entityPath.push_back(id);
                    }
                    if(!ccw)std::reverse(entityPath.begin(),entityPath.end());
                    if(entityPath.size()==1) {if(!ccw)std::reverse(section.entities[entityPath[0]].begin(),section.entities[entityPath[0]].end());}
                    else for(std::size_t j=0;j<entityPath.size();++j) {
                        auto& a=section.entities[entityPath[j]];auto& b=section.entities[entityPath[(j+1)%entityPath.size()]];
                        int da=0,db=0;
                        if(a.front()==b.front()){da=-1;db=1;}else if(a.front()==b.back()){da=-1;db=-1;}
                        else if(a.back()==b.front()){da=1;db=1;}else if(a.back()==b.back()){da=1;db=-1;}
                        if(da<0)std::reverse(a.begin(),a.end());if(db<0)std::reverse(b.begin(),b.end());
                    }
                    entityPaths.push_back(std::move(entityPath));
                }
            }
        }
        for(std::size_t i=0;i<predecessor.size();++i)if(predecessor[i]>=0)remaining[i]=false;
    }
    for(const auto& entityPath:entityPaths) {
        Path path;
        for(std::size_t j=0;j<entityPath.size();++j) {
            const auto& entity=section.entities[entityPath[j]];
            const auto count=entity.size()-(j+1<entityPath.size()?1:0);
            for(std::size_t i=0;i<count;++i)path.push_back(section.vertices[entity[i]]);
        }
        if(signedArea(path)<0)std::reverse(path.begin(),path.end());
        section.paths.push_back(std::move(path));
    }
}
std::vector<IndexPath> buildEntities(const std::vector<Edge>& edges,std::size_t vertexCount) {
    std::vector<std::set<std::uint32_t>> adjacency(vertexCount);
    std::vector<unsigned> degree(vertexCount);
    std::set<Edge> source;
    for(const auto& edge:edges) {
        adjacency[edge[0]].insert(edge[1]); adjacency[edge[1]].insert(edge[0]);
        ++degree[edge[0]]; ++degree[edge[1]]; source.insert(edgeKey(edge[0],edge[1]));
    }
    std::vector<std::uint32_t> starts;
    for(std::uint32_t i=0;i<vertexCount;++i) if(degree[i]==1) starts.push_back(i);
    for(std::uint32_t i=0;i<vertexCount;++i) if(degree[i]>1) starts.push_back(i);
    std::vector<bool> visited(vertexCount);
    std::vector<IndexPath> entities;
    std::set<Edge> included;
    for(const auto root:starts) if(!visited[root]) {
        IndexPath traversal;
        // scipy csgraph DFS: ascending CSR neighbor indices, recursive order.
        std::function<void(std::uint32_t)> visit=[&](std::uint32_t id) {
            visited[id]=true; traversal.push_back(id);
            for(const auto next:adjacency[id]) if(!visited[next]) visit(next);
        };
        visit(root);
        IndexPath current;
        auto append=[&] {
            if(current.size()<2) { current.clear(); return; }
            if(current.size()>2 && current.front()!=current.back() && source.count(edgeKey(current.front(),current.back()))) current.push_back(current.front());
            for(std::size_t i=1;i<current.size();++i) included.insert(edgeKey(current[i-1],current[i]));
            entities.push_back(current); current.clear();
        };
        for(std::size_t i=0;i<traversal.size();++i) {
            if(!current.empty() && !source.count(edgeKey(current.back(),traversal[i]))) append();
            current.push_back(traversal[i]);
        }
        append();
    }
    for(const auto& edge:source) if(!included.count(edge)) entities.push_back({edge[0],edge[1]});
    return entities;
}
void createSectionGraph(PlanarSection& section) {
    // unique_rows(...digits=5): float_to_int subtracts 1e-6 before round;
    // hashable_rows packs small values, otherwise compares raw int64 bytes.
    std::vector<Vec3> flat;
    for(const auto& line:section.rawLines) {flat.push_back(line[0]);flat.push_back(line[1]);}
    std::vector<std::array<std::int64_t,3>> quantized(flat.size());
    bool packed=true;
    for(std::size_t i=0;i<flat.size();++i) for(int j=0;j<3;++j) {
        const double scaled=std::nearbyint(flat[i][j]*1e5-1e-6);
        if(!std::isfinite(scaled)||std::abs(scaled)>9e18) throw std::invalid_argument("Slice coordinates exceed reference int64 quantization");
        quantized[i][j]=static_cast<std::int64_t>(scaled);
        if(quantized[i][j]>=1048575 || quantized[i][j]<=-1048575) packed=false;
    }
    auto hash=[&](std::size_t i) {
        std::uint64_t value=0;
        for(int j=0;j<3;++j) value^=static_cast<std::uint64_t>(quantized[i][j]+1048576)<<(j*21);
        return value;
    };
    std::vector<std::size_t> order(flat.size()); std::iota(order.begin(),order.end(),0);
    std::stable_sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b) {
        if(packed) return hash(a)<hash(b);
        return std::memcmp(quantized[a].data(),quantized[b].data(),sizeof(quantized[a]))<0;
    });
    std::vector<std::uint32_t> inverse(flat.size());
    std::size_t previous=flat.size();
    for(const auto index:order) {
        if(previous==flat.size() || quantized[previous]!=quantized[index]) {
            section.vertices.push_back({flat[index].x,flat[index].y});section.vertices3D.push_back(flat[index]);previous=index;
        }
        inverse[index]=static_cast<std::uint32_t>(section.vertices.size()-1);
    }
    for(std::size_t i=0;i<inverse.size();i+=2) section.edges.push_back({inverse[i],inverse[i+1]});
    section.entities=buildEntities(section.edges,section.vertices.size());
    refreshSectionPaths(section);
}
void fillSectionGaps(PlanarSection& section) {
    std::vector<std::set<std::uint32_t>> adjacency(section.vertices.size());
    std::vector<std::uint32_t> nodeOrder;
    for(const auto& entity:section.entities)if(entity.front()!=entity.back())for(std::size_t i=1;i<entity.size();++i) {
        for(const auto edge:std::array<Edge,2>{{{entity[i-1],entity[i]},{entity[i],entity[i-1]}}}) {
            if(adjacency[edge[0]].empty())nodeOrder.push_back(edge[0]);adjacency[edge[0]].insert(edge[1]);
        }
    }
    std::vector<std::uint32_t> broken;
    for(auto id:nodeOrder)if(adjacency[id].size()!=2)broken.push_back(id);
    if(broken.empty())return;
    double minX=section.vertices[0].x,maxX=minX,minY=section.vertices[0].y,maxY=minY;
    for(auto p:section.vertices){minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);minY=std::min(minY,p.y);maxY=std::max(maxY,p.y);}
    const double scale=std::hypot(maxX-minX,maxY-minY),distance=scale/100;
    std::set<Edge> endpoints;for(const auto& entity:section.entities)endpoints.insert(edgeKey(entity.front(),entity.back()));
    std::vector<std::array<Vec2,2>> segments;
    std::vector<IndexPath> retained;
    for(const auto& entity:section.entities) {
        if(std::find(broken.begin(),broken.end(),entity.front())!=broken.end() || std::find(broken.begin(),broken.end(),entity.back())!=broken.end()) {
            for(std::size_t i=1;i<entity.size();++i)segments.push_back({section.vertices[entity[i-1]],section.vertices[entity[i]]});
        } else retained.push_back(entity);
    }
    for(std::size_t i=0;i<broken.size();++i)for(std::size_t j=i+1;j<broken.size();++j) {
        const auto edge=edgeKey(broken[i],broken[j]);
        if(norm(section.vertices[edge[0]]-section.vertices[edge[1]])<=distance && !endpoints.count(edge))
            segments.push_back({section.vertices[edge[0]],section.vertices[edge[1]]});
    }
    if(segments.empty())return;
    std::vector<bool> keep(segments.size(),true);
    std::vector<std::array<Vec2,2>> split;
    for(auto id:broken) {
        const auto p=section.vertices[id];
        for(std::size_t i=0;i<segments.size();++i) {
            const auto& line=segments[i];const double length=norm(line[1]-line[0]),a=norm(line[0]-p),b=norm(line[1]-p);
            // trimesh.util.isclose uses a symmetric comparison with rtol=0.
            if(std::abs(length-a-b)<1e-5 && a>1e-5 && b>1e-5) {keep[i]=false;split.push_back({p,line[0]});split.push_back({p,line[1]});}
        }
    }
    std::vector<std::array<Vec2,2>> all;
    for(std::size_t i=0;i<segments.size();++i)if(keep[i])all.push_back(segments[i]);all.insert(all.end(),split.begin(),split.end());
    auto quant=[](Vec2 p,int digits) {const double factor=std::pow(10.0,digits);return std::array<std::int64_t,2>{static_cast<std::int64_t>(std::nearbyint(p.x*factor-1e-6)),static_cast<std::int64_t>(std::nearbyint(p.y*factor-1e-6))};};
    std::set<std::array<std::int64_t,4>> segmentKeys;
    std::vector<Vec2> vertices=section.vertices;
    for(const auto& line:all) {
        auto a=quant(line[0],5),b=quant(line[1],5);if(a==b)continue;if(a>b)std::swap(a,b);
        if(!segmentKeys.insert({a[0],a[1],b[0],b[1]}).second)continue;
        const auto id=static_cast<std::uint32_t>(vertices.size());vertices.push_back(line[0]);vertices.push_back(line[1]);retained.push_back({id,id+1});
    }
    const int digits=std::max(1,std::min(20,std::abs(static_cast<int>(std::log10(1e-5*scale)))));
    std::vector<std::array<std::int64_t,2>> keys;for(auto p:vertices)keys.push_back(quant(p,digits));
    std::vector<std::size_t> order(vertices.size());std::iota(order.begin(),order.end(),0);
    bool packed=true;for(const auto& k:keys)for(auto v:k)if(v>=2147483647LL||v<=-2147483647LL)packed=false;
    auto hash=[&](std::size_t i){return static_cast<std::uint64_t>(keys[i][0]+2147483648LL)^(static_cast<std::uint64_t>(keys[i][1]+2147483648LL)<<32);};
    std::stable_sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b){return packed?hash(a)<hash(b):std::memcmp(keys[a].data(),keys[b].data(),sizeof(keys[a]))<0;});
    std::vector<std::uint32_t> inverse(vertices.size());section.vertices.clear();
    std::size_t previous=vertices.size();
    for(auto id:order){if(previous==vertices.size()||keys[id]!=keys[previous]){section.vertices.push_back(vertices[id]);previous=id;}inverse[id]=static_cast<std::uint32_t>(section.vertices.size()-1);}
    section.entities.clear();std::set<IndexPath> entityKeys;
    for(const auto& entity:retained) {
        IndexPath ids;for(auto i:entity)if(ids.empty()||ids.back()!=inverse[i])ids.push_back(inverse[i]);
        if(ids.size()==3&&ids.front()==ids.back())ids.resize(2);if(ids.size()<2)continue;
        auto key=ids;if(key.front()<=key.back())std::reverse(key.begin(),key.end());
        if(entityKeys.insert(key).second)section.entities.push_back(std::move(ids));
    }
    // remove_unreferenced_vertices, in ascending original vertex index.
    std::set<std::uint32_t> referenced;for(const auto& e:section.entities)referenced.insert(e.begin(),e.end());
    std::vector<std::uint32_t> renumber(section.vertices.size());Path compact;
    for(auto id:referenced){renumber[id]=static_cast<std::uint32_t>(compact.size());compact.push_back(section.vertices[id]);}
    section.vertices=std::move(compact);section.vertices3D.clear();for(auto p:section.vertices)section.vertices3D.push_back({p.x,p.y,section.z});
    section.edges.clear();for(auto& entity:section.entities){for(auto& id:entity)id=renumber[id];for(std::size_t i=1;i<entity.size();++i)section.edges.push_back({entity[i-1],entity[i]});}
    section.gapRepairApplied=true;refreshSectionPaths(section);
}
Polygons sectionPolygons(const PlanarSection& section) {
    Geos geos;
    std::vector<Geos::Ptr> polygons;
    for(const auto& path:section.paths) {
        if(path.size()<4) continue;
        auto p=geos.polygon(Polygon{path,{}});
        try { polygons.push_back(geos.repair(p.get())); } catch(const std::runtime_error&) { polygons.push_back(geos.hold(GEOSGeom_createEmptyPolygon_r(geos.context))); }
    }
    std::vector<std::vector<std::size_t>> children(polygons.size());
    std::vector<unsigned> depth(polygons.size());
    for(std::size_t i=0;i<polygons.size();++i) for(std::size_t j=0;j<polygons.size();++j) if(i!=j) {
        if(GEOSContains_r(geos.context,polygons[i].get(),polygons[j].get())==1) {children[i].push_back(j);++depth[j];}
    }
    std::vector<std::size_t> roots;
    for(std::size_t i=0;i<depth.size();++i) if(depth[i]%2==0) roots.push_back(i);
    if(!depth.empty() && *std::max_element(depth.begin(),depth.end())>1)
        std::stable_sort(roots.begin(),roots.end(),[&](std::size_t a,std::size_t b){return depth[a]<depth[b];});
    Polygons result;
    for(auto root:roots) {
        if(GEOSisEmpty_r(geos.context,polygons[root].get())) continue;
        const auto shells=geos.extract(polygons[root].get());
        if(shells.empty()) continue;
        Polygon polygon{shells.front().exterior,{}};
        for(auto child:children[root]) if(depth[child]==depth[root]+1) {
            auto holes=geos.extract(polygons[child].get());
            if(!holes.empty()) {auto ring=holes.front().exterior;std::reverse(ring.begin(),ring.end());polygon.interiors.push_back(std::move(ring));}
        }
        auto complete=geos.polygon(polygon); auto repaired=geos.repair(complete.get());
        auto extracted=geos.extract(repaired.get()); result.insert(result.end(),extracted.begin(),extracted.end());
    }
    return result;
}

float readFloat(std::istream& stream) { float value;stream.read(reinterpret_cast<char*>(&value),sizeof(value));if(!stream)throw std::runtime_error("Truncated STL");return value; }
std::uint32_t readU32(std::istream& stream) {std::uint32_t value;stream.read(reinterpret_cast<char*>(&value),sizeof(value));if(!stream)throw std::runtime_error("Truncated STL");return value;}

// Adapted from Pillow 12.3.0 src/libImaging/Draw.c (add_edge,
// ImagingDrawPolygon and polygon_generic, 8-bit non-alpha path).
// Copyright (c) 1996-2006 Fredrik Lundh; 1997-2006 Secret Labs AB;
// Pillow contributors. MIT-CMU license and pinned source are retained in
// third_party/pillow_raster/LICENSE and source.lock.json.
void fillPillowPolygon(Bitmap& image,const std::vector<std::array<int,2>>& xy,std::uint8_t ink) {
    struct RasterEdge {int x0,y0,xmin,ymin,xmax,ymax;float dx;};
    std::vector<RasterEdge> edges;
    auto addEdge=[&](int x0,int y0,int x1,int y1){RasterEdge edge{x0,y0,std::min(x0,x1),std::min(y0,y1),std::max(x0,x1),std::max(y0,y1),y0==y1?0.0F:static_cast<float>(x1-x0)/static_cast<float>(y1-y0)};edges.push_back(edge);};
    auto hline=[&](int x0,int y,int x1){if(y<0||y>=static_cast<int>(image.height))return;x0=std::max(0,x0);x1=std::min(static_cast<int>(image.width)-1,x1);if(x0<=x1)std::fill(image.pixels.begin()+static_cast<std::size_t>(y)*image.width+x0,image.pixels.begin()+static_cast<std::size_t>(y)*image.width+x1+1,ink);};
    if(xy.empty())return;
    for(std::size_t i=0;i+1<xy.size();++i){const auto a=xy[i],b=xy[i+1];
        if(a[1]==b[1]&&i!=0&&a[1]==xy[i-1][1]){auto& previous=edges.back();if(b[0]>a[0]&&a[0]>xy[i-1][0]){previous.xmax=b[0];continue;}if(b[0]<a[0]&&a[0]<xy[i-1][0]){previous.xmin=b[0];continue;}}
        addEdge(a[0],a[1],b[0],b[1]);
    }
    if(xy.back()!=xy.front())addEdge(xy.back()[0],xy.back()[1],xy.front()[0],xy.front()[1]);
    int ymin=static_cast<int>(image.height)-1,ymax=0;std::vector<const RasterEdge*> table;
    for(const auto& edge:edges){ymin=std::min(ymin,edge.ymin);ymax=std::max(ymax,edge.ymax);if(edge.ymin==edge.ymax)hline(edge.xmin,edge.ymin,edge.xmax);else table.push_back(&edge);}
    ymin=std::max(ymin,0);ymax=std::min(ymax,static_cast<int>(image.height));
    auto roundUp=[](float f){return static_cast<int>(f>=0?std::floor(static_cast<double>(f+0.5F)):-std::floor(std::fabs(static_cast<double>(f))+0.5));};
    auto roundDown=[](float f){return static_cast<int>(f>=0?std::ceil(static_cast<double>(f-0.5F)):-std::ceil(std::fabs(static_cast<double>(f))-0.5));};
    for(int y=ymin;y<=ymax;++y){std::vector<float> intersections;intersections.reserve(table.size()*2);
        for(std::size_t i=0;i<table.size();++i){const auto& current=*table[i];if(y<current.ymin||y>current.ymax)continue;
            intersections.push_back(static_cast<float>(y-current.y0)*current.dx+static_cast<float>(current.x0));
            if(y==current.ymax&&y<ymax)intersections.push_back(intersections.back());
            else if((y==current.ymin||y==current.ymax)&&current.dx!=0.0F){for(std::size_t k=0;k<i;++k){const auto& other=*table[k];if((y!=other.ymin&&y!=other.ymax)||other.dx==0.0F)continue;
                    const float otherX=static_cast<float>(y-other.y0)*other.dx+static_cast<float>(other.x0);
                    if(std::round(intersections.back())==std::round(otherX)){
                        const int offset=y==current.ymax?-1:1;
                        const float adjacent=static_cast<float>(y+offset-current.y0)*current.dx+static_cast<float>(current.x0);
                        if(y+offset>=other.ymin&&y+offset<=other.ymax){const float adjacentOther=static_cast<float>(y+offset-other.y0)*other.dx+static_cast<float>(other.x0);
                            if(intersections.back()>adjacent+1&&intersections.back()>adjacentOther+1)intersections.back()=std::round(std::max(adjacent,adjacentOther))+1;
                            else if(intersections.back()<adjacent-1&&intersections.back()<adjacentOther-1)intersections.back()=std::round(std::min(adjacent,adjacentOther))-1;
                            break;
                        }
                    }
                }
            }
        }
        std::sort(intersections.begin(),intersections.end());
        for(std::size_t i=1;i<intersections.size();i+=2)hline(roundUp(intersections[i-1]),y,roundDown(intersections[i]));
    }
}

} // namespace

Mesh loadSTL(const std::string& filename) {
    std::ifstream file(std::filesystem::u8path(filename),std::ios::binary);
    if(!file) throw std::runtime_error("Cannot open STL: "+filename);
    file.seekg(0,std::ios::end);const auto length=file.tellg();file.seekg(0);
    Mesh result;
    bool binary=false;std::uint32_t count=0;
    if(length>=84) {file.seekg(80);count=readU32(file);binary=(static_cast<std::uint64_t>(length)==84ULL+50ULL*count);}
    file.clear();file.seekg(0);
    if(binary) {
        file.seekg(84);result.vertices.reserve(static_cast<std::size_t>(count)*3);result.faces.reserve(count);
        for(std::uint32_t i=0;i<count;++i) {
            readFloat(file);readFloat(file);readFloat(file);
            Triangle face{};
            for(int j=0;j<3;++j) {face[j]=static_cast<std::uint32_t>(result.vertices.size());const double x=readFloat(file),y=readFloat(file),z=readFloat(file);result.vertices.push_back({x,y,z});}
            file.ignore(2);if(!file) throw std::runtime_error("Truncated STL attributes");result.faces.push_back(face);
        }
    } else {
        std::string word;Path unused;
        while(file>>word) if(word=="vertex") {
            Vec3 point;if(!(file>>point.x>>point.y>>point.z))throw std::runtime_error("Invalid ASCII STL vertex");
            result.vertices.push_back(point);
            if(result.vertices.size()%3==0) {const auto end=static_cast<std::uint32_t>(result.vertices.size());result.faces.push_back({end-3,end-2,end-1});}
        }
        if(result.vertices.size()%3 || result.faces.empty()) throw std::runtime_error("Invalid or empty ASCII STL");
    }
    return result;
}
void mergeMeshVertices(Mesh& mesh,int digits) {checkMesh(mesh);mergeVerticesImpl(mesh,digits);}

DocumentObject::DocumentObject(std::string value):name(std::move(value)) {}
Part::Part(std::string value):DocumentObject(std::move(value)) {}
std::string Part::toString() const {return "Part <"+name+">";}
bool Part::isDirty() const noexcept { return dirty_ || !same(origin,cachedOrigin_) || !same(rotation,cachedRotation_) || !same(scaleFactor,cachedScale_); }
void Part::setGeometry(const std::string& filename,bool fix,bool merge) {setGeometry(loadSTL(filename),fix,merge);}
void Part::setGeometry(const Mesh& mesh,bool fix,bool merge) {
    checkMesh(mesh);source_=mesh;
    if(merge) mergeMeshVertices(source_);
    if(fix) fixMesh(source_);
    hasGeometry_=true;dirty_=true;
}
void Part::setGeometryByMesh(const Mesh& mesh) {checkMesh(mesh);source_=mesh;hasGeometry_=true;dirty_=true;}
Matrix4 Part::getTransform() const {
    if(scaleFactor.x<std::numeric_limits<double>::epsilon() || scaleFactor.y<std::numeric_limits<double>::epsilon() || scaleFactor.z<std::numeric_limits<double>::epsilon())
        throw std::invalid_argument("Scale factor must be positive");
    const double a=rotation.x*pi/180,b=rotation.y*pi/180,c=rotation.z*pi/180;
    const double sa=std::sin(a),ca=std::cos(a),sb=std::sin(b),cb=std::cos(b),sc=std::sin(c),cc=std::cos(c);
    // Intrinsic rxyz = Rx * Ry * Rz, then diagonal scale, then translation.
    return {cb*cc*scaleFactor.x,-cb*sc*scaleFactor.y,sb*scaleFactor.z,origin.x,
            (ca*sc+sa*sb*cc)*scaleFactor.x,(ca*cc-sa*sb*sc)*scaleFactor.y,-sa*cb*scaleFactor.z,origin.y,
            (sa*sc-ca*sb*cc)*scaleFactor.x,(sa*cc+ca*sb*sc)*scaleFactor.y,ca*cb*scaleFactor.z,origin.z,
            0,0,0,1};
}
void Part::regenerate() const {
    if(!hasGeometry_) throw std::runtime_error("Part geometry was not set");
    const auto matrix=getTransform();transformed_=source_;
    double identityDifference=0.0;for(int i=0;i<16;++i){const double difference=std::abs(matrix[i]-(i%5==0?1.0:0.0));identityDifference=std::isfinite(difference)?std::max(identityDifference,difference):std::numeric_limits<double>::infinity();}
    for(auto& point:transformed_.vertices) if(identityDifference>=1e-8) {
        const auto p=point;
        // Frozen NumPy's OpenBLAS dgemm accumulates the four terms with FMA.
        // A separate multiply/add changes one ulp at many rotated vertices,
        // enough to add collinear vertices to GEOS buffer output.
        auto transform=[&](int row){double value=0.0;value=std::fma(matrix[row*4],p.x,value);value=std::fma(matrix[row*4+1],p.y,value);value=std::fma(matrix[row*4+2],p.z,value);return std::fma(matrix[row*4+3],1.0,value);};
        point={transform(0),transform(1),transform(2)};
    }
    cachedOrigin_=origin;cachedRotation_=rotation;cachedScale_=scaleFactor;dirty_=false;
}
const Mesh& Part::geometry() const {if(isDirty())regenerate();return transformed_;}
Bounds3 Part::boundingBox() const {return meshBounds(geometry());}
Vec3 Part::extents() const {const auto b=boundingBox();return {b[3]-b[0],b[4]-b[1],b[5]-b[2]};}
bool Part::checkGeometry() const {return watertight(geometry());}
double Part::surfaceArea() const {
    double area=0.0;const auto& mesh=geometry();
    for(const auto& f:mesh.faces)area+=norm(cross(mesh.vertices[f[1]]-mesh.vertices[f[0]],mesh.vertices[f[2]]-mesh.vertices[f[0]]))*0.5;
    return area;
}
double Part::volume() const {
    const auto& mesh=geometry();const double volume=signedVolume(mesh);
    if(!watertight(mesh,true) || !(volume>0.0))throw std::runtime_error("Part does not contain a valid volume");
    return volume;
}
void Part::dropToPlatform(double zPos) {origin.z=-(boundingBox()[2]-origin.z)+zPos;dirty_=true;}
Path Part::getProjectedHull() const {
    const auto& vertices=geometry().vertices;
    if(vertices.size()<3)throw std::invalid_argument("Projected convex hull requires three points");
    if(vertices.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))throw std::length_error("Qhull point count exceeds int range");
    std::vector<coordT> points;points.reserve(vertices.size()*2);
    for(auto p:vertices){if(!finite(p))throw std::invalid_argument("Convex hull points must be finite");points.push_back(p.x);points.push_back(p.y);}
    struct QhullContext {
        qhT state{};
        FILE* errorFile=nullptr;
        QhullContext(){errorFile=std::tmpfile();qh_zero(&state,errorFile?errorFile:stderr);}
        ~QhullContext(){qh_freeqhull(&state,qh_ALL);int current=0,total=0;qh_memfreeshort(&state,&current,&total);if(errorFile)std::fclose(errorFile);}
        std::string error() const {
            if(!errorFile)return "Projected convex hull failed";
            std::fflush(errorFile);std::rewind(errorFile);std::string result;std::array<char,512> buffer{};
            while(const auto size=std::fread(buffer.data(),1,buffer.size(),errorFile))result.append(buffer.data(),size);
            return result;
        }
    } context;
    auto* qh=&context.state;
    char options[]="qhull i Qt";
    const int code=qh_new_qhull(qh,2,static_cast<int>(vertices.size()),points.data(),False,options,nullptr,context.errorFile);
    if(code!=0)throw std::runtime_error(context.error());
    qh_triangulate(qh);
    // SciPy 1.18.1 _Qhull.get_extremes_2d: traverse facets starting at
    // facet_list. Do not rotate to a lexicographically chosen hull vertex.
    ++qh->visit_id;++qh->vertex_visit;
    auto* facet=qh->facet_list;auto* first=facet;Path hull;
    while(facet) {
        if(facet->visitid==qh->visit_id)throw std::runtime_error("Qhull facet traversal loop");
        vertexT* a=nullptr;vertexT* b=nullptr;facetT* next=nullptr;
        if(facet->toporient) {a=static_cast<vertexT*>(facet->vertices->e[0].p);b=static_cast<vertexT*>(facet->vertices->e[1].p);next=static_cast<facetT*>(facet->neighbors->e[0].p);}
        else {b=static_cast<vertexT*>(facet->vertices->e[0].p);a=static_cast<vertexT*>(facet->vertices->e[1].p);next=static_cast<facetT*>(facet->neighbors->e[1].p);}
        for(auto* vertex:{a,b})if(vertex->visitid!=qh->vertex_visit){vertex->visitid=qh->vertex_visit;const auto index=qh_pointid(qh,vertex->point);hull.push_back({vertices.at(index).x,vertices.at(index).y});}
        facet->visitid=qh->visit_id;facet=next;if(facet==first)break;
    }
    return hull;
}
Polygon Part::getProjectedHullPolygon() const {Polygon result;result.exterior=getProjectedHull();if(!result.exterior.empty())result.exterior.push_back(result.exterior.front());return result;}
Geometry2D Part::getProjectedGeometry() const {
    Geos geos;std::vector<GEOSGeometry*> polygons;const auto& mesh=geometry();
    for(const auto& face:mesh.faces) {
        Polygon triangle;for(auto id:face)triangle.exterior.push_back({mesh.vertices[id].x,mesh.vertices[id].y});
        triangle.exterior.push_back(triangle.exterior.front());
        auto p=geos.polygon(triangle);polygons.push_back(p.release());
    }
    auto collection=geos.hold(GEOSGeom_createCollection_r(geos.context,GEOS_GEOMETRYCOLLECTION,polygons.data(),static_cast<unsigned>(polygons.size())));
    auto result=geos.hold(GEOSUnaryUnion_r(geos.context,collection.get()));return geos.extractGeometry(result.get());
}
Polygons Part::getProjectedArea() const {
    const auto geometry=getProjectedGeometry();Polygons result;
    std::function<void(const Geometry2D&)> append=[&](const Geometry2D& shape){if(shape.type==Geometry2DType::Polygon)result.push_back(shape.polygon);else for(const auto& part:shape.parts)append(part);};
    append(geometry);return result;
}

std::optional<PlanarSection> Part::getTrimeshSlice(double z) const {
    const auto bounds=boundingBox();if(z<bounds[2] || z>bounds[5])return std::nullopt;
    auto intersections=meshPlane(geometry(),{0,0,z},{0,0,1});
    if(intersections.first.empty())return std::nullopt;
    PlanarSection section;section.z=z;section.rawLines=std::move(intersections.first);section.faceIndex=std::move(intersections.second);
    createSectionGraph(section);
    if(!section.isClosed)fillSectionGaps(section);
    return section;
}
std::pair<std::vector<std::array<Vec3,2>>,std::vector<std::size_t>> meshPlane(const Mesh& mesh,Vec3 origin,Vec3 normal) {
    checkMesh(mesh);if(norm(normal)<1e-13)throw std::invalid_argument("Plane normal must be nonzero");
    std::pair<std::vector<std::array<Vec3,2>>,std::vector<std::size_t>> result;
    std::array<std::vector<std::pair<std::array<Vec3,2>,std::size_t>>,3> cases;
    auto intersect=[&](Vec3 a,Vec3 b) {
        // Array-valued trimesh.util.unitize multiplies by a reciprocal. A
        // direct component-wise divide is mathematically equal but can change
        // GEOS buffer topology by one ulp at nearly collinear vertices.
        const auto delta=b-a;const double magnitude=norm(delta);const auto direction=delta*(1.0/magnitude);
        return a+direction*(dot(normal/norm(normal),origin-a)/dot(normal/norm(normal),direction));
    };
    for(std::size_t faceId=0;faceId<mesh.faces.size();++faceId) {
        const auto& face=mesh.faces[faceId];std::array<int,3> signs{};int negative=0,positive=0,zero=0;
        for(int i=0;i<3;++i) {const double d=dot(mesh.vertices[face[i]]-origin,normal);signs[i]=d>mergeTolerance?1:d<-mergeTolerance?-1:0;negative+=signs[i]<0;positive+=signs[i]>0;zero+=signs[i]==0;}
        std::array<Vec3,2> line{};int which=-1;
        if((negative==1 && positive==2)||(negative==2 && positive==1)) {
            const int unique=negative==1?-1:1;int i=0;while(signs[i]!=unique)++i;
            const auto base=mesh.vertices[face[i]];
            line={intersect(base,mesh.vertices[face[(i+1)%3]]),intersect(base,mesh.vertices[face[(i+2)%3]])};which=0;
        } else if(zero==1 && positive==1 && negative==1) {
            std::vector<Vec3> off;
            for(int i=0;i<3;++i)if(signs[i]==0)line[0]=mesh.vertices[face[i]];else off.push_back(mesh.vertices[face[i]]);
            line[1]=intersect(off[0],off[1]);which=1;
        } else if(zero==2 && positive==1) {
            int j=0;for(int i=0;i<3;++i)if(signs[i]==0)line[j++]=mesh.vertices[face[i]];which=2;
        }
        if(which>=0)cases[which].push_back({line,faceId});
    }
    for(const auto& group:cases)for(const auto& entry:group){result.first.push_back(entry.first);result.second.push_back(entry.second);}
    return result;
}
Matrix4 planeTransform(Vec3 origin,Vec3 normal) {
    const double magnitude=norm(normal);if(magnitude<1e-13)throw std::invalid_argument("Plane normal must be nonzero");
    const auto u=normal/magnitude;
    // Complete the rank-one SVD basis used by NumPy/LAPACK, then orient it.
    std::array<double,9> a{u.x,0,0,u.y,1,0,u.z,0,1};
    const double sign=u.x<0?-1.0:1.0;
    if(std::abs(u.x)<1.0) {
        a[1]=-sign*u.y;a[2]=-sign*u.z;
        a[4]=1-u.y*u.y/(1+sign*u.x);a[5]=-u.y*u.z/(1+sign*u.x);
        a[7]=a[5];a[8]=1-u.z*u.z/(1+sign*u.x);
    }
    if(sign<0)for(int i=0;i<3;++i)a[i*3+2]*=-1;
    const std::array<double,9> b{0,0,-1,0,1,0,1,0,0};
    Matrix4 matrix{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)for(int k=0;k<3;++k)matrix[i*4+j]+=b[i*3+k]*a[j*3+k];
    for(int i=0;i<3;++i)matrix[i*4+3]=-(matrix[i*4]*origin.x+matrix[i*4+1]*origin.y+matrix[i*4+2]*origin.z);
    return matrix;
}
std::optional<PlanarSection> sectionMesh(const Mesh& mesh,Vec3 origin,Vec3 normal) {
    auto raw=meshPlane(mesh,origin,normal);if(raw.first.empty())return std::nullopt;
    PlanarSection section;section.rawLines=std::move(raw.first);section.faceIndex=std::move(raw.second);
    createSectionGraph(section);
    const auto matrix=planeTransform(origin,normal);
    // Recover original 3D vertex values from the representative raw endpoint.
    for(std::size_t i=0;i<section.vertices.size();++i) {
        const auto p=section.vertices3D[i];
        section.vertices[i]={matrix[0]*p.x+matrix[1]*p.y+matrix[2]*p.z+matrix[3],matrix[4]*p.x+matrix[5]*p.y+matrix[6]*p.z+matrix[7]};
    }
    section.paths.clear();
    for(const auto& entity:section.entities)if(entity.size()>2&&entity.front()==entity.back()){
        Path path;for(auto index:entity)path.push_back(section.vertices[index]);if(signedArea(path)<0)std::reverse(path.begin(),path.end());section.paths.push_back(std::move(path));
    }
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)section.to3D[i*4+j]=matrix[j*4+i];
    section.to3D[3]=origin.x;section.to3D[7]=origin.y;section.to3D[11]=origin.z;
    return section;
}
PlanarSection pathFromLines2D(const std::vector<std::array<Vec2,2>>& lines,int digits) {
    if(digits<0||digits>15)throw std::invalid_argument("2D line quantization digits must be in [0,15]");
    PlanarSection section;Path flat;for(const auto& line:lines){flat.push_back(line[0]);flat.push_back(line[1]);section.rawLines.push_back({Vec3{line[0].x,line[0].y,0},Vec3{line[1].x,line[1].y,0}});}
    if(flat.empty())return section;
    const double factor=std::pow(10.0,digits);
    std::vector<std::array<std::int64_t,2>> keys;keys.reserve(flat.size());bool packed=true;
    for(auto p:flat){std::array<std::int64_t,2> key{static_cast<std::int64_t>(std::nearbyint(p.x*factor-1e-6)),static_cast<std::int64_t>(std::nearbyint(p.y*factor-1e-6))};for(auto v:key)if(v>=2147483647LL||v<=-2147483647LL)packed=false;keys.push_back(key);}
    auto hash=[&](std::size_t i){return static_cast<std::uint64_t>(keys[i][0]+2147483648LL)^(static_cast<std::uint64_t>(keys[i][1]+2147483648LL)<<32);};
    std::vector<std::size_t> order(flat.size());std::iota(order.begin(),order.end(),0);
    std::stable_sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b){return packed?hash(a)<hash(b):std::memcmp(keys[a].data(),keys[b].data(),sizeof(keys[a]))<0;});
    std::size_t previous=flat.size();std::vector<std::uint32_t> inverse(flat.size());
    for(auto id:order){if(previous==flat.size()||keys[id]!=keys[previous]){section.vertices.push_back(flat[id]);section.vertices3D.push_back({flat[id].x,flat[id].y,0});previous=id;}inverse[id]=static_cast<std::uint32_t>(section.vertices.size()-1);}
    for(std::size_t i=0;i<inverse.size();i+=2)section.edges.push_back({inverse[i],inverse[i+1]});
    section.entities=buildEntities(section.edges,section.vertices.size());refreshSectionPaths(section);return section;
}
Polygons Part::getVectorSlicePolygons(double z,const SliceOptions& options) const {
    const auto section=getTrimeshSlice(z);if(!section)return {};
    auto polygons=sectionPolygons(*section);Geos geos;
    Polygons result;
    for(const auto& polygon:polygons) {
        auto current=geos.polygon(polygon);
        if(options.simplificationFactor && *options.simplificationFactor!=0.0) {
            double tolerance=*options.simplificationFactor;
            if(options.simplificationFactorMode=="bound") {
                const auto& vertices=section->vertices;
                double minX=vertices[0].x,maxX=minX,minY=vertices[0].y,maxY=minY;
                for(auto p:vertices){minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);minY=std::min(minY,p.y);maxY=std::max(maxY,p.y);}
                tolerance*=((maxX-minX)+(maxY-minY))*0.5;
            } else if(options.simplificationFactorMode=="line")throw std::logic_error("Line simplification mode is not implemented upstream");
            else if(options.simplificationFactorMode!="absolute")throw std::invalid_argument("Invalid simplification mode");
            current=geos.hold(options.simplificationPreserveTopology?GEOSTopologyPreserveSimplify_r(geos.context,current.get(),tolerance):GEOSSimplify_r(geos.context,current.get(),tolerance));
        }
        if(options.fixPolygons)current=geos.hold(GEOSBuffer_r(geos.context,current.get(),POLYGON_FIX_EPSILON,16));
        auto extracted=geos.extract(current.get());result.insert(result.end(),extracted.begin(),extracted.end());
    }
    return result;
}
Paths Part::path2DToPathList(const Polygons& shapes) {
    Paths paths;for(const auto& polygon:shapes){paths.push_back(polygon.exterior);paths.insert(paths.end(),polygon.interiors.begin(),polygon.interiors.end());}return paths;
}
Paths Part::getVectorSlice(double z,const SliceOptions& options) const {return path2DToPathList(getVectorSlicePolygons(z,options));}
Bitmap Part::getBitmapSlice(double z,double resolution,std::optional<Vec2> requestedOrigin) const {
    if(!(resolution>0) || !std::isfinite(resolution))throw std::invalid_argument("Bitmap pitch must be finite and positive");
    const auto section=getTrimeshSlice(z);if(!section)return {};
    auto polygons=sectionPolygons(*section);
    const auto b=boundingBox();const auto origin=requestedOrigin.value_or(Vec2{b[0],b[1]});
    double minX=origin.x,maxX=origin.x,minY=origin.y,maxY=origin.y;
    for(auto p:section->vertices){minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);minY=std::min(minY,p.y);maxY=std::max(maxY,p.y);}
    Bitmap bitmap;bitmap.width=static_cast<std::size_t>(std::ceil((maxX-minX)/resolution))+2;bitmap.height=static_cast<std::size_t>(std::ceil((maxY-minY)/resolution))+2;
    if(bitmap.width>100000000/bitmap.height)throw std::length_error("Bitmap exceeds supported pixel count");
    bitmap.pixels.assign(bitmap.width*bitmap.height,0);
    auto fill=[&](const Path& path,std::uint8_t value) {
        std::vector<std::array<int,2>> points;
        for(auto p:path){const double x=std::nearbyint((p.x-origin.x)/resolution),y=std::nearbyint((p.y-origin.y)/resolution);if(!std::isfinite(x)||!std::isfinite(y)||x<std::numeric_limits<int>::min()||x>std::numeric_limits<int>::max()||y<std::numeric_limits<int>::min()||y>std::numeric_limits<int>::max())throw std::out_of_range("Rasterized coordinates exceed Pillow int range");points.push_back({static_cast<int>(x),static_cast<int>(y)});}
        fillPillowPolygon(bitmap,points,value);
    };
    for(const auto& polygon:polygons){fill(polygon.exterior,1);for(const auto& hole:polygon.interiors)fill(hole,0);}return bitmap;
}

void Document::addObject(std::shared_ptr<DocumentObject> object) {
    if(!object)throw std::invalid_argument("Document object cannot be null");
    std::set<const DocumentObject*> active;
    std::function<std::size_t(std::shared_ptr<DocumentObject>)> add=[&](std::shared_ptr<DocumentObject> node) {
        if(active.count(node.get()))throw std::invalid_argument("Document dependency cycle");
        auto found=std::find(objects_.begin(),objects_.end(),node);
        std::size_t id;
        if(found==objects_.end()){id=objects_.size();objects_.push_back(node);}else id=static_cast<std::size_t>(found-objects_.begin());
        active.insert(node.get());
        for(const auto& child:node->attributes)if(child) {
            const auto childId=add(child);const std::array<std::size_t,2> edge{childId,id};
            if(std::find(dependencies_.begin(),dependencies_.end(),edge)==dependencies_.end())dependencies_.push_back(edge);
        }
        active.erase(node.get());return id;
    };
    add(std::move(object));recalculateDocument();
}
std::vector<std::shared_ptr<DocumentObject>> Document::getDependencyList() const {
    std::vector<unsigned> indegree(objects_.size());for(const auto& edge:dependencies_)++indegree[edge[1]];
    std::vector<std::size_t> generation;for(std::size_t i=0;i<indegree.size();++i)if(!indegree[i])generation.push_back(i);
    std::vector<std::shared_ptr<DocumentObject>> result;
    while(!generation.empty()) {
        std::vector<std::size_t> next;
        for(auto id:generation){result.push_back(objects_[id]);for(const auto& edge:dependencies_)if(edge[0]==id && --indegree[edge[1]]==0)next.push_back(edge[1]);}
        generation=std::move(next);
    }
    if(result.size()!=objects_.size())throw std::runtime_error("Document dependency graph is cyclic");return result;
}
void Document::recalculateDocument() {
    const auto ordered=getDependencyList();
    for(const auto& object:ordered)if(object->usesBoundaryLayers()) {
        std::set<const DocumentObject*> visited;
        std::function<void(const std::shared_ptr<DocumentObject>&)> mark=[&](const auto& node){for(const auto& child:node->attributes)if(child && visited.insert(child.get()).second){child->setRequiresBoundaryLayers();mark(child);}};
        mark(object);
    }
}
std::shared_ptr<DocumentObject> Document::head() const {auto ordered=getDependencyList();if(ordered.empty())throw std::out_of_range("Document has no objects");return ordered.back();}
std::vector<std::shared_ptr<Part>> Document::parts() const {std::vector<std::shared_ptr<Part>> result;for(const auto& object:objects_)if(auto part=std::dynamic_pointer_cast<Part>(object))result.push_back(part);return result;}
Bounds3 Document::boundingBox() const {return head()->boundingBox();}
Bounds3 Document::partBoundingBox() const {
    const auto list=parts();if(list.empty())throw std::runtime_error("Document contains no parts");auto bounds=list.front()->boundingBox();
    for(const auto& part:list){const auto b=part->boundingBox();for(int i=0;i<3;++i){bounds[i]=std::min(bounds[i],b[i]);bounds[i+3]=std::max(bounds[i+3],b[i+3]);}}return bounds;
}
Vec3 Document::extents() const {const auto b=boundingBox();return {b[3]-b[0],b[4]-b[1],b[5]-b[2]};}
Vec3 Document::partExtents() const {const auto b=partBoundingBox();return {b[3]-b[0],b[4]-b[1],b[5]-b[2]};}
std::string Document::drawNetworkGraph() const {
    std::ostringstream graph;graph<<"digraph PySLM {\n";
    for(std::size_t i=0;i<objects_.size();++i)graph<<"  n"<<i<<" [label="<<std::quoted(objects_[i]->name)<<"];\n";
    for(const auto& edge:dependencies_)graph<<"  n"<<edge[0]<<" -> n"<<edge[1]<<";\n";graph<<"}\n";return graph.str();
}
} // namespace pyslm
