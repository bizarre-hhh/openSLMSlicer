# 切片文件输出功能设计（金属 SLM）

> 目标项目：openSLMSlicer（C++20 / Qt5 / OpenGL，Windows x64，基于 pyslm_cpp 的金属 SLM 切片预览）
> 文档状态：设计提案 v0.1 ｜ 2026-10-26
> 实现状态（2026-10-29）：**M1 全清单已落地**——P0 数据模型、`.slmslice` v1 writer/reader、CLI 2.0 ASCII、SVG/PNG 联系人表、CSV 逐层统计、`IJobWriter` + `SliceExportRegistry`、导出 UI、导出后回读校验（层数/轮廓数/填充数/长度）、打开 `.slmslice` 预览/信息对话框和扩展后的 `slice_export_tests` 均已实现；Debug 与 Release 全量 CTest 4/4 通过（Release UI 用例首次运行时出现过一次 45 s 超时，重跑通过），另用独立的 Python/Pillow 解析器对五种导出做了一次第三方回读验收。CLI 的 `$$LAYER/z` 按规范写**层顶面**（切片内部仍保存层中面），详见 `README.md`、`docs/slmslice-v1.md` 与 `docs/slice-report-formats.md`。
> 结论先行：**不要自造"对外交换格式"**；自建一个**内部切片容器 `.slmslice`** 用于无损保存/回归测试；对外依次实现 **CLI → OVF → 3MF Toolpath Extension**；设备适配靠 **machine profile + build processor + 后处理插件**，而不是靠文件后缀。

---

## 1. 结论摘要

1. **输出功能 = 数据模型问题，不是文件后缀问题。** 当前 `SliceResult` 只够画预览：没有 part 归属、内外环、扫描顺序、跳转/开关光、激光与工艺参数。先补数据模型，再写 writer。
2. **内部格式可以有，对外标准不要造。** `.slmslice` 只服务本软件（无损、快速、可回归、可转换）；对外输出用已被行业接受的格式。
3. **推荐格式组合**
   - 现在：`.slmslice`（内部/无损） + **CLI**（兼容老软件和第三方导入） + **SVG/PNG**（预览、报告、质检）；
   - 近期：**OVF（Open Vector Format，格式定义 MIT，protobuf）** —— 与金属 LPBF 的"几何与工艺参数分离 + build processor"模型几乎一一对应，是当前最合适的开放设备/研究机交换格式；参考实现（C#）为 LGPL-2.1，复用时注意许可方式；
   - 中期：**3MF Toolpath Extension**（3MF 家族已进入 ISO/IEC 25422:2025；其 Toolpath 扩展 2026-09 发布 v1.0.0 RC，原生支持 SLM 的 loop/polyline/hatch、激光功率/速度、多激光、扫描场）——战略下注，等工具链和生态成熟；
   - 不做主线：G-code（不适合振镜 SLM）、厂商专有格式（无公开规范/NDA/逆向法律风险）。
4. **"适配更多设备"的现实路径**：Tier 1 开放格式（OVF/CLI/3MF）直出；Tier 2 导出通用层格式给厂商软件导入；Tier 3 专有格式交给**外部后处理插件**，主仓库不逆向、不分发密钥。任何承诺"支持所有 SLM 机器"都不现实，应发布**机器兼容性矩阵**。
5. **第一个可交付版本（建议 1～2 周）**：快照接口 + `.slmslice` + CLI + 导出对话框 + 原子写/取消/回读校验 + 单元测试。OVF 放第二步，3MF 放第三步。

---

## 2. 现状：数据链路与缺口

### 2.1 当前链路

```
STL 网格
  → StlModel::AppendBuildTrianglesMm()（stl_model.h:238，世界坐标 mm）
  → SceneViewport::GenerateSlice()（scene_viewport.cpp:2035-2044，含隐藏模型）
  → SliceBuild()（slice_engine.cpp:149）：每个模型逐层求交 + 可选 hatch
  → SliceResult{layers, 统计, issues}
  → SceneViewport 私有 slice_layers_（scene_viewport.h:313）
  → 切片预览 dock（scene_editor_window.cpp:1056-1208）
```

- 切片结果只存在视口内存里：模型/平台/变换变化后自动 `ClearSlicePreview`（scene_viewport.cpp:603-611）；预览不写工程、不进撤销（README:112）。
- UI 只能拿到 `SliceInfo()` 的聚合统计（scene_viewport.h:67-90），拿不到逐层几何。
- 因此导出功能应当是：**把"当前预览"的数据快照序列化，而不是重新切片**（所见即所得，也避免导出参数与预览参数不一致）。
- 插入点：app 层新增 `slice_export.*`；视口只加只读快照接口；`slice_engine` 保持纯几何计算、不引入文件 IO。UI 入口在"切片"菜单（scene_editor_window.cpp:205-217）和预览 dock 的按钮行（:1186-1195）。

