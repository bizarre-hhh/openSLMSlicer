#include "welcome_dialog.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLayout>
#include <QListWidget>
#include <QMessageBox>
#include <QPaintEvent>
#include <QPushButton>
#include <QShowEvent>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

#include <FluentQt/BasicInput.h>
#include <FluentQt/Design.h>
#include <FluentQt/Layout.h>
#include <FluentQt/TextFields.h>

#include "fluent_ui.h"

#include <utility>

namespace
{
using Button = fluent::basicinput::Button;
using Label = fluent::textfields::Label;
using LabelColor = fluent::textfields::Label::TextColorRole;

constexpr int kRecentRowHeight = 64;
constexpr int kRecentRowSpacing = 4;
// 条目间距 4px、条目高 64px -> 每条占用 72px（列表整体高度按最新 5 条计算）。
constexpr int kRecentRowStride = kRecentRowHeight + 2 * kRecentRowSpacing;

// 最近工程列表：每行高度固定，但把整叠条目的高度作为首选高度上报。
// 这样窗口足够大时一次显示最新 5 个工程，窗口被压小（最低 640x480）时
// 列表是卡片里唯一可压缩的区域，条目在列表内部滚动，对话框布局不会溢出。
class RecentProjectList final : public QListWidget
{
public:
    explicit RecentProjectList(QWidget* parent) : QListWidget(parent) {}

    void SetPreferredRowsHeight(int height)
    {
        if (preferred_rows_height_ == height)
            return;
        preferred_rows_height_ = height;
        updateGeometry();
    }

    QSize sizeHint() const override
    {
        QSize hint = QListWidget::sizeHint();
        hint.setHeight(preferred_rows_height_);
        return hint;
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        EnsureItemWidgetsLaidOut();
        QListWidget::paintEvent(event);
    }

private:
    // QWidget::grab()（截图模式）的离屏渲染不会给从未显示过的窗口补发子控件
    // 的 resize 事件，setItemWidget() 的行控件可能仍停留在创建时的宽度。
    // 绘制前按当前可视区域摆放一次行控件并强制重排行布局，让截图与真实显示一致。
    void EnsureItemWidgetsLaidOut()
    {
        for (int i = 0; i < count(); ++i)
        {
            QListWidgetItem* entry = item(i);
            QWidget* row = entry ? itemWidget(entry) : nullptr;
            if (!row)
                continue;
            const QRect rect = visualItemRect(entry);
            if (rect.isValid() && row->geometry() != rect)
                row->setGeometry(rect);
            if (QLayout* row_layout = row->layout())
            {
                // QLayout::activate() 在布局已被激活过时会直接返回，先 invalidate
                // 才能按新的行宽重新排布图标和文字。
                row_layout->invalidate();
                row_layout->activate();
            }
        }
    }

    int preferred_rows_height_ = kRecentRowStride;
};

// 带省略号的 Fluent 标签。fluent::textfields::Label 只在 resizeEvent 里重算
// 省略文本，而 QWidget::grab()（截图模式）不会给从未显示过的控件补发 resize
// 事件；这里在绘制前按当前宽度校一次，保证离屏截图和真实显示一致。
class ElidedLabel final : public Label
{
public:
    ElidedLabel(const QString& text, Qt::TextElideMode mode, QWidget* parent)
        : Label(text, parent), mode_(mode), full_text_(text)
    {
        setTextElideMode(mode);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setMinimumWidth(0);
        setToolTip(text);
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        SyncElidedText();
        Label::paintEvent(event);
    }

private:
    void SyncElidedText()
    {
        const int available = qMax(0, contentsRect().width());
        const QString expected = fontMetrics().elidedText(full_text_, mode_, available);
        if (QLabel::text() == expected)
            return;
        // Label::setText() 只接受完整文本，切换省略模式会强制按当前宽度重算。
        setTextElideMode(Qt::ElideNone);
        setTextElideMode(mode_);
    }

    Qt::TextElideMode mode_;
    QString full_text_;
};

Label* CreateLabel(QWidget* parent, const QString& object_name, const QString& text,
                   Typography::FontRole role, LabelColor color)
{
    auto* label = new Label(text, parent);
    label->setObjectName(object_name);
    label->setFluentTypography(role);
    label->setTextColorRole(color);
    return label;
}

Label* CreateElidedLabel(QWidget* parent, const QString& object_name, const QString& text,
                         Qt::TextElideMode mode, Typography::FontRole role, LabelColor color)
{
    auto* label = new ElidedLabel(text, mode, parent);
    label->setObjectName(object_name);
    label->setFluentTypography(role);
    label->setTextColorRole(color);
    return label;
}
} // namespace

