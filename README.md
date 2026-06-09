![](docs/images/teaser.png)

# Falcor — PBRT 离线渲染 + Filament 对齐（当前改动说明）

本仓库在 [NVIDIA Falcor](https://github.com/NVIDIAGameWorks/Falcor) 基础上扩展 **PBRT v4 离线预览** 与 **Filament 风格渲染管线**，分支：`feature/pbrt-offline-renderer`。

> 上游完整介绍、特性列表、Python/CUDA/OptiX 说明见 [官方 README](https://github.com/NVIDIAGameWorks/Falcor/blob/master/README.md) 与 [文档索引](./docs/index.md)。

---

## 当前改动摘要

### 1. `Source/Samples/PBRTOfflineRenderer` — Kitchen 场景样例

- 加载 PBRT v4 场景（网格、材质、相机、面光源），支持交互预览与 headless 单帧 PNG 输出
- 前向着色：`PBRTOfflineRenderer.3d.slang` 内接 Filament IBL split-sum、SSAO/GTAO、CSM 阴影采样
- UI 分组贴近 Filament `gltf_viewer`：Sun/IBL、阴影、后处理参数可调
- 默认场景：`scene-v4.pbrt`（可用 `--scene` / `--output` 指定）
- CMake 输出可执行文件名为 **`pbrt_viewer.exe`**（目标名仍为 `PBRTOfflineRenderer`）

### 2. `Source/RenderPasses/FilamentFX` — 后处理静态库

C++ 调度入口：`FilamentPostProcess.cpp/h`、`FilamentIBL.cpp/h`、`FilamentFX.cpp`。

| 子目录 | 内容 |
|--------|------|
| **`Utils/`** | 公共 Slang 头：`FilamentDepth.slangh`（深度打包/view-Z）、`FilamentGeometry.slangh`、`FilamentNoise.slangh` |
| **`AO/`** | 环境光遮蔽：`StructurePass.cs.slang`（深度 Mipmap 金字塔）、`SSAO.cs.slang` / `GTAO.cs.slang`、`DeferredSSAO.cs.slang`；`FilamentAO.slangh`（双边上采样、`evaluateSSAO`）、`FilamentSAOImpl.slangh` / `FilamentGTAOImpl.slangh` / `FilamentAOBilateral.slangh` |
| **`Shadow/`** | CSM 阴影：`ShadowMap.cs.slang`、`ShadowEVSM.cs.slang`（PCF/VSM/EVSM 矩模糊链）、`FilamentShadow.slangh` |
| **`Lighting/`** | IBL / PBR：`FilamentIBL.slangh`、`FilamentIBLData.slang`、`FilamentPBR.slangh` |
| **`PostProcess/`** | 后处理 Compute：`TAA`、`DoF`、`Fog`、`Bloom`、`ColorGrading`（CPU 烘焙 3D LUT + 全分辨率 AO 评估）、`FXAA`、`FSR`（RCAS 锐化） |

**帧序：** Shadow CSM → Depth prepass → Structure+SSAO/GTAO → Forward(Color+IBL+AO+Shadow) → Post(TAA→DoF→Fog→Bloom→Grading→FXAA→FSR)

**资产：** `data/ibl/lightroom_14b/`、`dfg.dds` 部署至 `bin/Release/data/ibl/`

### 3. Mogwai 插件

- `FilamentPostProcess::execute()` 支持可选 depth / motionVec / shadowMap 输入，便于 RenderGraph 复用

### 4. 已实现 vs 简化

| 已实现（Wave 0–3） | 简化 / 未实现 |
|-------------------|---------------|
| IBL、CSM、TAA Halton jitter、SSAO/GTAO 半分辨率 + view-Z 双边上采样 | 完整 SSR 光线步进 |
| VSM/EVSM、3D LUT、Fog、GTAO 可选 | FSR EASU 动态分辨率 |
| FSR RCAS、独立 depth prepass | Froxel 点光、SSCT、Lens Flare |

详细对照见 [Filament_vs_Falcor_Comparison.md](./docs/development/Filament_vs_Falcor_Comparison.md) §12。

---

## 源码目录结构

仓库根目录按 **框架核心 → 扩展 → 应用 → 资源/工具** 分层组织。

### 根目录

| 目录 / 文件 | 说明 |
|-------------|------|
| **`Source/`** | 全部 C++/Slang 源码（见下节） |
| **`cmake/`** | CMake 模块与辅助脚本 |
| **`external/`** | 上游子模块：fmt、glfw、imgui、pybind11、vulkan-headers 等 |
| **`third_party/`** | 本分支额外依赖：pbrtio（PBRT 解析）、taskflow、glm、tinygltf 等 |
| **`data/`** | 运行时数据（IBL、LUT、测试场景片段等） |
| **`media/`** | 示例媒体资源 |
| **`models/`** | 本地 OBJ/MTL 等测试模型（非 PBRT 主路径） |
| **`docs/`** | 文档与开发笔记（架构、Filament 对齐表、调试图） |
| **`tests/`** | 单元测试、图像测试与 `run_*.bat` |
| **`tools/`** | packman 依赖拉取、代码格式化、新建 Sample/RenderPass 脚本 |
| **`build_scripts/`** | 部署与打包辅助 |
| **`build/`** | CMake 生成目录（`windows-vs2022` 等，本地构建产物） |
| **`setup_vs2022.bat`** | 首次 CMake 配置（VS2022 preset） |
| **`build_pbrt_renderer.bat`** | 增量 / 全量 / clean 编译 PBRT 样例 |
| **`view_kitchen.bat`** | 交互打开 kitchen 场景（`--preview`） |
| **`render_kitchen.bat`** | headless 单帧渲染 kitchen 到 PNG |
| **`pbrt_viewer.bat`** | 通用 PBRT viewer 启动器 |

### `Source/` — 应用与框架

| 目录 | 说明 |
|------|------|
| **`Falcor/`** | 渲染框架核心库：设备与资源、场景与材质、RenderGraph、DiffRendering、Testing 等 |
| **`Modules/`** | 高层功能模块（如 `USDUtils`） |
| **`RenderPasses/`** | 可插拔渲染通道，由 RenderGraph 组合；含 PathTracer、TAA、GBuffer、**FilamentFX** 等 |
| **`plugins/importers/`** | 运行时插件：Assimp、Mitsuba、**PBRT**、Python、USD 等场景导入 |
| **`Mogwai/`** | 图形化主程序，交互加载场景与 RenderGraph |
| **`Samples/`** | 独立示例：`PBRTOfflineRenderer`（本分支重点）、HelloDXR、CudaInterop 等 |
| **`Tools/`** | 开发工具：`FalcorTest`、`RenderGraphEditor`、`ImageCompare` 等 |

### `Source/Falcor/` — 核心库子模块

| 子目录 | 职责 |
|--------|------|
| **`Core/`** | 设备、上下文、资源、着色器编译、窗口与输入 |
| **`Scene/`** | 场景图、网格、相机、灯光、材质系统 |
| **`Rendering/`** | 渲染状态、管线、采样器 |
| **`RenderGraph/`** | RenderGraph 与 Pass 调度 |
| **`RenderPasses/`** | 核心库内置 Pass 基类与公共接口 |
| **`DiffRendering/`** | 可微 / 差分渲染相关 |
| **`Utils/`** | 数学、图像、日志等通用工具 |
| **`Testing/`** | 框架内测试辅助 |

### `Source/RenderPasses/` — 常见 Pass（节选）

除 **FilamentFX** 外，仓库仍保留上游大量 Pass，例如：`PathTracer`、`MinimalPathTracer`、`GBuffer`、`TAA`、`ToneMapper`、`NRDPass`、`DLSSPass`、`RTXDIPass`、`OptixDenoiser`、`SVGFPass` 等。新增或修改渲染特性时，优先在对应 Pass 目录下扩展，并通过 Mogwai / Sample 验证。

更完整的分层说明见 [current-architecture.md](./docs/development/current-architecture.md)。

---

## 构建（Windows / VS2022）

首次配置：

```bat
cd D:\gitProject\FalcorRendering
setup_vs2022.bat
```

日常最小编译（推荐）：

```bat
build_pbrt_renderer.bat
```

| 模式 | 命令 | 说明 |
|------|------|------|
| 增量（默认） | `build_pbrt_renderer.bat` | 仅编译 `PBRTOfflineRenderer` 及变更依赖，`/m:1` |
| 全量链式 | `build_pbrt_renderer.bat full` | Falcor → FilamentPostProcessLib → App |
| 清缓存 | `build_pbrt_renderer.bat clean` | Clean 后增量编译 |

产物：`build\windows-vs2022\bin\Release\pbrt_viewer.exe`

> **注意：** 勿对全 solution 使用 `-j` 或 `/m:8`，易触发 `LNK1104`/`LNK1181`（`ScriptBindings.obj`、`cmake_pch.obj` 竞争）。

## 运行

| 脚本 | 用途 |
|------|------|
| `view_kitchen.bat` | 交互预览 kitchen（`--preview`） |
| `render_kitchen.bat` | headless 单帧 PNG（默认输出到 `docs/development/debug_images/`） |
| `pbrt_viewer.bat` | 打开默认 PBRT 场景或传入自定义参数 |

或手动：

```bat
build\windows-vs2022\bin\Release\pbrt_viewer.exe --preview --scene D:\gitProject\VLR_WF\models\kitchen\scene-v4.pbrt
build\windows-vs2022\bin\Release\pbrt_viewer.exe --headless --single-frame --scene D:\path\scene.pbrt --output out.png
```

---

## 与 Filament 分支的关系

[filament](https://github.com/trianglestrip/filament) 的 `feature/pbrt-kitchen` 在原生 Filament 上验证 PBRT 几何与矩形面光源；本分支在 **Falcor** 上实现 Filament 风格后处理与 IBL 对齐，用于 kitchen 场景观感交叉对比。

---

## 上游 Falcor 构建与环境

- **前置条件：** Windows 10 20H2+、VS2022、Windows 10 SDK 10.0.19041.0、支持 DXR 的 GPU
- **CMake Presets：** `windows-vs2022`（VS 方案）、`windows-ninja-msvc`（VS Code）
- **可选：** NVAPI、CUDA、OptiX、DLSS/RTXDI/NRD — 见 [官方 README](https://github.com/NVIDIAGameWorks/Falcor/blob/master/README.md)

## 相关文档

- [current-architecture.md](./docs/development/current-architecture.md) — 仓库分层与 PBRTOfflineRenderer 架构
- [Filament_vs_Falcor_Comparison.md](./docs/development/Filament_vs_Falcor_Comparison.md) — 逐项对齐表
- [Filament_Strict_Implementation.md](./docs/development/Filament_Strict_Implementation.md) — 实现约束

## Citation

若在研究项目中引用 Falcor，请引用上游项目：

```bibtex
@Misc{Kallweit22,
   author =      {Simon Kallweit and Petrik Clarberg and Craig Kolb and Tom{'a}{\v s} Davidovi{\v c} and Kai-Hwa Yao and Theresa Foley and Yong He and Lifan Wu and Lucy Chen and Tomas Akenine-M{\"o}ller and Chris Wyman and Cyril Crassin and Nir Benty},
   title =       {The {Falcor} Rendering Framework},
   year =        {2022},
   month =        {8},
   url =         {https://github.com/NVIDIAGameWorks/Falcor},
   note =        {\url{https://github.com/NVIDIAGameWorks/Falcor}}
}
```
