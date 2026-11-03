#include "export_slice_dialog.h"

#include "fluent_ui.h"

#include "components/basicinput/Button.h"
#include "components/basicinput/CheckBox.h"
#include "components/basicinput/ComboBox.h"
#include "components/textfields/Label.h"
#include "components/textfields/LineEdit.h"
#include "design/Spacing.h"
#include "design/Typography.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStringList>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

namespace
{
// Fluent 控件别名：它们分别是 QCheckBox / QComboBox / QLineEdit / QLabel 的
// 子类，测试按 Qt 基类查找控件的代码保持不变。
using fluent::basicinput::CheckBox;
using fluent::basicinput::ComboBox;
using fluent::textfields::LineEdit;

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

// 卡片正文中的说明小字（Fluent Caption 排版角色 + 次要文本色）。
QLabel* Caption(const QString& text, QWidget* parent)
{
    return FluentUi::MakeLabel(parent, text, FluentUi::TextRole::Caption, true);
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
    // 对话框字体、Fluent 排版和控件外观由集成层统一设置。
    FluentUi::PolishDialog(*this);
    setMinimumWidth(640);

    part_count_ = snapshot.parts.size();
    for (const SlicePartInfo& part : snapshot.parts)
        if (part.kind == SlicePartKind::Support)
            ++support_part_count_;
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
    layout->setContentsMargins(Spacing::Large, 20, Spacing::Large, 20);
    layout->setSpacing(Spacing::Gap::Loose);

    // ---------- 标题区 ----------
    QLabel* title = FluentUi::MakeLabel(this, QStringLiteral("导出切片文件"),
                                        FluentUi::TextRole::Title);
    title->setObjectName(QStringLiteral("exportTitleLabel"));
    layout->addWidget(title);
    QLabel* subtitle = FluentUi::MakeLabel(
        this, QStringLiteral("选择输出格式与文件位置，选项和预览摘要会随格式自动调整。"),
        FluentUi::TextRole::Caption, true);
    subtitle->setObjectName(QStringLiteral("exportSubtitleLabel"));
    subtitle->setWordWrap(true);
    subtitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(subtitle);

    // ---------- 输出 ----------
    QVBoxLayout* output_body = nullptr;
    QWidget* output_card = FluentUi::MakeSection(
        this, QStringLiteral("输出"), &output_body);
    output_card->setObjectName(QStringLiteral("exportOutputGroup"));

    QFormLayout* output_form = new QFormLayout;
    output_form->setContentsMargins(0, 0, 0, 0);
    output_form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    output_form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    output_form->setHorizontalSpacing(Spacing::Standard);
    output_form->setVerticalSpacing(Spacing::Medium);
    output_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    output_form->setRowWrapPolicy(QFormLayout::DontWrapRows);

    format_combo_ = new ComboBox(output_card);
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
    format_combo_->setMinimumHeight(Spacing::ControlHeight::Standard);
    output_form->addRow(Caption(QStringLiteral("格式"), output_card), format_combo_);

    path_edit_ = new LineEdit(output_card);
    path_edit_->setObjectName(QStringLiteral("exportPathEdit"));
    path_edit_->setText(QDir::toNativeSeparators(initial_path));
    path_edit_->setClearButtonEnabled(true);
    path_edit_->setToolTip(QStringLiteral("导出文件的完整路径，可手动修改"));
    path_edit_->setMinimumHeight(Spacing::ControlHeight::Standard);
    browse_button_ = FluentUi::MakeButton(
        output_card, QStringLiteral("浏览…"), FluentUi::ButtonKind::Standard);
    browse_button_->setObjectName(QStringLiteral("exportBrowseButton"));
    browse_button_->setMinimumWidth(96);
    browse_button_->setMinimumHeight(Spacing::ControlHeight::Standard);
    QWidget* path_row_widget = new QWidget(output_card);
    QHBoxLayout* path_row = new QHBoxLayout(path_row_widget);
    path_row->setContentsMargins(0, 0, 0, 0);
    path_row->setSpacing(Spacing::Gap::Normal);
    path_row->addWidget(path_edit_, 1);
    path_row->addWidget(browse_button_);
    output_form->addRow(Caption(QStringLiteral("文件"), output_card), path_row_widget);
    output_body->addLayout(output_form);

    // 选中格式的说明文字，跟随格式切换更新（原 updater 逻辑不变）。
    hint_label_ = new fluent::textfields::Label(output_card);
    hint_label_->setObjectName(QStringLiteral("exportFormatHintLabel"));
    hint_label_->setWordWrap(true);
    hint_label_->setTextFormat(Qt::PlainText);
    hint_label_->setFluentTypography(Typography::FontRole::Caption);
    hint_label_->setTextColorRole(
        fluent::textfields::Label::TextColorRole::Secondary);
    hint_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    output_body->addWidget(hint_label_);
    layout->addWidget(output_card);

    // ---------- 选项 ----------
    QVBoxLayout* options_body = nullptr;
    QWidget* options_card = FluentUi::MakeSection(
        this, QStringLiteral("选项"), &options_body);
    options_card->setObjectName(QStringLiteral("exportOptionsGroup"));

    include_empty_layers_check_ = new CheckBox(
        QStringLiteral("包含空层（.slmslice / CSV）"), options_card);
    include_empty_layers_check_->setObjectName(
        QStringLiteral("exportIncludeEmptyLayersCheck"));
    include_empty_layers_check_->setChecked(true);
    options_body->addWidget(include_empty_layers_check_);

    include_hatches_check_ =
        new CheckBox(QStringLiteral("包含填充"), options_card);
    include_hatches_check_->setObjectName(
        QStringLiteral("exportIncludeHatchesCheck"));
    include_hatches_check_->setChecked(true);
    include_hatches_check_->setToolTip(
        hatch_count_ == 0 ? QStringLiteral("当前预览没有填充线")
                          : QStringLiteral("把预览中的填充线写入文件"));
    options_body->addWidget(include_hatches_check_);

    validate_check_ =
        new CheckBox(QStringLiteral("导出后校验"), options_card);
    validate_check_->setObjectName(QStringLiteral("exportValidateCheck"));
    validate_check_->setChecked(true);
    validate_check_->setToolTip(
        QStringLiteral("写入后回读文件，校验层数、轮廓数、填充数和路径长度"));
    options_body->addWidget(validate_check_);

    open_folder_check_ =
        new CheckBox(QStringLiteral("完成后打开文件夹"), options_card);
    open_folder_check_->setObjectName(QStringLiteral("exportOpenFolderCheck"));
    open_folder_check_->setChecked(true);
    open_folder_check_->setToolTip(
        QStringLiteral("导出成功后用资源管理器打开输出目录"));
    options_body->addWidget(open_folder_check_);
    layout->addWidget(options_card);

    // ---------- 预览摘要 ----------
    QVBoxLayout* summary_body = nullptr;
    QWidget* summary_card = FluentUi::MakeSection(
        this, QStringLiteral("预览摘要"), &summary_body);
    summary_card->setObjectName(QStringLiteral("exportSummaryGroup"));
    summary_label_ = new fluent::textfields::Label(summary_card);
    summary_label_->setObjectName(QStringLiteral("exportSummaryLabel"));
    summary_label_->setWordWrap(true);
    summary_label_->setTextFormat(Qt::PlainText);
    summary_label_->setFluentTypography(Typography::FontRole::Body);
    summary_label_->setTextColorRole(
        fluent::textfields::Label::TextColorRole::Primary);
    summary_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    summary_body->addWidget(summary_label_);
    layout->addWidget(summary_card);

    // ---------- 底部操作按钮 ----------
    QWidget* footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("exportButtonBox"));
    QHBoxLayout* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(0, 0, 0, 0);
    footer_layout->setSpacing(Spacing::Gap::Normal);
    footer_layout->addStretch(1);
    auto* cancel_button = FluentUi::MakeButton(
        footer, QStringLiteral("取消"), FluentUi::ButtonKind::Standard);
    cancel_button->setObjectName(QStringLiteral("exportCancelButton"));
    cancel_button->setMinimumWidth(96);
    cancel_button->setMinimumHeight(Spacing::ControlHeight::Standard);
    cancel_button->setAutoDefault(true);
    footer_layout->addWidget(cancel_button);
    auto* accept_button = FluentUi::MakeButton(
        footer, QStringLiteral("确定"), FluentUi::ButtonKind::Accent);
    accept_button->setObjectName(QStringLiteral("exportAcceptButton"));
    accept_button->setMinimumWidth(96);
    accept_button->setMinimumHeight(Spacing::ControlHeight::Standard);
    accept_button->setAutoDefault(true);
    accept_button->setDefault(true);
    footer_layout->addWidget(accept_button);
    layout->addWidget(footer);
    connect(accept_button, &QPushButton::clicked, this,
            &ExportSliceDialog::accept);
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);

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

    const std::size_t support_part_count = support_part_count_;
    const std::size_t model_part_count =
        part_count_ >= support_part_count ? part_count_ - support_part_count : 0;

    QString text = QStringLiteral("模型 part：%1 个 | 层厚：%2")
        .arg(NumberText(model_part_count), ThicknessText(layer_thickness_mm_));
    text += QStringLiteral("\n切片层：%1 层（有效 %2 层，空层 %3 层）")
        .arg(NumberText(layer_count_))
        .arg(NumberText(valid_layers))
        .arg(NumberText(empty_layer_count_));
    text += QStringLiteral("\n本次写入：%1 层 | 轮廓 %2 条 | 填充 %3 条")
        .arg(NumberText(written_layers))
        .arg(NumberText(contour_count_))
        .arg(NumberText(hatch_count_));
    text += QStringLiteral("\n支撑对象：%1 个（几何已计入轮廓/填充统计）")
        .arg(NumberText(support_part_count));
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