### 2.2 数据模型缺口（按优先级）

| 优先级 | 缺口 | 现状证据 | 对导出的影响 |
|---|---|---|---|
| P0 | 轮廓没有模型归属；外轮廓/内孔被拍平为同质折线 | `SlicePath` 只有 points（slice_engine.h:25-28）；`AppendPolygons` 把 exterior/interior 一起 push（slice_engine.cpp:109-126） | 无法按 part 输出、无法区分孔洞/岛、无法做内外轮廓不同工艺 |
| P0 | 空层被省略；`index` 与 `grid_index` 语义分裂 | 空层跳过（slice_engine.cpp:471-496）；`index` 是"有效层序号"、`grid_index` 才是 Z 网格（slice_engine.h:48-49） | 机器需要完整层序列/层号；空层缺失会导致 Z 对不上 |
| P0 | Z 是层中面 `(i+0.5)t`，没有声明顶面/底面约定 | slice_engine.cpp:351-352；README:103 | 不同设备对层平面的定义不同，导出必须显式带上约定 |
| P0 | 扫描顺序、跳转、开关光、延时没有数据 | 轮廓/hatch 分开存；hatch 顺序由 AlternateSort 决定（slice_engine.cpp:234-236, 297）；跳转只在预览里临时推导（scene_viewport.cpp:2310-2326） | 无法生成真实可执行作业，只能生成"几何集合" |
| P1 | 无激光/工艺参数、无多激光、无光斑补偿后的真实扫描线 | `SliceSettings` 只有层厚 + 填充几何（slice_engine.h:37-44, 58-63）；`SliceHatchLine` 只有起终点/model_id（:30-35） | OVF/3MF 的 marking params、多激光分配、扫描场拆分无从谈起 |
| P1 | 无材料/机器/操作者/软件版本等作业元数据 | 工程文件只存平台 + STL（project_file.h:23-27） | 无法追溯、无法做机器 profile 校验 |
| P2 | 精度/闭合/单位策略不统一 | mm double；pyslm 内部 int µm（types.hpp:91,154）；`ConvertPath` 强制闭合但未规范方向（slice_engine.cpp:104-105） | 导出到 int/float 格式会引入舍入误差，需要统一舍入策略 |

> 结论：先完成 P0 的模型改造，再谈 writer；否则写出来的文件"能看不能用"。

---

## 3. 输出什么：按使用场景决定格式

| 使用场景 | 主要需求 | 推荐格式 |
|---|---|---|
| 本软件重新打开、续算、回归对比 | 无损、快、含全部元数据 | **`.slmslice`（自有容器）** |
| 发给 Magics / Netfabb / 厂商软件导入 | 通用、老软件都认 | **CLI**（几何）/ 3MF（模型+排版） |
| 发给开放设备/研究机/未来厂商控制器 | 矢量 + 工艺参数 + 多激光 | **OVF**；中期 **3MF Toolpath** |
| 人工检查、论文/报告、质检对比 | 矢量清晰、可批量出图 | **SVG / PNG**（逐层或联系人表） |
| 统计、仿真输入、CI 校验 | 结构化、可 diff | **CSV / JSON** |
| 工艺参数与机器配置 | 可编辑、可继承、可校验 | **JSON profile（本软件自有 schema）** |
| 厂商专有格式（.sli/.mtt 等） | 直接上机 | 外部后处理插件，不进主仓库 |

---

## 4. 格式对比与选型

### 4.1 CLI 2.0（Common Layer Interface）

- 定位：1994 年 Brite-EuRam 项目发布的**厂商无关逐层几何交换格式**（参与方包括 BIBA/BMW/Fiat/EOS/Mercedes-Benz 等）；它是**交换格式，不是设备控制格式**。规范原文：https://www.hmilch.net/downloads/cli_format.html
- 结构（ASCII）：
  - `$$HEADERSTART … $$HEADEREND` / `$$GEOMETRYSTART … $$GEOMETRYEND` 两段；
  - 头部：`$$ASCII` 或 `$$BINARY`、`$$UNITS/u`（坐标单位，u 为实数，通常为 mm）、`$$LAYER/z`；
  - 几何：`$$POLYLINE/id,dir,n,...`，`dir` 0=内轮廓 CW、1=外轮廓 CCW、2=开放线；`$$HATCHES/id,n,...`，每 4 个实数为一条直线的起终点；
  - `$$LABEL`、`$$USERDATA` 可携带自定义信息；`//` 为注释。
