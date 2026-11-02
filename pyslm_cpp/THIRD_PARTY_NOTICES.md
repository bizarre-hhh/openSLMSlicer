# 源码与依赖署名

本项目由 Luke Parry 等作者的 PySLM 0.6.1 移植，保留原项目 LGPL-2.1。C++ 实现还按锁定版本的 Trimesh、NumPy、pyclipr、SciPy 和 scikit-image 算法行为进行移植；对应作者、版权及许可全文随 `licenses` 目录提供。

| 组件 | 使用方式 | 许可/记录 |
|---|---|---|
| PySLM 0.6.1 | 库功能移植、原版基准 | LGPL-2.1，原项目 `LICENSE` |
| Clipper2 2.0.1 | 多边形裁剪、偏置、伪 Z 顺序 | Boost Software License 1.0 |
| GEOS 3.14.1 | 多边形构建、修复、简化、布尔 | LGPL-2.1；Ryu 另有 Boost/Apache 许可 |
| Manifold 3.5.4 | 原生网格布尔 | Apache-2.0 |
| Triangle 1.6 | 原生二维三角剖分 | Jonathan Richard Shewchuk 原许可，完整文本见 Triangle-README.txt |
| mapbox earcut | 原生耳切三角剖分 | ISC，见 earcut-LICENSE |
| Qhull 2020.2 / 8.0.2 | 保留 SciPy 的凸包顶点顺序 | Qhull COPYING；SciPy 调试输出补丁见 `third_party/qhull/SCIPY_PATCHES.md` |
| SciPy 1.18.1 FITPACK / MINPACK | 纯 C 样条和圆拟合数值实现 | BSD-3-Clause，源文件按固定提交保留 |
| Intel x86-simd-sort | NumPy 相同的 AVX2 同值排序 | BSD-3-Clause |
| nlohmann JSON 3.11.3 | 开放交换格式、命令行输入输出 | MIT |
| Trimesh 4.12.2 | 切片图、重采样、支撑等行为移植 | MIT |
| NumPy 2.5.4 | 求和和排序等数值行为移植 | BSD-3-Clause 等，完整 wheel 许可保留 |
| pyclipr 0.1.8 | Clipper 缩放/树结构行为移植 | 随附其原 LICENSE |
| scikit-image 0.26.0 | marching squares 与轮廓拼接移植 | BSD-3-Clause |
| Pillow 12.3.0 | polygon 扫描线位图填充移植 | 随附 MIT-CMU 许可，源码与归档摘要见 `third_party/pillow_raster` |
| Qt 5.14.2 Widgets | 独立示例查看器 | 原生 DLL 不使用 Qt；查看器采用已安装 Qt 运行库及其 LGPLv3 等许可 |

依赖源码下载地址、固定提交、归档摘要见 `third_party/sources.lock.json`、`third_party/qhull.lock.json` 及数值组件的 `source.lock.json`。正常构建使用本地源文件，不下载浮动版本。

Triangle 原文对商业系统分发另有要求；本地构建按已确认的个人使用场景提供。这里保留许可原文和组件来源，未代替各组件的原始许可条件。Windows 系统库和 MSVC 运行库分别来自操作系统及本机 Visual Studio 安装。
