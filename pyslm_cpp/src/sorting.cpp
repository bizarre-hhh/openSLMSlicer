#include "pyslm/hatching.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace pyslm::hatching {
namespace {
constexpr double pi=3.141592653589793238462643383279502884;
bool less(double a,double b){return a<b||(!std::isnan(a)&&std::isnan(b));}
Path reordered(const Path& vectors,const std::vector<std::size_t>& order) {
    Path out;out.reserve(vectors.size());
    for(auto i:order) {out.push_back(vectors[2*i]);out.push_back(vectors[2*i+1]);}
    return out;
}
void valid(const Path& vectors) {
    if(!isValidHatchArray(vectors))throw std::invalid_argument("Hatch vector coordinate count must be even");
}
std::vector<Vec2> midPoints(const Path& vectors) {
    valid(vectors);std::vector<Vec2> out;out.reserve(vectors.size()/2);
    for(std::size_t i=0;i<vectors.size();i+=2)out.push_back((vectors[i]+vectors[i+1])*0.5);
    return out;
}
} // namespace

std::vector<std::size_t> numpyArgsortAvx2(const std::vector<double>& values);
namespace {
bool hasAvx2() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    int features[4]{};__cpuid(features,1);
    if((features[2]&(1<<27))==0||(features[2]&(1<<28))==0)return false;
    if((_xgetbv(0)&6)!=6)return false;
    __cpuidex(features,7,0);return (features[1]&(1<<5))!=0;
#elif defined(__x86_64__) || defined(__i386__)
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}
}
// The pinned baseline dispatches NumPy's identical x86-simd-sort AVX2 path.
// Keep the generic fallback for machines lacking AVX2; tie order there differs
// and is explicitly outside the current baseline certification.
std::vector<std::size_t> numpyArgsort(const std::vector<double>& v) {
    static const bool avx2=hasAvx2();
    if(avx2)return numpyArgsortAvx2(v);
    std::vector<std::size_t> out(v.size());std::iota(out.begin(),out.end(),0);
    if(out.size()<2)return out;
    struct Range {std::ptrdiff_t l,r;int depth;};
    int depth=0;for(auto n=out.size();n>1;n>>=1)++depth;
    std::vector<Range> stack{{0,static_cast<std::ptrdiff_t>(out.size()-1),depth*2}};
    while(!stack.empty()) {
        auto current=stack.back();stack.pop_back();
        auto l=current.l,r=current.r;depth=current.depth;
        while(r-l>15) {
            if(depth<0) {
                auto begin=out.begin()+l,end=out.begin()+r+1;
                auto compare=[&](auto a,auto b){return less(v[a],v[b]);};
                std::make_heap(begin,end,compare);std::sort_heap(begin,end,compare);
                l=r;break;
            }
            auto m=l+(r-l)/2;
            if(less(v[out[m]],v[out[l]]))std::swap(out[m],out[l]);
            if(less(v[out[r]],v[out[m]]))std::swap(out[r],out[m]);
            if(less(v[out[m]],v[out[l]]))std::swap(out[m],out[l]);
            double pivot=v[out[m]];
            auto i=l,j=r-1;std::swap(out[m],out[j]);
            for(;;) {
                do{++i;}while(less(v[out[i]],pivot));
                do{--j;}while(less(pivot,v[out[j]]));
                if(j<i)break;
                std::swap(out[i],out[j]);
            }
            std::swap(out[i],out[r-1]);--depth;
            if(i-l<r-i) {if(i+1<r)stack.push_back({i+1,r,depth});r=i-1;}
            else {if(l<i-1)stack.push_back({l,i-1,depth});l=i+1;}
        }
        for(auto i=l+1;i<=r;++i) {
            auto item=out[i];auto j=i;
            while(j>l&&less(v[item],v[out[j-1]])){out[j]=out[j-1];--j;}
            out[j]=item;
        }
    }
    return out;
}