- BINARY：头部仍是 ASCII，几何区用命令号（CI 127/128 层、129/130 折线 short/long、131/132 填充），坐标为 32 位 IEEE REAL（约 7 位十进制有效数字），`$$ALIGN` 控制 32 位对齐。
- 精度：坐标单位由 `$$UNITS` 实数给出；ASCII 实数总位数 ≤ 16；BINARY 为 float32，100 mm 量级约 1e-5 mm 分辨率，对 SLM 几何足够，但不要用于微米级工艺数据。
- 能力边界：只有 **层 / 轮廓 / 填充 / part 标签**，**没有功率、速度、光斑、扫描策略、多激光**；也没有校验和。
- 注意：任务描述里常提到的 `@`/`#` 记录类型在 CLI 2.0 中不存在；公开资料中未查到 CLI 3.0 规范（以上两点均"待核实"）。
- 结论：**第一优先实现的外向格式**，与当前"轮廓折线 + 填充线"输出几乎 1:1，实现成本最低；定位为"兼容/导入格式"，不承载工艺参数。需要特别小心 BINARY 的对齐和实现差异，首版建议先只做 ASCII，再补 BINARY 并用回读验证。

### 4.2 OVF（Open Vector Format）

- 事实（有出处）：由 RWTH Aachen 的 Digital Production Aachen 维护；**格式定义仓库 MIT**，但**C# 参考实现 OpenVectorFormatTools 为 LGPL-2.1**，C++ tools 为 MIT —— 如复用参考实现代码需注意 LGPL 的链接/分发方式，只使用 `.proto`/生成代码则按格式仓库许可。**基于 Google protobuf**；对象模型为 `Job / WorkPlane / VectorBlock / MarkingParams`，支持 Hatches、LineSequences，以及圆弧/椭圆/贝塞尔等可扩展块类型；支持多扫描场、多激光（vector block 可带激光器索引）、skywriting/lookahead、Part 元数据与质量追溯；文件采用 magic + LUT + delimited protobuf 分段，适合 GB 级作业流式写。相关论文：ASTM STP 1637 (2022)，DOI 10.1520/STP163720210013。
- 关键机制：**几何与工艺参数分离**。`VectorBlock` 只存几何，通过索引引用 `MarkingParams`；`BuildProcessorStrategy` 明确"切片器输出无参数几何，build processor 再按机器/材料/Part 元数据补参数"（v1.7 引入）。这与本项目的"预览几何 + 后续工艺"结构天然吻合。
- 生态：C# 参考实现（Reader/Writer、Streaming、BuildProcessor、一致性检查）较完整；**C++ 工具较少**（有 OpenVectorFormatToolsCpp，规模很小），因此 C++ 端大概率需要引入 protobuf 或手写子集编码；采用者仍以研究/开放设备为主（市场占有率待核实）。
- 结论：**金属 SLM 场景最值得投入的开放格式**。建议作为第二阶段主输出；首版可只写几何（无 MarkingParams），由 build processor 在后续版本补齐参数。

### 4.3 3MF（Core / Slice / Toolpath Extension）

- 事实：3MF 是 XML + ZIP 容器，2025 年成为 **ISO/IEC 25422:2025** 标准族。当前版本（调研时）：Core 1.4.0（2025-02）、Production 1.2、Materials & Properties 1.2.1、**Slice 1.0.2**。
- **Slice Extension**：`slicestack / slice / sliceref`，每层 `ztop`（相对基板、单调递增）+ 顶点/多边形/线段；`pid/p1/p2` 可挂材料属性；变换限平面；**只有多边形，没有扫描矢量/激光参数**。适合"切片几何预览/存档"，不适合作为设备作业。
- **Toolpath Extension v1.0（2026-09-03 发布，状态 Release Candidate；命名空间 2026/03）** 直接面向增材制造 toolpath：
  - 支持 planar toolpath（SLA/SLM/SLS/FFF），层内用 `loop / polyline / hatch` 表达；
  - `toolpathprofile` 支持 **laserpower、laserspeed、jumpspeed、laserfocus、spotradius、laserindex**；
  - 支持 `lasersource`（多激光、扫描场 bounds/center/cornerradius）、`syncgroup`（多激光同步屏障）、`partid`/UUID（需要 Production 扩展）；
  - 支持自定义命名空间的机器专属 profile codec；
  - 每层一个 XML part（`/Toolpath/Layers/layer00001.xml`），便于随机访问/流式读，但**数据量风险**：百万级矢量用 XML 表达会很大，需关注后续 Binary Encoding 扩展和实际读写性能。
- 结论：**战略上最正确、战术上最早期**。等 lib3mf 等工具链支持 Toolpath 扩展、且我们自己有 OVF 的"工艺参数模型"经验后再实现；先做技术预研和 PoC，不阻塞首版。

### 4.4 其他

