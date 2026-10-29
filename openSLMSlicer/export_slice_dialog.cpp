#include "export_slice_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>

#include <cmath>

namespace
{
// Append the suffix of the selected format, or replace the suffix of any other
// supported format. Other suffixes typed by the user are kept.
QString EnsureFormatSuffix(const QString& path, SliceExportFormat format)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty())
        return QString();
    const QString suffix = SliceExportSuffix(format);
    if (trimmed.endsWith(QStringLiteral(".") + suffix, Qt::CaseInsensitive))
        return trimmed;
    for (const IJobWriter* writer : SliceExportRegistry::Instance().Writers())
    {
        if (writer->format() == format)
            continue;
        const QString other = writer->suffix();
        if (!other.isEmpty() &&
            trimmed.endsWith(QStringLiteral(".") + other, Qt::CaseInsensitive))
        {
            return trimmed.left(trimmed.size() - other.size() - 1) +
                   QStringLiteral(".") + suffix;
        }
    }
    return trimmed + QStringLiteral(".") + suffix;
}

QString NumberText(std::size_t value)
{
    return QLocale().toString(static_cast<qulonglong>(value));
}

QString ThicknessText(double thickness_mm)
{
    const QLocale locale;
    if (!(thickness_mm > 0.0))
        return QStringLiteral("未知");
    const double micrometers = thickness_mm * 1000.0;
    if (std::abs(micrometers - std::round(micrometers)) < 0.05)
    {
        return QStringLiteral("%1 µm").arg(
            locale.toString(static_cast<qlonglong>(std::llround(micrometers))));
    }
    return QStringLiteral("%1 µm").arg(locale.toString(micrometers, 'f', 1));
}
} // namespace