Path UnidirectionalSort::sort(const Path& v) const{return v;}
Path FlipSort::sort(const Path& v) const {
    valid(v);Path out=v;for(std::size_t i=0;i<out.size();i+=2)std::swap(out[i],out[i+1]);return out;
}
Path AlternateSort::sort(const Path& v) const {
    valid(v);Path out=v;for(std::size_t i=2;i<out.size();i+=4)std::swap(out[i],out[i+1]);return out;
}
Path HatchDirectionalSort::sort(const Path& v) const {
    valid(v);Path out=v;
    for(std::size_t i=0;i<out.size();i+=2) {
        auto delta=out[i+1]-out[i];
        if(std::atan2(delta.x,delta.y)<0)std::swap(out[i],out[i+1]);
    }
    for(std::size_t i=2;i<out.size();i+=4)std::swap(out[i],out[i+1]);
    return out;
}
ChainSort::ChainSort(std::vector<std::shared_ptr<BaseSort>> chain):sorters(std::move(chain)){}
Path ChainSort::sort(const Path& v) const {
    Path out=v;
    for(auto& sorter:sorters) {
        if(!sorter)throw std::invalid_argument("ChainSort contains a null sorter");
        out=sorter->sort(out);
    }
    return out;
}
Path LinearSort::sort(const Path& v) const {
    auto points=midPoints(v);double radians=hatchAngle*pi/180,c=std::cos(radians),s=std::sin(radians);
    std::vector<double> keys;keys.reserve(points.size());
    for(auto p:points)keys.push_back(c*p.x+s*p.y);
    return reordered(v,numpyArgsort(keys));
}
Path GreedySort::sort(const Path& v) const {
    auto points=midPoints(v);if(points.empty())return {};
    if(!(clusterDistance>0.0)||!std::isfinite(clusterDistance)||!std::isfinite(hatchTol))
        throw std::invalid_argument("Greedy cluster distance must be positive and parameters finite");
    const auto n=points.size();std::vector<std::vector<std::size_t>> paths;
    std::vector<bool> visited(n,false);double radians=hatchAngle*pi/180,c=std::cos(radians),s=std::sin(radians);
    for(std::size_t root=0;root<n;++root) {
        if(visited[root])continue;
        // Graph nodes and component subgraph preserve input node order.
        std::vector<std::size_t> queue{root};visited[root]=true;
        for(std::size_t q=0;q<queue.size();++q)for(std::size_t j=0;j<n;++j) {
            if(!visited[j]&&norm(points[queue[q]]-points[j])<hatchTol){visited[j]=true;queue.push_back(j);}
        }
        std::sort(queue.begin(),queue.end());
        std::vector<double> keys;for(auto i:queue)keys.push_back(c*points[i].x+s*points[i].y);
        auto indices=numpyArgsort(keys);std::vector<std::size_t> path;
        for(auto i:indices)path.push_back(queue[i]);
        paths.push_back(std::move(path));
    }
    std::vector<double> first;for(auto& p:paths)first.push_back(points[p.front()][sortY?1:0]);
    auto clusterOrder=numpyArgsort(first);std::vector<std::vector<std::size_t>> sorted;
    for(auto i:clusterOrder)sorted.push_back(std::move(paths[i]));paths=std::move(sorted);
    std::vector<std::size_t> next(paths.size(),0),order;order.reserve(n);
    double maxMove=0;bool advance=true;
    while(order.size()<n) {
        if(advance)maxMove+=clusterDistance;
        advance=true;
        bool selected=false;
        for(std::size_t i=0;i<paths.size();++i) {
            auto& p=paths[i];if(next[i]==p.size())continue;
            if(points[p[next[i]]][sortY?1:0]>maxMove)continue;
            double innerDistance=0;
            while(innerDistance<clusterDistance) {
                if(next[i]<p.size())order.push_back(p[next[i]++]);
                if(next[i]==p.size())break;
                innerDistance+=points[p[next[i]]][sortY?1:0]-points[p[next[i]-1]][sortY?1:0];
            }
            advance=false;selected=true;break;
        }
        // Skip empty absolute windows without altering which cluster is chosen.
        if(!selected) {
            double minimum=std::numeric_limits<double>::infinity();
            for(std::size_t i=0;i<paths.size();++i)if(next[i]<paths[i].size())
                minimum=std::min(minimum,points[paths[i][next[i]]][sortY?1:0]);
            if(std::isfinite(minimum)&&minimum>maxMove+clusterDistance)
                maxMove+=std::floor((minimum-maxMove)/clusterDistance)*clusterDistance;
        }
    }
    return reordered(v,order);
}
} // namespace pyslm::hatching