- **G-code**：为 FFF/运动控制设计，缺少振镜/激光参数语义；金属 SLM 不应作为主格式。仅当未来支持开放 G-code 控制器（DED/WAAM/DIY）时作为插件。
- **PNG/TIFF 栅格**：适合 DLP/LCD/SLA 面曝光，不适合振镜扫描的 SLM；仅用于预览/质检出图。
- **厂商专有格式**：常见归属为 SLC→3D Systems、SLI→SLM 系设备、MTT→Renishaw、SSF→Solidscape；EOS/Concept Laser/Trumpf/Additive Industries/Aconity3D 的具体作业格式与授权**没有找到公开规范，全部"待核实"**。PySLM/libSLM 支持部分格式（libSLM 自述用于"生成并传输到 SLM 机器系统"，属于第三方实现），其翻译器需向作者申请、未完全公开；直接实现面临 NDA/逆向/DMCA 风险，**不进入主仓库基线，只做机型插件**。

### 4.5 选型结论

| 阶段 | 格式 | 角色 | 理由 |
|---|---|---|---|
| 现在 | `.slmslice` | 内部无损容器 | 快速、可回归、可转任意格式 |
| 现在 | CLI 2.0（ASCII 优先） | 兼容/导入 | 成本最低、老软件普遍能导入、可做回读验证；无工艺参数 |
| 现在 | SVG/PNG + CSV | 预览/报告/CI | 调试和验收成本极低 |
| 近期 | **OVF** | 开放设备/研究机主格式 | 格式 MIT、专为 LPBF/振镜设计、几何/参数分离、支持多激光/扫描场；参考实现 LGPL 需注意 |
| 中期 | **3MF Toolpath** | 标准/厂商生态 | ISO 家族、原生 SLM 语义、多激光同步；但仍是 RC，工具链/生态未成形，需锁版本跟踪 |
| 可选 | 3MF Slice | 几何预览/存档 | 生态大、只含多边形；不能替代设备作业格式 |
| 不推荐 | 自造对外格式、G-code、厂商格式逆向 | — | 生态、语义、法律三方面都不划算 |

---

## 5. 架构设计

### 5.1 分层

```
┌──────────────────────────── UI（Qt）────────────────────────────┐
│ 切片菜单 / dock 按钮 / 导出对话框（格式、profile、选项、进度）      │
├────────────────────────── 导出服务层 ──────────────────────────┤
│ SliceExportService：快照、校验、调度、原子写、回读校验、日志        │
│   ├─ IJobWriter 注册表（Native / CLI / OVF / 3MF / SVG）          │
│   ├─ BuildProcessor（几何+profile → 带工艺参数的作业）             │
│   └─ PostProcessor（外部进程，可选）                              │
├────────────────────────── 数据模型层 ──────────────────────────┤
│ SliceBuild：Parts + Layers + ScanOps + Settings + Provenance     │
│ MachineProfile / ProcessProfile（JSON）                           │
├────────────────────────── 切片内核（无 IO）──────────────────────┤
│ SliceBuild()（现有 pyslm_cpp 几何求交 + hatch）                    │
└───────────────────────────────────────────────────────────────────┘
```

原则：

1. **切片内核不写文件**；writer 不读 UI 全局状态。
2. **writer 只序列化，不做工艺决策**；工艺参数由 build processor + profile 决定。
3. **先快照再导出**，导出过程不依赖视口/GUI 状态，可取消、可关闭窗口。
4. **原子写**：先写临时文件，成功后 rename 覆盖；失败保留旧文件（参考 `project_file.cpp:202-239` 的 `QSaveFile` 思路）。
5. **回读校验**：首版至少对逐层点数/路径长度做一致性检查；OVF/3MF 用官方/第三方 reader 做 roundtrip。

### 5.2 数据模型改造（最小可用集）

现有 `SliceLayer` 保留给预览，新增/扩展为导出用的 `SliceBuild`（或直接扩展 `SliceResult` 并补字段）：

```cpp
enum class RingRole { Outer, Inner };
enum class RegionKind { Contour, Volume, UpSkin, DownSkin, Support };

struct SliceRing {
    std::uint64_t model_id = 0;
    RingRole role = RingRole::Outer;
    RegionKind region = RegionKind::Contour;
    bool closed = true;
    std::vector<std::array<double, 2>> points; // mm，闭合环不重复首点
};

struct SliceHatchRun {              // 一段连续填充（同一 model/region/参数）
    std::uint64_t model_id = 0;
    RegionKind region = RegionKind::Volume;
    std::uint32_t laser_index = 0;
    std::vector<std::pair<Vec2d, Vec2d>> lines;
};

struct SliceLayer {
    std::int64_t grid_index = 0;    // 全局 Z 网格序号（含空层）
    double z_mm = 0.0;              // 层平面 Z
    bool empty = false;
    std::vector<SliceRing> rings;
    std::vector<SliceHatchRun> hatches;
    std::vector<ScanOp> scan_order; // 可选：ContourRef / HatchRunRef / Jump
};

struct SliceBuild {
    std::vector<SlicePart> parts;   // id、名称、源文件、变换、网格 hash
    std::vector<SliceLayer> layers; // 从首层到末层，含空层
    SliceSettings settings;
    std::string machine_profile_id, process_profile_id;
    std::string material, operator_name, app_version;
    double layer_thickness_mm = 0.0;
    ZReference z_reference = ZReference::LayerMidPlane; // MidPlane / TopSurface / BottomSurface
    std::string coordinate_system;  // "plate_corner_mm_right_handed_z_up"
    std::uint64_t source_project_revision = 0;
};
```

