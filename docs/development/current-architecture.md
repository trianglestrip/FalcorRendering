# Falcor 当前代码架构说明

本文档描述当前仓库的主要目录职责、模块边界和构建产物关系，便于快速定位功能与扩展点。

## 1. 总体分层

Falcor 代码可以粗分为四层：

1. 基础框架层：`Source/Falcor`
2. 功能扩展层：`Source/Modules`、`Source/plugins`、`Source/RenderPasses`
3. 应用与工具层：`Source/Mogwai`、`Source/Samples`、`Source/Tools`
4. 资源与测试层：`data`、`media`、`tests`

构建入口在根目录 `CMakeLists.txt`，通过 `add_subdirectory()` 组织上述模块。

## 2. 关键目录与职责

### 2.1 `Source/Falcor`（核心库）

- 渲染框架核心抽象，包含设备、资源、管线、场景、材质、RenderGraph 等基础能力。
- 为上层 RenderPass、工具和样例提供统一 API。
- 该库通常是大多数可执行目标与插件的依赖核心。

### 2.2 `Source/RenderPasses`（渲染通道实现）

- 各类可插拔渲染通道（例如 PathTracer、后处理、降噪等）。
- 通过 RenderGraph 机制组合，常被 `Mogwai` 或测试场景加载使用。

### 2.3 `Source/plugins`（导入与功能插件）

- 面向模型/场景格式与特定功能扩展的插件化实现。
- 运行时按需加载，减少核心库耦合。

### 2.4 `Source/Modules`（功能模块）

- 对核心能力的高层封装或独立模块化能力集。
- 位于核心库与具体应用之间，复用率高。

### 2.5 `Source/Mogwai`（主应用）

- 图形化运行入口之一，用于交互式加载场景、RenderGraph、脚本调试。
- 常作为验证 RenderPass 和整体框架行为的主程序。

### 2.6 `Source/Samples`（示例程序）

- 面向单一特性的示例（如 DXR、CUDA 互操作、可视化）。
- 用于最小闭环验证和新功能教学。

### 2.7 `Source/Tools`（工具与测试可执行程序）

- 例如 `FalcorTest`、`RenderGraphEditor`、`ImageCompare` 等开发工具。
- 支撑自动化测试、图调试与结果验证。

## 3. 构建与依赖组织

## 3.1 构建系统

- 使用 CMake + Presets（见 `CMakePresets.json`）。
- Windows VS2022 常用预设：`windows-vs2022`。
- 生成目录通常为：`build/windows-vs2022`。

## 3.2 外部依赖

- 第三方源码子模块位于 `external`。
- 二进制依赖通过 `tools/packman` 与 `dependencies.xml` 拉取与管理。

## 4. 运行产物与目录关系

Release 编译后，主要可执行文件位于：

- `build/windows-vs2022/bin/Release`

典型产物包括：

- `Mogwai.exe`
- `FalcorTest.exe`
- `RenderGraphEditor.exe`
- 以及各类 Sample 可执行文件。

## 5. 典型开发路径

1. 在 `Source/Falcor` 或 `Source/Modules` 扩展基础能力。
2. 在 `Source/RenderPasses` 添加/修改渲染通道。
3. 通过 `Mogwai` 或 `Samples` 快速验证。
4. 使用 `FalcorTest`、`ImageCompare` 与 `tests` 进行回归。

## 6. 架构特点（当前版本）

- 以核心库为中心，RenderPass/插件为扩展边界。
- 应用层与能力层解耦，便于研究功能快速迭代。
- 通过 CMake 子目录拆分，实现多目标并行构建与独立维护。

## 7. PBRTOfflineRenderer + FilamentFX（Filament 对齐）

样例 `Source/Samples/PBRTOfflineRenderer` 使用静态库 `FilamentPostProcessLib`（`Source/RenderPasses/FilamentFX`）：

**帧序：** Shadow CSM → Depth prepass → Structure+SSAO/GTAO → Forward(Color+IBL+AO+Shadow) → Post(TAA→DoF→Fog→Bloom→Grading→FXAA→FSR)

**主要 Shader：** `StructurePass.cs.slang`、`SSAO.cs.slang`、`GTAO.cs.slang`、`ShadowEVSM.cs.slang`、`Fog.cs.slang`、`ColorGrading.cs.slang`、`FSR.cs.slang`、`FilamentIBL.slangh`、`FilamentAO.slangh`

**构建：** `build_pbrt_renderer.bat`（单线程 MSBuild，避免 PCH 竞争）

**资产：** `data/ibl/lightroom_14b/` 部署至 `bin/Release/data/ibl/`

