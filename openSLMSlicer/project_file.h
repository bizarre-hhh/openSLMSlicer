#ifndef PROJECT_FILE_H
#define PROJECT_FILE_H

#include <QByteArray>
#include <QString>
#include <glm/vec3.hpp>
#include "plate_dimensions.h"
#include "support_types.h"

#include <vector>

// Original STL bytes are embedded in the document, never external links.
struct ProjectModel
{
    QString source_path;
    QByteArray stl_data;
    glm::vec3 translation_mm{0.0f};
    glm::vec3 scale{1.0f};
    glm::vec3 rotation_degrees{0.0f};
    glm::vec2 placement_center_mm{PlateDimensions::kDefaultMm * 0.5f};
    QString name;
};

// Persisted support group. Supports are stored in the same local coordinates as
// SupportGroup::local_triangles (X/Y relative to the owner model, Z absolute),
// and are owned by the model at owner_model_index in ProjectData::models.
struct ProjectSupportRecord
{
    std::uint32_t owner_model_index = 0;
    SupportState state = SupportState::NotNeeded;
    double overhang_angle_deg = kSupportOverhangAngleDeg;
    double volume_mm3 = 0.0;
    std::vector<SliceTriangle> local_triangles;
};

struct ProjectData
{
    std::vector<ProjectModel> models;
    std::vector<ProjectSupportRecord> supports;
    glm::vec3 plate_size_mm = PlateDimensions::DefaultSizeMm();
};

namespace ProjectFile
{
QString Filter();
QString DefaultOpenDirectory();
bool Validate(const ProjectData& project, QString& error);
bool Read(const QString& path, ProjectData& project, QString& error);
bool Write(const QString& path, const ProjectData& project, QString& error);
} // namespace ProjectFile

#endif