实施策略：

- **先加字段不破坏预览**：`SlicePath` 加 `model_id/role/closed`，`SliceHatchLine` 加 `laser_index`，`SliceResult` 加 settings/part 元数据；预览代码填默认值。
- `AppendPolygons` 在拆分 exterior/interior 时写回 model_id 与 role（slice_engine.cpp:109-126）。
- 先不追求 `scan_order` 的完整实现：M1 的 writer 允许"轮廓后填充"的固定顺序，并在文件头声明"顺序未优化"；M2 再补跳转/开关光/激光分配。
- 空层：`SliceBuild` 保留从 `grid_index = first` 到 `last` 的所有层，`empty=true` 的层只写 Z/层号（导出选项可省略）。

### 5.3 导出接口（建议新增 `slice_export.h/.cpp`）

```cpp
enum class ExportFormat { NativeSlmSlice, Cli, OpenVectorFormat, ThreeMfToolpath, Svg, Png, JsonStats };

struct ExportOptions {
    ExportFormat format = ExportFormat::Cli;
    QString output_path;
    QString machine_profile_id;
    QString process_profile_id;
    bool include_empty_layers = true;
    bool include_hatches = true;
    bool include_contours = true;
    int decimal_places = 4;             // 文本格式
    bool validate_after_write = true;
    bool open_folder_after = true;
};

struct ExportResult {
    bool ok = false;
    bool cancelled = false;
    QString error;
    std::vector<ExportIssue> issues;
    std::uint64_t bytes_written = 0;
    std::size_t layers_written = 0;
    std::size_t vectors_written = 0;
    double elapsed_ms = 0.0;
};

using ExportProgress = std::function<bool(std::size_t done, std::size_t total)>;

class IJobWriter {
public:
    virtual ~IJobWriter() = default;
    virtual QString id() const = 0;              // "cli" / "ovf" / "slmslice"
    virtual QString displayName() const = 0;
    virtual QString fileFilter() const = 0;      // "CLI (*.cli)"
    virtual QString defaultSuffix() const = 0;
    virtual bool supports(const SliceBuild&) const { return true; }
    virtual ExportResult write(const SliceBuild& build, const ExportOptions& options,
                               const ExportProgress& progress) const = 0;
};

class SliceExportService {
public:
    void registerWriter(std::unique_ptr<IJobWriter> writer);
    std::vector<WriterInfo> writers() const;
    ExportResult exportBuild(const SliceBuild& build, const ExportOptions& options,
                             const ExportProgress& progress) const;
};
```

- 只读写快照，不接 GUI；writer 内部用 `std::ofstream`/`QSaveFile` 流式写，不在内存拼完整文件。
- 加入 `readBuild()` 作为可选能力，仅 `.slmslice` 和 CLI 首版实现，用于测试和"导出后校验"。
- 不建议首版复用 `pyslm::io::FormatAdapter`：它只有注册表 + JSON adapter、`write()` 无返回值/无取消/非原子、跨 DLL 传 `std::string/vector` 要求同工具链/CRT（io.hpp:14-16），且 `int mid` 与 `uint64 id`、µm 量化会丢信息。可作为后续 CLI 适配器**上游贡献**到 pyslm_cpp，而不是在 app 里硬接。

### 5.4 BuildProcessor：补工艺参数

```
几何（part/layer/ring/hatch + region） 
   + MachineProfile（激光数、功率范围、扫描场、光斑） 
   + ProcessProfile（层厚、hatch 间距/角度、功率、速度、skywriting）
   → BuildProcessor → 每个 VectorBlock/HatchRun 的 MarkingParams / laser_index / scan order
```

- M1 可以只做 pass-through（不补参数），导出"几何作业"，文件头明确 `process_params: none`。
- M2 实现按 region 的参数映射（Contour/Volume/UpSkin/DownSkin/Support），并支持参数回退（fallback）。
- M3 增加多激光分配、扫描场拆分、扫描顺序/跳转/开关光、skywriting。

