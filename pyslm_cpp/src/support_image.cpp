#include "pyslm/support_render.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <stdexcept>

namespace pyslm::support {
namespace {
void validate(const HeightMap& a){if(a.width*a.height!=a.values.size())throw std::invalid_argument("Height-map shape mismatch");}
double cast(double x,bool f){return f?double(float(x)):x;}
}
HeightMap transposeHeightMap(const HeightMap& a){validate(a);HeightMap b;b.width=a.height;b.height=a.width;b.float32=a.float32;b.values.resize(a.values.size());
    for(std::size_t r=0;r<a.height;++r)for(std::size_t c=0;c<a.width;++c)b.at(r,c)=a.at(c,r);return b;}
HeightMap flipHeightMapVertical(const HeightMap& a){validate(a);auto b=a;
    for(std::size_t r=0;r<a.height;++r)for(std::size_t c=0;c<a.width;++c)b.at(c,r)=a.at(c,a.height-1-r);return b;}
HeightMap padHeightMap(const HeightMap& a,std::size_t border,double value){validate(a);HeightMap b;
    b.width=a.width+2*border;b.height=a.height+2*border;b.float32=a.float32;b.values.assign(b.width*b.height,cast(value,a.float32));
    for(std::size_t r=0;r<a.height;++r)for(std::size_t c=0;c<a.width;++c)b.at(c+border,r+border)=a.at(c,r);return b;}
HeightMap heightMapGradientMagnitude(const HeightMap& a,double sigma){validate(a);
    if(a.width<2||a.height<2)throw std::invalid_argument("Gradient requires at least 2 pixels on each axis");
    if(sigma<0||!std::isfinite(sigma))throw std::invalid_argument("Gaussian sigma must be finite and nonnegative");
    auto b=a;
    for(std::size_t r=0;r<a.height;++r)for(std::size_t c=0;c<a.width;++c){
        auto r0=r==0?r:r-1,r1=r+1==a.height?r:r+1,c0=c==0?c:c-1,c1=c+1==a.width?c:c+1;
        double dx=cast(cast(a.at(c,r1)-a.at(c,r0),a.float32)/double(r1-r0),a.float32);
        double dy=cast(cast(a.at(c1,r)-a.at(c0,r),a.float32)/double(c1-c0),a.float32);
        b.at(c,r)=cast(std::sqrt(cast(cast(dx*dx,a.float32)+cast(dy*dy,a.float32),a.float32)),a.float32);
    }
    if(sigma<=1e-15)return b;
    int radius=int(4*sigma+0.5);std::vector<double> kernel(2*radius+1);double total=0;
    for(int i=-radius;i<=radius;++i){kernel[i+radius]=std::exp(-.5/(sigma*sigma)*i*i);total+=kernel[i+radius];}
    for(auto& v:kernel)v/=total;
    auto reflect=[](long long x,long long n){while(x<0||x>=n){if(x<0)x=-x-1;else x=2*n-x-1;}return std::size_t(x);};
    for(int axis=0;axis<2;++axis){auto source=b;
        for(std::size_t r=0;r<a.height;++r)for(std::size_t c=0;c<a.width;++c){
            // scipy NI_Correlate1D symmetric kernel: centre first, then mirrored pairs.
            double v=source.at(c,r)*kernel[radius];
            for(int i=1;i<=radius;++i){double lo=axis==0?source.at(c,reflect(static_cast<long long>(r)-i,a.height)):source.at(reflect(static_cast<long long>(c)-i,a.width),r);
                double hi=axis==0?source.at(c,reflect(static_cast<long long>(r)+i,a.height)):source.at(reflect(static_cast<long long>(c)+i,a.width),r);
                v+=(lo+hi)*kernel[radius+i];}b.at(c,r)=cast(v,a.float32);
        }
    }return b;
}
// Marching squares case orientation and contour stitching follow scikit-image
// 0.26.0 _find_contours_cy.pyx and _assemble_contours (BSD-3-Clause).
Paths findHeightMapContours(const HeightMap& a,double level,const HeightMap* mask){validate(a);
    if(a.width<2||a.height<2)throw std::invalid_argument("Contour image must be at least 2x2");
    if(mask){validate(*mask);if(mask->width!=a.width||mask->height!=a.height)throw std::invalid_argument("Contour mask shape mismatch");}
    using Point=std::pair<double,double>;using Segment=std::pair<Point,Point>;std::vector<Segment> segments;
    auto fraction=[&](double f,double t){return t==f?0:(level-f)/(t-f);};
    for(std::size_t r=0;r+1<a.height;++r)for(std::size_t c=0;c+1<a.width;++c){
        if(mask&&!(mask->at(c,r)&&mask->at(c+1,r)&&mask->at(c,r+1)&&mask->at(c+1,r+1)))continue;
        double ul=a.at(c,r),ur=a.at(c+1,r),ll=a.at(c,r+1),lr=a.at(c+1,r+1);
        if(std::isnan(ul)||std::isnan(ur)||std::isnan(ll)||std::isnan(lr))continue;
        int k=(ul>level)+2*(ur>level)+4*(ll>level)+8*(lr>level);if(k==0||k==15)continue;
        Point top{double(r),c+fraction(ul,ur)},bottom{double(r+1),c+fraction(ll,lr)},left{r+fraction(ul,ll),double(c)},right{r+fraction(ur,lr),double(c+1)};
        auto add=[&](Point f,Point t){segments.emplace_back(f,t);};
        switch(k){case 1:add(top,left);break;case 2:add(right,top);break;case 3:add(right,left);break;
        case 4:add(left,bottom);break;case 5:add(top,bottom);break;case 6:add(right,top);add(left,bottom);break;
        case 7:add(right,bottom);break;case 8:add(bottom,right);break;case 9:add(top,left);add(bottom,right);break;
        case 10:add(bottom,top);break;case 11:add(bottom,left);break;case 12:add(left,right);break;case 13:add(top,right);break;case 14:add(left,top);break;}
    }
    std::map<std::size_t,std::deque<Point>> contours;std::map<Point,std::size_t> starts,ends;std::size_t next=0;
    for(const auto& segment:segments){auto f=segment.first,t=segment.second;if(f==t)continue;
        auto ti=starts.find(t),hi=ends.find(f);bool hasTail=ti!=starts.end(),hasHead=hi!=ends.end();
        std::size_t tail=hasTail?ti->second:0,head=hasHead?hi->second:0;
        if(hasTail)starts.erase(ti);if(hasHead)ends.erase(hi);
        if(hasTail&&hasHead){
            if(tail==head)contours.at(head).push_back(t);
            else if(tail>head){auto& h=contours.at(head);const auto& q=contours.at(tail);h.insert(h.end(),q.begin(),q.end());
                contours.erase(tail);starts[h.front()]=head;ends[h.back()]=head;}
            else{auto& q=contours.at(tail);const auto& h=contours.at(head);starts.erase(h.front());q.insert(q.begin(),h.begin(),h.end());
                contours.erase(head);starts[q.front()]=tail;ends[q.back()]=tail;}
        }else if(!hasTail&&!hasHead){contours[next]={f,t};starts[f]=next;ends[t]=next;++next;}
        else if(!hasHead){contours.at(tail).push_front(f);starts[f]=tail;}
        else{contours.at(head).push_back(t);ends[t]=head;}
    }
    Paths result;for(const auto& entry:contours){Path path;for(auto p:entry.second)path.push_back({p.first,p.second});result.push_back(std::move(path));}return result;
}
} // namespace pyslm::support
