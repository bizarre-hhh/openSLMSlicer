#pragma once
#include "types.hpp"
#include <memory>
namespace pyslm::io {
struct BuildDocument {Header header;std::vector<Model> models;std::vector<Layer> layers;};
struct FormatInfo {std::string name,description;std::vector<std::string> extensions;bool canRead=false,canWrite=false;};
// Vendor translators can live in another native DLL. Both sides must use the
// supported MSVC x64 toolchain and the same CRT/configuration.
class PYSLM_API FormatAdapter {
public:
    virtual ~FormatAdapter()=default;
    virtual FormatInfo format() const=0;
    virtual BuildDocument read(const std::string& filename) const;
    virtual void write(const std::string& filename,const BuildDocument& build) const;
};
class PYSLM_API FormatRegistry {
public:
    void add(std::shared_ptr<FormatAdapter> adapter);
    void remove(const std::string& name);
    std::vector<FormatInfo> formats() const;
    BuildDocument read(const std::string& filename,const std::string& format={}) const;
    void write(const std::string& filename,const BuildDocument& build,const std::string& format={}) const;
private:
    std::vector<std::shared_ptr<FormatAdapter>> adapters_;
    const FormatAdapter& resolve(const std::string& filename,const std::string& name,bool writing) const;
};
// Open native interchange format, separate from proprietary device formats.
PYSLM_API std::shared_ptr<FormatAdapter> jsonAdapter();
}
