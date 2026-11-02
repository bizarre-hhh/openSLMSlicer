#include "pyslm/io.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
namespace pyslm::io {
namespace {
using json=nlohmann::json;
std::string lower(std::string s){std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return char(std::tolower(c));});return s;}
class JsonAdapter:public FormatAdapter {
public:
    FormatInfo format()const override{return {"pyslm-json","PySLM native build interchange v1",{".json"},true,true};}
    void write(const std::string& filename,const BuildDocument& build)const override{
        json output{{"schemaVersion",1},{"format","pyslm-json"},{"header",{{"filename",build.header.filename},{"version",build.header.version},{"zUnit",build.header.zUnit}}},
            {"models",json::array()},{"layers",json::array()}};
        for(const auto& m:build.models){json styles=json::array();for(const auto& s:m.buildStyles){
            json t;
#define F(n) t[#n]=s.n
            F(bid);F(name);F(description);F(laserPower);F(laserSpeed);F(laserFocus);F(laserId);F(pointDistance);F(pointExposureTime);F(pointDelay);F(jumpDelay);F(jumpSpeed);
#undef F
            t["laserMode"]=int(s.laserMode);if(s.jumpSpeedSourceIsInteger)t["jumpSpeedSourceIsInteger"]=*s.jumpSpeedSourceIsInteger;styles.push_back(t);}
            output["models"].push_back({{"mid",m.mid},{"topLayerId",m.topLayerId},{"name",m.name},{"buildStyleName",m.buildStyleName},{"buildStyleDescription",m.buildStyleDescription},{"buildStyles",styles}});}
        for(const auto& l:build.layers){json groups=json::array();for(const auto& g:l.geometry){json coords=json::array();for(auto p:g.coords)coords.push_back({p.x,p.y});
            groups.push_back({{"type",int(g.kind)},{"mid",g.mid},{"bid",g.bid},{"subType",g.subType},{"coords",coords}});}
            output["layers"].push_back({{"z",l.z},{"id",l.id},{"name",l.name},{"layerFilePosition",l.layerFilePosition},{"geometry",groups}});}
        std::ofstream stream(std::filesystem::u8path(filename));if(!stream)throw std::runtime_error("Cannot create build file: "+filename);
        stream<<output.dump(2)<<'\n';if(!stream)throw std::runtime_error("Cannot write build file: "+filename);
    }
    BuildDocument read(const std::string& filename)const override{
        std::ifstream stream(std::filesystem::u8path(filename));if(!stream)throw std::runtime_error("Cannot open build file: "+filename);json input;stream>>input;
        if(input.at("schemaVersion")!=1||input.at("format")!="pyslm-json")throw std::invalid_argument("Unsupported native build schema");
        BuildDocument b;const auto& h=input.at("header");h.at("filename").get_to(b.header.filename);h.at("version").get_to(b.header.version);h.at("zUnit").get_to(b.header.zUnit);
        for(const auto& item:input.at("models")){Model m;
#define F(n) item.at(#n).get_to(m.n)
            F(mid);F(topLayerId);F(name);F(buildStyleName);F(buildStyleDescription);
#undef F
            for(const auto& t:item.at("buildStyles")){BuildStyle s;
#define F(n) t.at(#n).get_to(s.n)
                F(bid);F(name);F(description);F(laserPower);F(laserSpeed);F(laserFocus);F(laserId);F(pointDistance);F(pointExposureTime);F(pointDelay);F(jumpDelay);F(jumpSpeed);
#undef F
                s.laserMode=LaserMode(t.at("laserMode").get<int>());if(t.contains("jumpSpeedSourceIsInteger"))s.jumpSpeedSourceIsInteger=t.at("jumpSpeedSourceIsInteger").get<bool>();m.buildStyles.push_back(s);}b.models.push_back(std::move(m));}
        for(const auto& item:input.at("layers")){Layer l;
#define F(n) item.at(#n).get_to(l.n)
            F(z);F(id);F(name);F(layerFilePosition);
#undef F
            for(const auto& t:item.at("geometry")){LayerGeometry g;g.kind=LayerGeometryType(t.at("type").get<int>());
                t.at("mid").get_to(g.mid);t.at("bid").get_to(g.bid);t.at("subType").get_to(g.subType);
                for(const auto& p:t.at("coords")){if(!p.is_array()||p.size()!=2)throw std::invalid_argument("Layer coordinate shape must be Nx2");g.coords.push_back({p[0],p[1]});}l.geometry.push_back(std::move(g));}b.layers.push_back(std::move(l));}
        return b;
    }
};
}
BuildDocument FormatAdapter::read(const std::string&)const{throw std::logic_error("This adapter does not implement reading");}
void FormatAdapter::write(const std::string&,const BuildDocument&)const{throw std::logic_error("This adapter does not implement writing");}
void FormatRegistry::add(std::shared_ptr<FormatAdapter> a){if(!a||a->format().name.empty())throw std::invalid_argument("Adapter must have a format name");
    const auto name=lower(a->format().name);for(const auto& old:adapters_)if(lower(old->format().name)==name)throw std::invalid_argument("Duplicate format: "+name);adapters_.push_back(std::move(a));}
void FormatRegistry::remove(const std::string& name){auto key=lower(name);adapters_.erase(std::remove_if(adapters_.begin(),adapters_.end(),[&](const auto& a){return lower(a->format().name)==key;}),adapters_.end());}
std::vector<FormatInfo> FormatRegistry::formats()const{std::vector<FormatInfo> result;for(const auto& a:adapters_)result.push_back(a->format());return result;}
const FormatAdapter& FormatRegistry::resolve(const std::string& file,const std::string& name,bool writing)const{
    const auto key=lower(name),ext=lower(std::filesystem::u8path(file).extension().u8string());const FormatAdapter* found=nullptr;
    for(const auto& a:adapters_){auto f=a->format();bool matches=key.empty()?std::any_of(f.extensions.begin(),f.extensions.end(),[&](auto e){return lower(e)==ext;}):lower(f.name)==key;
        if(!matches)continue;if(!(writing?f.canWrite:f.canRead))continue;
        if(found)throw std::invalid_argument("Ambiguous file extension; select a format explicitly");found=a.get();}
    if(!found)throw std::invalid_argument("No registered native translator for this file format");return *found;
}
BuildDocument FormatRegistry::read(const std::string& filename,const std::string& format)const{return resolve(filename,format,false).read(filename);}
void FormatRegistry::write(const std::string& filename,const BuildDocument& build,const std::string& format)const{resolve(filename,format,true).write(filename,build);}
std::shared_ptr<FormatAdapter> jsonAdapter(){return std::make_shared<JsonAdapter>();}
}