### 5.5 MachineProfile / ProcessProfile（JSON schema）

用 Qt 自带 `QJsonDocument` 即可，无需引入 JSON 库；建议发布 JSON Schema 并在启动时校验。

```jsonc
// machine profile 示例
{
  "schema": "slmslicer.machine/1",
  "id": "generic-lpbf-100",
  "vendor": "generic",
  "model": "100x100 single-laser",
  "build_volume_mm": { "x": 100, "y": 100, "z": 100 },
  "origin": "front_left_bottom",
  "z_reference": "layer_midplane",
  "layer_thickness_mm": { "min": 0.01, "max": 0.2 },
  "lasers": [
    { "id": 1, "max_power_w": 400, "wavelength_nm": 1070, "spot_diameter_mm": 0.07 }
  ],
  "scan_fields": [
    { "id": 0, "center_mm": [50, 50], "size_mm": [100, 100], "corner_radius_mm": 0 }
  ],
  "limits": { "max_vectors_per_layer": 4000000, "max_layers": 20000 },
  "formats": { "native": ["ovf", "cli"], "post_processor": null }
}
```

```jsonc
// process profile 示例
{
  "schema": "slmslicer.process/1",
  "id": "ss316l-30um-generic",
  "material": "SS316L",
  "layer_thickness_mm": 0.03,
  "contour": { "passes": 1, "offset_mm": 0.0, "power_w": 200, "speed_mm_s": 800 },
  "volume":  { "hatch_spacing_mm": 0.08, "angle_deg": 45, "angle_increment_deg": 67,
               "power_w": 250, "speed_mm_s": 1000 },
  "skin":    { "up": { "layers": 3 }, "down": { "layers": 3 } }
}
```

- Profile 采用 base → family → model → user 的**单继承覆盖**（参考 Cura definition / Orca machine profile），但**不要**照搬 Cura 的 settings 间 Python 表达式求值——静态值 + 少量派生字段即可。
- 未知字段只告警不失败（便于社区扩展）；校验失败要指出字段路径。
- 把现有 `plate_dimensions.h` 的固定尺寸、`SliceSettings` 默认值逐步改为 profile 驱动。

### 5.6 插件与后处理：外部进程优先

- **不要**首版做 in-process C++ 插件 ABI：Windows/MSVC DLL + CRT 组合很脆弱，且可能引入许可污染。
- 后处理器采用**外部可执行 + JSON 参数**（参考 PrusaSlicer post-processing scripts）：输入 `.slmslice`/`.ovf`/`.cli`，输出厂商文件；主程序按序执行、捕获 stdout/stderr、非零退出即失败并保留中间产物。
- 插件 manifest 至少包含：id、version、required API version、author、license、入口路径、支持的输入格式。
- 如果未来确实需要 in-process：用**C ABI + 版本化结构体**，不暴露 STL 类型；插件加载失败/崩溃不得影响主程序。
- 专有格式插件单独分发，主仓库不捆绑厂商代码/密钥。

### 5.7 UI / 流程

- 菜单："切片 → 导出切片文件…"（放在"清除切片预览"之后）；dock 按钮行加"导出切片…"（scene_editor_window.cpp:1186-1195）。
- 启用条件：`has_preview && !slice_invalidated`；切片过期时提示重新切片。
- 导出对话框：
  - 格式下拉（CLI / .slmslice / OVF / SVG / PNG / 统计 JSON，后续 3MF）
  - 机器 profile、工艺 profile 下拉 + "打开配置目录"
  - 输出路径（`QFileDialog`，记住上次目录/格式）
  - 选项：含空层、含填充、小数位、导出后校验、完成后打开文件夹
  - 预览摘要：层数、轮廓数、填充数、预计大小、警告（如"该格式不携带工艺参数"）
- 执行：`RenderPause` 暂停渲染 + `QProgressDialog`（沿用现有切片流程 scene_editor_window.cpp:1578-1613），后台线程或至少 `processEvents` + 守卫；导出只读，不置脏、不进 history。
- 完成后状态栏显示路径/大小/耗时；失败显示第一个错误 + 日志文件。

### 5.8 文件与性能

- 流式写；不要整包 `QByteArray`；文本格式按层 flush。
- 预计量级：100 mm 立方体、30 µm 层厚、0.08 mm 间距约数千万个点，二进制几十到几百 MB；ASCII 可达 GB 级。UI 必须显示预计大小并允许取消。
- 临时文件 + rename 原子替换；写失败/取消时清理临时文件，旧文件保持不变。
- 大文件路径使用 `std::filesystem::path`/QString 正确处理中文路径；UTF-8 写文本格式头部。

---

## 6. 自有格式 `.slmslice`（建议这样做）

### 6.1 定位

