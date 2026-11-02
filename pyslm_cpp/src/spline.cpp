#include "pyslm/spline.hpp"
extern "C" {
#include "dfitpack.h"
#include "minpack.h"
}
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace pyslm {
namespace {
thread_local const Path* fittingPoints=nullptr;
double npSum(const double* data,std::size_t count){
    if(count<8){double s=-0.;for(std::size_t i=0;i<count;++i)s+=data[i];return s;}
    if(count<=128){double r[8];std::copy(data,data+8,r);std::size_t i=8;
        for(;i<count-(count%8);i+=8)for(int k=0;k<8;++k)r[k]+=data[i+k];
        double s=((r[0]+r[1])+(r[2]+r[3]))+((r[4]+r[5])+(r[6]+r[7]));for(;i<count;++i)s+=data[i];return s;}
    std::size_t n=count/2;n-=n%8;return npSum(data,n)+npSum(data+n,count-n);
}
int circleResidual(int* m,int*,double* center,double* residual,int*){
    for(int i=0;i<*m;++i){auto p=(*fittingPoints)[i];double x=p.x-center[0],y=p.y-center[1];residual[i]=x*x+y*y;}
    double avg=npSum(residual,*m)/double(*m);for(int i=0;i<*m;++i)residual[i]-=avg;return 0;
}
}
SplineCurve fitSpline(const Path& points,double smooth){
    if(points.size()<4||points.size()>10000000)throw std::invalid_argument("Cubic spline fitting requires at least four points");
    if(!(smooth>=0)||!std::isfinite(smooth))throw std::invalid_argument("Spline smoothing must be finite and nonnegative");
    int m=int(points.size()),degree=3,nest=std::max(smooth==0?m+degree+1:m+2*degree,2*degree+3),n=0,status=0;
    int lwrk=m*(degree+1)+nest*(6+2+3*degree);
    std::vector<double> x;for(auto p:points){if(!std::isfinite(p.x)||!std::isfinite(p.y))throw std::invalid_argument("Spline data must be finite");x.push_back(p.x);x.push_back(p.y);}
    std::vector<double> u(m),w(m,1),knots(nest),coefficients(2*nest),work(lwrk);std::vector<int> iwork(nest);
    double ub=0,ue=1,fp=0;
    parcur(0,0,2,m,u.data(),2*m,x.data(),w.data(),&ub,&ue,degree,smooth,nest,&n,knots.data(),int(coefficients.size()),coefficients.data(),&fp,work.data(),lwrk,iwork.data(),&status);
    if(status>3)throw std::invalid_argument("FITPACK rejected spline input (status "+std::to_string(status)+")");
    SplineCurve result;result.knots.assign(knots.begin(),knots.begin()+n);result.fitStatus=status;
    for(int i=0;i<n-degree-1;++i)result.control.push_back({coefficients[i],coefficients[n+i]});
    const auto delta=points.front()-points.back();result.closed=std::sqrt(delta.x*delta.x+delta.y*delta.y)<1e-5;
    if(result.closed){auto shared=(result.control.front()+result.control.back())*.5;result.control.front()=shared;result.control.back()=shared;}
    return result;
}
Path discretizeSpline(const SplineCurve& curve,double scale,std::optional<std::size_t> count){
    if(curve.degree<1||curve.degree>5||curve.control.size()<std::size_t(curve.degree+1))throw std::invalid_argument("Spline degree must be 1..5 with sufficient controls");
    if(curve.control.empty()||curve.knots.size()!=curve.control.size()+curve.degree+1)throw std::invalid_argument("Spline knot/control shape mismatch");
    for(auto p:curve.control)if(!std::isfinite(p.x)||!std::isfinite(p.y))throw std::invalid_argument("Spline controls must be finite");
    for(std::size_t i=0;i<curve.knots.size();++i)if(!std::isfinite(curve.knots[i])||(i&&curve.knots[i]<curve.knots[i-1]))throw std::invalid_argument("Spline knots must be finite and nondecreasing");
    if(!(curve.knots[curve.degree]<curve.knots[curve.control.size()]))throw std::invalid_argument("Spline parameter interval must be positive");
    if(!(scale>0)||!std::isfinite(scale))throw std::invalid_argument("Spline drawing scale must be positive");
    std::size_t size;
    if(count)size=*count;
    else{double length=0;for(std::size_t i=1;i<curve.control.size();++i){auto d=curve.control[i]-curve.control[i-1];length+=std::sqrt(d.x*d.x+d.y*d.y);}
        size=std::size_t(std::clamp(length/(.05*scale),20.*curve.control.size(),500.*curve.control.size()));}
    if(size<1||size>100000000)throw std::invalid_argument("Spline sample count is invalid");
    std::vector<double> t(size),cx,cy,ox(size),oy(size);double start=curve.knots.front(),end=curve.knots.back();
    double step=size>1?(end-start)/double(size-1):0;
    for(std::size_t i=0;i<size;++i)t[i]=start+i*step;if(size>1)t.back()=end;
    for(auto p:curve.control){cx.push_back(p.x);cy.push_back(p.y);}int status=0;
    splev(curve.knots.data(),int(curve.knots.size()),cx.data(),int(cx.size()),curve.degree,t.data(),ox.data(),int(size),0,&status);
    if(status)throw std::runtime_error("FITPACK x evaluation failed");
    splev(curve.knots.data(),int(curve.knots.size()),cy.data(),int(cy.size()),curve.degree,t.data(),oy.data(),int(size),0,&status);
    if(status)throw std::runtime_error("FITPACK y evaluation failed");
    Path result;result.reserve(size);for(std::size_t i=0;i<size;++i)result.push_back({ox[i],oy[i]});return result;
}
std::optional<Path> fitClosedCircle(const Path& p,double scale){
    constexpr double pi=3.14159265358979323846;
    if(p.size()<3||!(scale>0)||norm(p.front()-p.back())>1e-5)return std::nullopt;
    double minx=p.front().x,maxx=minx,miny=p.front().y,maxy=miny;
    for(auto q:p){minx=std::min(minx,q.x);maxx=std::max(maxx,q.x);miny=std::min(miny,q.y);maxy=std::max(maxy,q.y);}
    double aspect=(maxx-minx)/(maxy-miny);if(!std::isfinite(aspect)||std::abs(aspect-1)>.1)return std::nullopt;
    int m=int(p.size()),info=0,nfev=0,ipvt[2]{};double center[2]{},diag[2]{},qtf[2]{},wa1[2]{},wa2[2]{},wa3[2]{};
    for(auto q:p){center[0]+=q.x;center[1]+=q.y;}center[0]/=m;center[1]/=m;
    std::vector<double> residual(m),fjac(2*m),wa4(m);auto previous=fittingPoints;fittingPoints=&p;
    LMDIF(circleResidual,m,2,center,residual.data(),1.49012e-8,1e-8,0,600,std::numeric_limits<double>::epsilon(),diag,1,100,0,&info,&nfev,fjac.data(),m,ipvt,qtf,wa1,wa2,wa3,wa4.data());
    fittingPoints=previous;if(info<1||info>4)throw std::runtime_error("MINPACK circle fit failed");
    Vec2 c{center[0],center[1]};std::vector<double> radii;for(auto q:p){auto d=q-c;radii.push_back(std::sqrt(d.x*d.x+d.y*d.y));}
    double radius=npSum(radii.data(),radii.size())/radii.size();auto extrema=std::minmax_element(radii.begin(),radii.end());
    if(!(radius/scale>1e-4&&radius/scale<50)||(*extrema.second-*extrema.first)/radius>.02)return std::nullopt;
    for(std::size_t i=1;i<p.size();++i){auto d=p[i]-p[i-1];double length=std::sqrt(d.x*d.x+d.y*d.y);
        if(length/radius>50*pi/180||length/scale>.125)return std::nullopt;}
    for(auto i:{std::size_t(0),p.size()-2}){auto d=p[i+1]-p[i],mid=p[i]+d*.5-c;
        double dl=std::sqrt(d.x*d.x+d.y*d.y),rl=std::sqrt(mid.x*mid.x+mid.y*mid.y);
        double tangent=std::abs(std::acos(dot(mid/rl,d/dl))-pi/2);if(tangent>20*pi/180)return std::nullopt;}
    Path result;for(auto angle:{0.,pi/2,pi})result.push_back({std::cos(angle)*radius+c.x,std::sin(angle)*radius+c.y});return result;
}
Path discretizeClosedCircle(const Path& points,double scale){
    if(points.size()!=3||!(scale>0)||!std::isfinite(scale))throw std::invalid_argument("Circle requires three control points and finite positive scale");
    for(auto p:points)if(!std::isfinite(p.x)||!std::isfinite(p.y))throw std::invalid_argument("Circle controls must be finite");
    constexpr double pi=3.14159265358979323846;
    Vec2 vectors[3]={points[2]-points[1],points[0]-points[2],points[1]-points[0]};
    double abc2[3],abc[3];for(int i=0;i<3;++i){abc2[i]=dot(vectors[i],vectors[i]);abc[i]=std::sqrt(abc2[i]);}
    double smallest=*std::min_element(abc,abc+3);if(smallest==0)return points;
    if(!std::isfinite(smallest)||!std::isfinite(abc[0]+abc[1]+abc[2]))throw std::invalid_argument("Circle edge lengths exceed numeric range");
    double edges[3]={abc[0]/smallest,abc[1]/smallest,abc[2]/smallest};
    double half=(edges[0]+edges[1]+edges[2])/2,denom=half*((half-edges[0])*(half-edges[1])*(half-edges[2]));
    if(denom<1e-5)return points;
    double radius=smallest*((edges[0]*edges[1]*edges[2]/4)/std::sqrt(denom));
    double weights[3]={(abc2[1]+abc2[2]-abc2[0])*abc2[0],(abc2[0]+abc2[2]-abc2[1])*abc2[1],(abc2[0]+abc2[1]-abc2[2])*abc2[2]};
    Vec2 c{};for(int i=0;i<3;++i){c.x=std::fma(points[i].x,weights[i],c.x);c.y=std::fma(points[i].y,weights[i],c.y);}c=c/(weights[0]+weights[1]+weights[2]);
    auto v1=points[0]-c;v1=v1/std::sqrt(dot(v1,v1));double normal=(-vectors[1].x)*vectors[2].y-(-vectors[1].y)*vectors[2].x;
    Vec2 v2=normal<0?Vec2{-v1.y,v1.x}:Vec2{v1.y,-v1.x};
    int count=int(std::ceil(std::max({2*pi/.08,radius*2*pi/(.05*scale),4.})));
    Path result;for(int i=0;i<count;++i){double t=double(i)*(2*pi/double(count-1));if(i+1==count)t=2*pi;
        auto p=c+v1*(radius*std::cos(t));p=p+v2*(radius*std::sin(t));result.push_back(p);}return result;
}
}