ExportSliceDialog::ExportSliceDialog(const SliceBuildSnapshot& snapshot,
                                     const QString& initial_path,
                                     SliceExportFormat initial_format,
                                     QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("exportSliceDialog"));
    setWindowTitle(QStringLiteral("导出切片文件"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setFont(QFont(QStringLiteral("Microsoft YaHei"), 11));
    setMinimumWidth(560);

    part_count_ = snapshot.parts.size();
    layer_thickness_mm_ = snapshot.settings.layer_thickness_mm;
    for (const SliceLayer& layer : snapshot.layers)
    {
        ++layer_count_;
        if (layer.empty)
            ++empty_layer_count_;
        contour_count_ += layer.contours.size();
        hatch_count_ += layer.hatches.size();
    }

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(14);

    QGroupBox* output_group = new QGroupBox(QStringLiteral("输出"), this);
    output_group->setObjectName(QStringLiteral("exportOutputGroup"));
    QFormLayout* output_form = new QFormLayout(output_group);
    format_combo_ = new QComboBox(output_group);
    format_combo_->setObjectName(QStringLiteral("exportFormatCombo"));
    for (const IJobWriter* writer :
         SliceExportRegistry::Instance().Writers())
    {
        format_combo_->addItem(writer->name(),
                               static_cast<int>(writer->format()));
    }
    format_combo_->setToolTip(QStringLiteral(
        "M1 支持 .slmslice（无损内部容器）、CLI 2.0 ASCII（通用交换格式）"
        "以及 SVG/PNG/CSV（预览、报告与统计）"));
    output_form->addRow(QStringLiteral("格式"), format_combo_);

    path_edit_ = new QLineEdit(output_group);
    path_edit_->setObjectName(QStringLiteral("exportPathEdit"));
    path_edit_->setText(QDir::toNativeSeparators(initial_path));
    path_edit_->setClearButtonEnabled(true);
    path_edit_->setToolTip(QStringLiteral("导出文件的完整路径，可手动修改"));
    browse_button_ = new QPushButton(QStringLiteral("浏览…"), output_group);
    browse_button_->setObjectName(QStringLiteral("exportBrowseButton"));
    QHBoxLayout* path_row = new QHBoxLayout;
    path_row->setContentsMargins(0, 0, 0, 0);
    path_row->addWidget(path_edit_, 1);
    path_row->addWidget(browse_button_);
    output_form->addRow(QStringLiteral("文件"), path_row);
    layout->addWidget(output_group);

    QGroupBox* options_group = new QGroupBox(QStringLiteral("选项"), this);
    options_group->setObjectName(QStringLiteral("exportOptionsGroup"));
    QVBoxLayout* options_layout = new QVBoxLayout(options_group);
    include_empty_layers_check_ = new QCheckBox(
        QStringLiteral("包含空层（.slmslice / CSV）"), options_group);
    include_empty_layers_check_->setObjectName(
        QStringLiteral("exportIncludeEmptyLayersCheck"));
    include_empty_layers_check_->setChecked(true);
    options_layout->addWidget(include_empty_layers_check_);

    include_hatches_check_ =
        new QCheckBox(QStringLiteral("包含填充"), options_group);
    include_hatches_check_->setObjectName(
        QStringLiteral("exportIncludeHatchesCheck"));
    include_hatches_check_->setChecked(true);
    include_hatches_check_->setToolTip(
        hatch_count_ == 0 ? QStringLiteral("当前预览没有填充线")
                          : QStringLiteral("把预览中的填充线写入文件"));
    options_layout->addWidget(include_hatches_check_);

    validate_check_ =
        new QCheckBox(QStringLiteral("导出后校验"), options_group);
    validate_check_->setObjectName(QStringLiteral("exportValidateCheck"));
    validate_check_->setChecked(true);
    validate_check_->setToolTip(
        QStringLiteral("写入后回读文件，校验层数、轮廓数、填充数和路径长度"));
    options_layout->addWidget(validate_check_);

    open_folder_check_ =
        new QCheckBox(QStringLiteral("完成后打开文件夹"), options_group);
    open_folder_check_->setObjectName(QStringLiteral("exportOpenFolderCheck"));
    open_folder_check_->setChecked(true);
    open_folder_check_->setToolTip(
        QStringLiteral("导出成功后用资源管理器打开输出目录"));
    options_layout->addWidget(open_folder_check_);
    layout->addWidget(options_group);

    QGroupBox* summary_group = new QGroupBox(QStringLiteral("预览摘要"), this);
    summary_group->setObjectName(QStringLiteral("exportSummaryGroup"));
    QVBoxLayout* summary_layout = new QVBoxLayout(summary_group);
    summary_label_ = new QLabel(summary_group);
    summary_label_->setObjectName(QStringLiteral("exportSummaryLabel"));
    summary_label_->setWordWrap(true);
    summary_label_->setTextFormat(Qt::PlainText);
    summary_layout->addWidget(summary_label_);
    layout->addWidget(summary_group);

    hint_label_ = new QLabel(this);
    hint_label_->setObjectName(QStringLiteral("exportFormatHintLabel"));
    hint_label_->setWordWrap(true);
    hint_label_->setTextFormat(Qt::PlainText);
    layout->addWidget(hint_label_);

    QDialogButtonBox* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("exportButtonBox"));
    if (QPushButton* accept_button = buttons->button(QDialogButtonBox::Ok))
    {
        accept_button->setObjectName(QStringLiteral("exportAcceptButton"));
        accept_button->setText(QStringLiteral("确定"));
        accept_button->setDefault(true);
    }
    if (QPushButton* cancel_button = buttons->button(QDialogButtonBox::Cancel))
    {
        cancel_button->setObjectName(QStringLiteral("exportCancelButton"));
        cancel_button->setText(QStringLiteral("取消"));
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &ExportSliceDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(format_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { SetFormat(Format()); });
    connect(browse_button_, &QPushButton::clicked, this,
            &ExportSliceDialog::Browse);
    connect(include_empty_layers_check_, &QCheckBox::toggled, this,
            [this](bool checked)
    {
        const SliceExportFormat format = Format();
        if (format == SliceExportFormat::SlmSlice ||
            format == SliceExportFormat::Csv)
            include_empty_layers_preference_ = checked;
        UpdateSummary();
    });
    connect(include_hatches_check_, &QCheckBox::toggled, this,
            [this](bool) { UpdateSummary(); });

    SetFormat(initial_format);
}

SliceExportFormat ExportSliceDialog::Format() const
{
    const SliceExportFormat format = static_cast<SliceExportFormat>(
        format_combo_->currentData().toInt());
    return SliceExportRegistry::Instance().WriterFor(format)
        ? format : SliceExportFormat::SlmSlice;
}

QString ExportSliceDialog::OutputPath() const
{
    const QString path = EnsureFormatSuffix(path_edit_->text(), Format());
    if (path.isEmpty())
        return QString();
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

SliceExportOptions ExportSliceDialog::Options() const
{
    SliceExportOptions options;
    options.format = Format();
    options.output_path = OutputPath();
    options.include_empty_layers =
        (options.format == SliceExportFormat::SlmSlice ||
         options.format == SliceExportFormat::Csv) &&
        include_empty_layers_check_->isChecked();
    options.include_hatches = include_hatches_check_->isChecked();
    options.validate_after_write = validate_check_->isChecked();
    return options;
}

bool ExportSliceDialog::OpenFolderAfterExport() const
{
    return open_folder_check_->isChecked();
}

void ExportSliceDialog::SetFormat(SliceExportFormat format)
{
    int index = -1;
    for (int i = 0; i < format_combo_->count(); ++i)
    {
        if (format_combo_->itemData(i).toInt() == static_cast<int>(format))
        {
            index = i;
            break;
        }
    }
    if (index < 0)
        index = 0;
    if (format_combo_->currentIndex() != index)
    {
        const QSignalBlocker blocker(format_combo_);
        format_combo_->setCurrentIndex(index);
    }
    UpdateFormatOptions();
    const QString path = path_edit_->text().trimmed();
    if (!path.isEmpty())
    {
        path_edit_->setText(QDir::toNativeSeparators(
            EnsureFormatSuffix(path, format)));
    }
    UpdateSummary();
}

void ExportSliceDialog::Browse()
{
    const SliceExportFormat format = Format();
    const QString current = OutputPath();
    QFileDialog dialog(this, QStringLiteral("选择切片导出文件"));
    dialog.setObjectName(QStringLiteral("exportFileDialog"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilter(SliceExportFileFilter(format));
    dialog.setDefaultSuffix(SliceExportSuffix(format));
    // Overwrite confirmation is done by the editor window right before the
    // export, so the native dialog must not ask a second time.
    dialog.setOption(QFileDialog::DontConfirmOverwrite, true);
    if (!current.isEmpty())
    {
        dialog.setDirectory(QFileInfo(current).absolutePath());
        dialog.selectFile(QFileInfo(current).fileName());
    }
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QStringList files = dialog.selectedFiles();
    if (files.isEmpty())
        return;
    const QString selected = EnsureFormatSuffix(files.front(), format);
    if (selected.isEmpty())
        return;
    path_edit_->setText(QDir::toNativeSeparators(selected));
}

void ExportSliceDialog::UpdateFormatOptions()
{
    const SliceExportFormat format = Format();
    const bool keeps_empty_layers = format == SliceExportFormat::SlmSlice ||
                                    format == SliceExportFormat::Csv;
    {
        const QSignalBlocker blocker(include_empty_layers_check_);
        if (keeps_empty_layers)
        {
            include_empty_layers_check_->setEnabled(true);
            include_empty_layers_check_->setChecked(
                include_empty_layers_preference_);
        }
        else
        {
            include_empty_layers_preference_ =
                include_empty_layers_check_->isChecked();
            include_empty_layers_check_->setChecked(false);
            include_empty_layers_check_->setEnabled(false);
        }
    }
    include_empty_layers_check_->setToolTip(keeps_empty_layers
        ? QStringLiteral("保留首层和末层之间的 Z 网格空层，使层号连续")
        : QStringLiteral("该格式不写入空层"));

    QString hint;
    switch (format)
    {
    case SliceExportFormat::SlmSlice:
        hint = QStringLiteral(
            "写入 .slmslice v1 二进制容器：保留 part、空层和逐层校验和。");
        break;
    case SliceExportFormat::Cli:
        hint = QStringLiteral(
            "CLI 2.0 ASCII：只写层几何并跳过空层，不携带工艺参数。");
        break;
    case SliceExportFormat::Svg:
        hint = QStringLiteral(
            "SVG 联系人表：每个非空层一个单元格，含层号/Z 标签，"
            "矢量可缩放，适合报告与质检对比。");
        break;
    case SliceExportFormat::Png:
        hint = QStringLiteral(
            "PNG 联系人表：与 SVG 相同的版式栅格化，适合贴进文档或工单。");
        break;
    case SliceExportFormat::Csv:
        hint = QStringLiteral(
            "CSV 逐层统计：每层一行（层号、Z、轮廓/填充数量与长度），"
            "可直接 diff、导入表格或作为仿真输入。");
        break;
    }
    hint_label_->setText(hint);
}

void ExportSliceDialog::UpdateSummary()
{
    const SliceExportFormat format = Format();
    const std::size_t valid_layers = layer_count_ - empty_layer_count_;
    const bool keeps_empty_layers = format == SliceExportFormat::SlmSlice ||
                                    format == SliceExportFormat::Csv;
    const bool keep_empty = keeps_empty_layers &&
        include_empty_layers_check_->isChecked();
    const std::size_t written_layers = keep_empty ? layer_count_ : valid_layers;

    QString text = QStringLiteral("模型 part：%1 个 | 层厚：%2")
        .arg(NumberText(part_count_), ThicknessText(layer_thickness_mm_));
    text += QStringLiteral("\n切片层：%1 层（有效 %2 层，空层 %3 层）")
        .arg(NumberText(layer_count_))
        .arg(NumberText(valid_layers))
        .arg(NumberText(empty_layer_count_));
    text += QStringLiteral("\n本次写入：%1 层 | 轮廓 %2 条 | 填充 %3 条")
        .arg(NumberText(written_layers))
        .arg(NumberText(contour_count_))
        .arg(NumberText(hatch_count_));
    if (format == SliceExportFormat::Svg || format == SliceExportFormat::Png)
    {
        text += QStringLiteral("\n联系人表：%1 个单元格（每个非空层一格）")
            .arg(NumberText(valid_layers));
    }
    else if (format == SliceExportFormat::Csv)
    {
        text += QStringLiteral("\nCSV 数据行：%1 行")
            .arg(NumberText(written_layers));
    }
    summary_label_->setText(text);
}

void ExportSliceDialog::accept()
{
    const QString path = OutputPath();
    if (path.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("导出切片文件"),
            QStringLiteral("请先选择导出文件路径。"));
        return;
    }
    const QFileInfo info(path);
    if (!info.absoluteDir().exists())
    {
        QMessageBox::warning(this, QStringLiteral("导出切片文件"),
            QStringLiteral("输出目录不存在：%1")
                .arg(QDir::toNativeSeparators(info.absolutePath())));
        return;
    }
    path_edit_->setText(QDir::toNativeSeparators(info.filePath()));
    QDialog::accept();
}
