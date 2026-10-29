#ifndef EXPORT_SLICE_DIALOG_H
#define EXPORT_SLICE_DIALOG_H

#include <QDialog>

#include <cstddef>

#include "slice_export.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

// Collects the M1 slice export format, target path and options. The dialog only
// edits options; SceneEditorWindow runs ExportSliceBuild() after accept() and
// performs the overwrite confirmation right before writing.
class ExportSliceDialog : public QDialog
{
public:
    ExportSliceDialog(const SliceBuildSnapshot& snapshot,
                      const QString& initial_path,
                      SliceExportFormat initial_format,
                      QWidget* parent = nullptr);

    SliceExportFormat Format() const;
    // Absolute path with the suffix of the selected format. Empty when the
    // path field is empty.
    QString OutputPath() const;
    SliceExportOptions Options() const;
    bool OpenFolderAfterExport() const;

    void accept() override;

private:
    void SetFormat(SliceExportFormat format);
    void Browse();
    void UpdateFormatOptions();
    void UpdateSummary();

    QComboBox* format_combo_ = nullptr;
    QLineEdit* path_edit_ = nullptr;
    QPushButton* browse_button_ = nullptr;
    QCheckBox* include_empty_layers_check_ = nullptr;
    QCheckBox* include_hatches_check_ = nullptr;
    QCheckBox* validate_check_ = nullptr;
    QCheckBox* open_folder_check_ = nullptr;
    QLabel* summary_label_ = nullptr;
    QLabel* hint_label_ = nullptr;
    std::size_t part_count_ = 0;
    std::size_t support_part_count_ = 0;
    std::size_t layer_count_ = 0;
    std::size_t empty_layer_count_ = 0;
    std::size_t contour_count_ = 0;
    std::size_t hatch_count_ = 0;
    double layer_thickness_mm_ = 0.0;
    // The empty-layer choice is kept while CLI (which skips empty layers) is
    // selected and restored when the user switches back to .slmslice.
    bool include_empty_layers_preference_ = true;
};

#endif