WelcomeDialog::WelcomeDialog(const QString& recent_projects_file)
    : recent_projects_file_(recent_projects_file)
{
    setObjectName(QStringLiteral("welcomeDialog"));
    setWindowTitle(QStringLiteral("欢迎使用 openSLMSlicer"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    resize(1160, 900);
    // 卡片约 800px 高；更小的窗口由列表内部滚动吸收，因此最小可以到 640x480。
    setMinimumSize(640, 480);
    // 正文字体与语义色由 FluentUi 统一提供；不再写内联样式表。
    FluentUi::PolishDialog(*this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Spacing::XLarge, Spacing::Large, Spacing::XLarge, Spacing::Standard);
    layout->setSpacing(Spacing::Small);
    layout->addStretch(1);

    auto* card = new fluent::layout::Card(this);
    card->setObjectName(QStringLiteral("welcomeCard"));
    card->setAppearance(fluent::layout::Card::Layer);
    card->setBorderVisible(true);
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(Spacing::XLarge, Spacing::Large, Spacing::XLarge, Spacing::Large);
    card_layout->setSpacing(Spacing::Medium);

    auto* logo = new Label(card);
    logo->setObjectName(QStringLiteral("welcomeLogo"));
    logo->setAlignment(Qt::AlignCenter);
    logo->setPixmap(QIcon(QStringLiteral(":/logo/slm_logo.png")).pixmap(QSize(96, 96)));
    logo->setFixedHeight(96);
    card_layout->addWidget(logo);

    auto* title = CreateLabel(card, QStringLiteral("welcomeTitle"),
                              QStringLiteral("欢迎使用 openSLMSlicer"),
                              Typography::FontRole::Title, LabelColor::Primary);
    title->setAlignment(Qt::AlignCenter);
    card_layout->addWidget(title);

    auto* subtitle = CreateLabel(card, QStringLiteral("welcomeSubtitle"),
                                 QStringLiteral("新建一个场景，或打开工程继续编辑。"),
                                 Typography::FontRole::BodyLarge, LabelColor::Secondary);
    subtitle->setAlignment(Qt::AlignCenter);
    subtitle->setWordWrap(true);
    card_layout->addWidget(subtitle);
    card_layout->addSpacing(Spacing::Small);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(Spacing::Standard);
    buttons->addStretch(1);
    auto* create = new Button(QStringLiteral("新建工程"), card);
    create->setObjectName(QStringLiteral("newProjectButton"));
    create->setFluentStyle(Button::Accent);
    create->setFluentSize(Button::Large);
    create->setMinimumSize(176, Spacing::ControlHeight::Large);
    create->setDefault(true);
    auto* open = new Button(QStringLiteral("打开工程…"), card);
    open->setObjectName(QStringLiteral("openProjectButton"));
    open->setFluentStyle(Button::Standard);
    open->setFluentSize(Button::Large);
    open->setMinimumSize(176, Spacing::ControlHeight::Large);
    buttons->addWidget(create);
    buttons->addWidget(open);
    buttons->addStretch(1);
    card_layout->addLayout(buttons);
    card_layout->addSpacing(Spacing::Small);

    auto* recent_title = CreateLabel(card, QStringLiteral("recentProjectsTitle"),
                                     QStringLiteral("最近打开的工程"),
                                     Typography::FontRole::Subtitle, LabelColor::Primary);
    card_layout->addWidget(recent_title);

    recent_list_ = new RecentProjectList(card);
    recent_list_->setObjectName(QStringLiteral("recentProjectList"));
    recent_list_->setIconSize(QSize(32, 32));
    recent_list_->setSpacing(kRecentRowSpacing);
    recent_list_->setFrameShape(QFrame::NoFrame);
    recent_list_->setTextElideMode(Qt::ElideMiddle);
    recent_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    recent_list_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    recent_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    recent_list_->setCursor(Qt::PointingHandCursor);
    recent_list_->setMinimumHeight(0);
    recent_list_->setAccessibleName(QStringLiteral("最近打开的工程，单击即可打开"));
    // 列表占用卡片里唯一可伸缩的空间：窗口大时按条目数撑开，窗口小时先被压缩。
    card_layout->addWidget(recent_list_, 1);

    recent_empty_label_ = CreateLabel(card, QStringLiteral("emptyRecentProjects"),
                                      QStringLiteral("暂无最近工程，可以新建工程或选择打开工程。"),
                                      Typography::FontRole::Body, LabelColor::Secondary);
    recent_empty_label_->setWordWrap(true);
    card_layout->addWidget(recent_empty_label_);

    const auto open_recent = [this](QListWidgetItem* item)
    {
        if (item)
            OpenProjectPath(item->data(Qt::UserRole).toString());
    };
    connect(recent_list_, &QListWidget::itemClicked, this, open_recent);
    connect(recent_list_, &QListWidget::itemActivated, this, open_recent);
    RefreshRecentProjects();

    auto* hint = CreateLabel(card, QStringLiteral("welcomeSubtitle"),
                             QStringLiteral("工程文件会保存模型和场景位置，随时继续上次的工作。"),
                             Typography::FontRole::Body, LabelColor::Tertiary);
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    card_layout->addWidget(hint);

    // 退出是对话框的关闭操作：放在卡片底部右侧作为 Fluent 命令行，而不是
    // 悬在卡片外面的裸文字。
    auto* exit = new Button(QStringLiteral("退出"), card);
    exit->setObjectName(QStringLiteral("exitWelcomeButton"));
    exit->setFluentStyle(Button::Standard);
    auto* card_footer = new QHBoxLayout;
    card_footer->addStretch(1);
    card_footer->addWidget(exit);
    card_layout->addLayout(card_footer);

    layout->addWidget(card);
    layout->addStretch(1);

    connect(create, &QPushButton::clicked, this, [this]()
    {
        project_path_.clear();
        initial_project_ = ProjectData{};
        accept();
    });
    connect(open, &QPushButton::clicked, this, &WelcomeDialog::OpenProject);
    connect(exit, &QPushButton::clicked, this, &QDialog::reject);
}

void WelcomeDialog::OpenProject()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开工程"), ProjectFile::DefaultOpenDirectory(), ProjectFile::Filter());
    if (!path.isEmpty())
        OpenProjectPath(path);
}

void WelcomeDialog::OpenProjectPath(const QString& path)
{
    ProjectData project;
    QString error;
    if (!ProjectFile::Read(path, project, error))
    {
        QMessageBox::warning(this, QStringLiteral("无法打开工程"), error);
        return;
    }
    project_path_ = QFileInfo(path).absoluteFilePath();
    initial_project_ = std::move(project);
    accept();
}

void WelcomeDialog::RefreshRecentProjects()
{
    recent_list_->clear();
    const QStringList paths = RecentProjects::Read(recent_projects_file_);
    for (const QString& path : paths)
    {
        const QFileInfo info(path);
        const QString native_path = QDir::toNativeSeparators(path);
        const QString name = info.exists() ? info.fileName()
            : QStringLiteral("%1（文件不存在）").arg(info.fileName());
        auto* item = new QListWidgetItem(recent_list_);
        item->setData(Qt::UserRole, path);
        item->setData(Qt::AccessibleTextRole, name + QStringLiteral("\n") + native_path);
        item->setToolTip(native_path + QStringLiteral("\n单击打开工程"));
        item->setSizeHint(QSize(0, kRecentRowHeight));
        auto* row = new QWidget(recent_list_->viewport());
        // Let the list receive clicks anywhere within the icon or either label.
        row->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(Spacing::Standard, Spacing::Small, Spacing::Standard, Spacing::Small);
        row_layout->setSpacing(Spacing::Standard);
        auto* icon = new Label(row);
        icon->setAlignment(Qt::AlignCenter);
        icon->setPixmap(QIcon(QStringLiteral(":/icons/folder-open.svg")).pixmap(32, 32));
        icon->setFixedSize(34, 34);
        row_layout->addWidget(icon);
        auto* text_layout = new QVBoxLayout;
        text_layout->setSpacing(Spacing::XSmall);
        auto* name_label = CreateElidedLabel(row, QStringLiteral("recentProjectName"), name,
                                             Qt::ElideRight, Typography::FontRole::BodyStrong,
                                             LabelColor::Primary);
        auto* path_label = CreateElidedLabel(row, QStringLiteral("recentProjectPath"), native_path,
                                             Qt::ElideMiddle, Typography::FontRole::Caption,
                                             LabelColor::Secondary);
        text_layout->addWidget(name_label);
        text_layout->addWidget(path_label);
        row_layout->addLayout(text_layout, 1);
        // 两行文字在高 64px 的条目里垂直居中，图标由布局居中。
        row_layout->setAlignment(text_layout, Qt::AlignVCenter);
        recent_list_->setItemWidget(item, row);
    }
    static_cast<RecentProjectList*>(recent_list_)
        ->SetPreferredRowsHeight(paths.isEmpty() ? kRecentRowStride : paths.size() * kRecentRowStride);
    recent_list_->setVisible(!paths.isEmpty());
    recent_empty_label_->setVisible(paths.isEmpty());
}

void WelcomeDialog::showEvent(QShowEvent* event)
{
    RefreshRecentProjects();
    QDialog::showEvent(event);
}