- 不是行业交换格式，不要求别人支持；只解决：**无损保存切片结果、快速重新打开、回归测试基线、格式转换的中间层**。
- 对外仍然用 CLI/OVF/3MF；`.slmslice` 通过官方提供的 `slmslice` 命令行工具转出。

### 6.2 v1 布局（QDataStream 二进制，参考 `.slmproj` 的 magic/version 约定）

```
magic       8 bytes  "SLMSLICE"
version     uint32   1
header      uint32   UTF-8 JSON 长度 + JSON（UTF-8）
payload     ...      逐层二进制块（little-endian IEEE754 double）
```

JSON header 至少包含：

```jsonc
{
  "format": "slmslice", "version": 1,
  "app": { "name": "openSLMSlicer", "version": "..." },
  "created_utc": "2026-10-26T09:00:00Z",
  "coordinate_system": {
    "unit": "mm", "origin": "plate_front_left_bottom",
    "up": "+Z", "z_reference": "layer_midplane"
  },
  "plate_mm": [100, 100, 100],
  "slice": { "layer_thickness_mm": 0.03, "first_grid_index": 0, "layer_count": 3334 },
  "parts": [ { "id": 1, "name": "cube", "source_sha256": "...", "transform": [...] } ],
  "profiles": { "machine": "...", "process": "...", "material": "SS316L" },
  "layers": { "encoding": "binary", "point_type": "float64", "offset": 1234 }
}
```

- 层数据：`grid_index(uint64) + z(float64) + flags(uint32) + ring_count(uint32) + ... + hatch_count + ...`；环点数量前缀，避免依赖分隔符。
- 可选 `preview/`：缩略 PNG/首层 SVG，便于文件管理器预览。
- 可选 `checksums`：每层/整文件 SHA-256，用于校验。
- 打包方式：v1 单文件二进制（零依赖、快）；如果社区需要"可解压查看"，v2 再改 ZIP + manifest + 逐层二进制（引入 miniz），并保留 v1 读取。

### 6.3 版本与兼容规则

- `version` 只增不改；新增字段必须在 header 中带默认值，旧版本读取时忽略未知字段。
- 任何语义变化（Z 参考面、单位、层号基）必须升 major 并在 header 显式写死，不允许"猜"。
- 提供 `slmslice-info`、`slmslice-export --format cli|ovf|svg` 两个命令行工具的规划（可先做测试用最小实现）。

---

## 7. 设备适配策略与法律边界

| 层级 | 对象 | 做法 | 承诺 |
|---|---|---|---|
| Tier 1 | 开放格式/开放设备（OVF、研究机、开源控制器） | 直接输出 OVF / CLI；提供机器 profile 模板 | 可用 |
| Tier 2 | 闭源厂商软件（Magics/Netfabb/QuantAM/EOSPRINT 等） | 导出 CLI/3MF 几何，用户在厂商软件里补工艺上机 | 视厂商导入能力，逐项验证 |
| Tier 3 | 厂商专有作业格式 | 外部后处理插件（独立分发），主仓库不逆向 | 不承诺、不合规不收录 |

- 维护一份**机器兼容性矩阵**：机器/厂商、导入格式、验证状态（已验证/理论/需厂商软件/不支持）、贡献者、日期。不要用"支持所有设备"作为宣传。
- 法务提醒：互操作逆向在部分司法辖区有条件合法，但 EULA/NDA、DMCA 1201（规避技术保护措施）是硬约束；加密/混淆格式尤其不要碰。许可方面：OVF 格式规范（MIT）、Open5x（MIT）可安全借鉴；OVF 的 C# 参考实现是 LGPL-2.1，Cura/Uranium（LGPL）、PySLM/libSLM（LGPL）、PrusaSlicer/OrcaSlicer/UVtools（AGPL）只能学架构，不复制代码。
- 主程序许可建议尽早确定（如 GPL-3.0 或 LGPL-3.0/Apache-2.0 的组合），插件通过外部进程隔离以降低许可耦合。

---

## 8. 路线图

### M1（1～2 周）导出闭环

- 数据模型 P0：`SlicePath` 加 model_id/role/closed；`AppendPolygons` 写回归属；`SliceBuild` 快照接口；空层保留。
- `slice_export.h/.cpp` + `SliceExportRegistry` + `.slmslice` writer + CLI writer + SVG/PNG/CSV。
- UI：菜单/ dock 按钮 + 导出对话框 + 进度/取消 + 原子写 + 回读校验。
- 测试：roundtrip、精度、空层、多模型、写失败原子性、取消、确定性。
- 验收：对示例 STL 导出 CLI 后，用独立解析器/第三方软件打开，层数/轮廓数/路径长度一致。

### M2（2～4 周）OVF + profile

