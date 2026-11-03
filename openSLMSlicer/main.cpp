#include <QApplication>
#include <QIcon>
#include <QMessageBox>

#include "fluent_ui.h"
#include "scene_editor_window.h"
#include "scene_viewport.h"
#include "welcome_dialog.h"

int main(int argc, char* argv[])
{
    // Fluent-Qt 的 High-DPI 属性必须在 QApplication 构造前设置。
    FluentUi::PrepareApplication();
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("openSLMSlicer"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    // 品牌 logo：作为所有窗口（标题栏、任务栏、对话框）的默认图标。
    app.setWindowIcon(QIcon(QStringLiteral(":/logo/slm_logo.png")));
    // Fluent 字体/资源、Fusion 基础样式、语义化调色板和原生控件样式表。
    FluentUi::Initialize(app);
    WelcomeDialog welcome;
    while (welcome.exec() == QDialog::Accepted)
    {
        SceneViewport viewport;
        if (!viewport.Initialize())
        {
            QMessageBox::critical(&welcome, QStringLiteral("无法启动编辑器"),
                                  QStringLiteral("无法初始化 OpenGL 视口，请检查显卡和程序资源。"));
            return 1;
        }
        if (!welcome.ProjectPath().isEmpty())
        {
            QString error;
            if (!viewport.ApplyProject(welcome.TakeInitialProject(), error))
            {
                QMessageBox::warning(&welcome, QStringLiteral("无法打开工程"), error);
                continue;
            }
        }

        int exit_code = 0;
        {
            SceneEditorWindow window(viewport, welcome.ProjectPath());
            if (!window.IsReady())
                return 1;
            window.showMaximized();
            window.StartRendering();
            exit_code = app.exec();
        } // Qt releases its foreign-window wrapper before GLFW is destroyed.
        return exit_code;
    }
    return 0;
}
