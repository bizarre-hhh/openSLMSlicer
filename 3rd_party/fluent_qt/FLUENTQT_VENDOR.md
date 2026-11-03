# Fluent-Qt（vendored fork）

本目录是 **Fluent-Qt 1.11.0** 的裁剪副本，作为 openSLMSlicer 的 Qt Widgets 界面组件库一起编译。
上游项目：<https://github.com/calvinhxx/Fluent-Qt>（MIT License，见 `LICENSE`）。

## 为什么放在仓库里

- 上游要求 Qt 5.15+ / 6.2+，本工程使用 **Qt 5.14.2**（唯一可用的 Qt kit）。本副本加了 4 处
  兼容补丁（见下），因此必须随工程一起维护，不能用 `FetchContent` 直接拉上游。
- 与 `pyslm_cpp/`、`3rd_party/glfw` 等依赖一致：随仓库提供，除 Qt 外不需要额外安装或联网。

## 构建方式

由 `openSLMSlicer/CMakeLists.txt` 以 `add_subdirectory()` 引入（二进制目录
`<build>/_deps/fluentqt-build`），只编译核心静态库 `FluentQt::FluentQt`：

```cmake
set(FLUENTQT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3rd_party/fluent_qt"
  CACHE PATH "Fluent-Qt 源码目录（包含 CMakeLists.txt）")
add_subdirectory("${FLUENTQT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/fluentqt-build")
```

以下可选目标全部强制关闭：`FLUENT_QT_BUILD_{TESTS,GALLERY,EXAMPLES,PARTICLES,PLUGINS,
SPATIAL,QML_BRIDGE,PYSIDE6_BINDINGS,WEBASSEMBLY_SUPPORT}`、`FLUENT_QT_INSTALL`、
`FLUENT_QT_ENABLE_WARNINGS`。核心库只依赖 Qt Core/Gui/Widgets，不依赖 spdlog、OpenGL 或网络模块。

## 对 Qt 5.14.2 的兼容补丁

补丁用 `QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)` 守卫，升级到 Qt 5.15+/6 后自动走上游原生路径，
无需回退。源码中的注释统一以 `VENDOR PATCH (openSLMSlicer)` 标记。

| # | 文件 | 改动 |
|---|------|------|
| 1 | `CMakeLists.txt`（`Enforce minimum supported Qt versions` 段） | Qt 5 的 `FATAL_ERROR` 改为 `WARNING`，允许 5.14 继续配置 |
| 2 | `src/compatibility/WidgetCompat.h` — `fluentLabelPixmapValue()` | `QLabel::pixmap(Qt::ReturnByValue)`（5.15 新增）改为条件编译；5.14 下用返回借用指针的 `QLabel::pixmap()` 并立即拷贝 |
| 3 | `src/compatibility/WindowChromeCompat.cpp` — `beginSystemMove()` | `QWindow::startSystemMove()`（5.15 新增）加版本守卫；5.14 下直接走已有的 `detail::beginPlatformSystemMove()`（Win32 `WM_NCLBUTTONDOWN` 回退） |
| 4 | `src/compatibility/WindowChromeCompat.cpp` — `beginSystemResize()` | 同上，`startSystemResize()` 守卫后走 `detail::beginPlatformSystemResize()` |

半透明 `Qt::ReturnByValue`/`QWindow::startSystemMove` 只用于「客户区窗口 chrome」；openSLMSlicer
保留原生窗口边框，不使用 `fluent::windowing::Window` 的自绘标题栏，因此第 3、4 条在常规使用中
不会触发。

### 运行时初始化顺序（必须）

```cpp
FluentUi::PrepareApplication();          // 内部调用 fluent::prepareHighDpiApplication()，必须在 QApplication 之前
QApplication app(argc, argv);
FluentUi::Initialize(app);                // fluent::initializeResources() + 字体 + Fusion 样式 + 调色板 + 样式表
```

`openSLMSlicer/main.cpp` 和 `openSLMSlicer/tests/project_tests.cpp` 都走同一条路径，保证测试截图
与真实程序外观一致。

## 裁剪范围

保留：`CMakeLists.txt`、`cmake/`、`include/`、`src/`、`res/`、`uilib/`、`plugin/`、`platforms/`、
`resources.qrc`、`LICENSE`、`THIRD_PARTY_NOTICES.md`、`README.md`。
删除：`docs/`、`app/`（Gallery）、`tests/`、`examples/`、`site/`、`support/`、`tools/`、`scripts/`、
`bindings/`、`third_party/`、`.github/`、`.githooks/`、`.agents/`——这些都是上游开发/Gallery/绑定
用的，不影响核心库编译。

## 升级步骤

1. 用新版本上游源码替换本目录（保留本文件与 `LICENSE`/`THIRD_PARTY_NOTICES.md`）。
2. 重新应用上表 4 处补丁；如果上游已经支持 Qt 5.14，可删除对应补丁。
3. `cmake --build out/build/<preset> --target project_tests` 编译，跑 `ctest` 的 UI 用例。
4. 用 `project_tests.exe --capture-window/--capture-welcome/--capture-slice …` 截图对比 Light/Dark
   与中文排版，确认字体、菜单、卡片和按钮没有回归。
