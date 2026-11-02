# PySLM C++（裁剪副本）

本目录是 PySLM C++ 0.6.1 的最小可编译源码子集，供 openSLMSlicer 作为 CMake 子工程一起编译。

保留内容：

- `src/`、`include/`：PySLM C++ 源码与公开头文件；
- `third_party/`：参与编译或提供头文件的第三方源码（GEOS、manifold、clipper2、qhull、triangle-c、fitpack、minpack、earcut、x86-simd-sort、nlohmann/json 等）；
- `CMakeLists.txt`：只保留构建目标，不再包含示例、测试、查看器和安装规则。

已移除：`apps/`（CLI 与查看器示例）、`tests/`、`examples/`、`baseline/`、`validation/`、`docs/`、`tools/`、安装规则、构建产物和预编译包；同时移除了嵌入构建不参与配置/编译的 GEOS `util`、`doxygen` 目录，以及只用于出处说明的 `pillow_raster/Draw.c`（保留其 LICENSE 与 source.lock.json）。

许可：PySLM 采用 LGPL-2.1，见 `LICENSE`；第三方组件许可见各子目录内的许可证文件与 `THIRD_PARTY_NOTICES.md`。