- `MachineProfile`/`ProcessProfile` JSON + 校验 + 示例库。
- `BuildProcessor`（Contour/Volume 参数映射，先单激光）。
- OVF writer（protobuf-lite 或最小 wire 编码，含 LUT 结构与 `OVFSliceViewer` 验证）+ `.slmslice` → OVF 转换。
- 多模型/part 元数据、空层、Z 参考、单位/精度策略统一。
- 测试：OVF 回读、与 OVFSliceViewer 对照、大文件性能（>1e6 矢量）。

### M3（观察/预研）3MF Toolpath + 插件

- 跟踪 lib3mf / 3MF Toolpath 工具链；做 `.build.3mf` PoC（一层/多层），评估 XML 体积与读写性能。
- 外部后处理器协议 + 示例脚本（CLI→某开放机器）。
- 多激光/扫描场/skywriting 的 BuildProcessor。
- 机器兼容性矩阵与 CI golden files。

### 暂不做

- 不在首版实现 G-code、厂商专有格式、栅格逐层文件、扫描策略优化、支撑生成。

---

## 9. 风险与待决问题

1. **OVF 的 C++ 工具链**：protobuf 依赖会增加构建复杂度；需要评估"protobuf-lite + 生成代码" vs "最小手写 encoder"（后者维护成本高但零依赖）。建议 M2 先做 spike。
2. **3MF Toolpath 体量**：XML 逐层表达百万级矢量可能不可接受；需实测 lib3mf 与 ZIP 压缩，必要时等待二进制编码扩展。
3. **数据模型重构影响面**：`SliceLayer` 被预览/测试使用；建议新增字段 + 新结构，不做破坏性重命名，分两步迁移。
4. **工艺参数正确性**：本软件不应生成"凭猜测的"功率/速度默认值；profile 必须来自用户/材料库，并在导出时明确"参数来源"。
5. **Windows x64 / pyslm DLL 约束**：短期无法跨平台；导出模块尽量不依赖 pyslm，保持可移植性。
6. **线程安全**：导出期间模型编辑/关闭窗口；用快照 + `exporting_` 守卫。
7. **许可**：主程序与插件、第三方格式库的许可需要提前规划。

---

## 10. 参考资料

- OVF 规范与读法（格式定义 MIT）：https://github.com/Digital-Production-Aachen/OpenVectorFormat
- OVF v1.8 Release（skywriting/多激光/scanfield）：https://github.com/Digital-Production-Aachen/OpenVectorFormat/releases/tag/v1.8
- OVF BuildProcessorStrategy（v1.7）：https://github.com/Digital-Production-Aachen/OpenVectorFormat/releases/tag/v1.7
- OVF 论文（ASTM STP 1637, 2022）：https://doi.org/10.1520/STP163720210013
- OVF Tools（C#，LGPL-2.1）：https://github.com/Digital-Production-Aachen/OpenVectorFormatTools
- OVF Tools C++（MIT）：https://github.com/Digital-Production-Aachen/OpenVectorFormatToolsCpp
- OVF Slice Viewer：https://github.com/Digital-Production-Aachen/OVFSliceViewer
- CLI 2.0 规范原文：https://www.hmilch.net/downloads/cli_format.html
- 3MF 规范总览（ISO/IEC 25422:2025）：https://3mf.io/spec/
- 3MF Core 1.4 / Production / Materials / Slice：https://github.com/3MFConsortium/spec_core 、https://github.com/3MFConsortium/spec_production 、https://github.com/3MFConsortium/spec_materials 、https://github.com/3MFConsortium/spec_slice
- 3MF Toolpath Extension v1.0（RC）：https://github.com/3MFConsortium/spec_toolpath
- PySLM 文档（libSLM 厂商格式清单）：https://pyslm.readthedocs.io/en/latest/
- libSLM（格式翻译器需申请）：https://github.com/drlukeparry/libSLM
- Cura 插件结构：https://github.com/Ultimaker/Cura/tree/main/plugins
- Cura 机器定义示例：https://github.com/Ultimaker/Cura/blob/main/resources/definitions/ultimaker_s5.def.json
- PrusaSlicer 后处理脚本：https://help.prusa3d.com/article/post-processing-scripts_283913
- PrusaSlicer 插件 API：https://github.com/prusa3d/PrusaSlicer/blob/master/doc/Plugin_API.md
- Freemelt openmelt：https://gitlab.com/freemelt/openmelt
- Open5x：https://github.com/FreddieHong19/Open5x

> 注：CLI 2.0 的字段/单位细节已按规范原文核对；OVF 在商业 SLM 设备上的实际采用率、3MF Toolpath 的最终字段与工具链支持仍不足，标注为"待核实"。3MF Toolpath 仍是 RC，命名空间与字段存在变更可能，实现时必须锁定版本。所有格式在实现前都需要用目标软件/设备做实测。
