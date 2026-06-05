![](docs/images/teaser.png)

# Falcor — PBRT 离线渲染 + Filament 对齐（当前改动说明）

本仓库在 [NVIDIA Falcor](https://github.com/NVIDIAGameWorks/Falcor) 基础上扩展 **PBRT v4 离线预览** 与 **Filament 风格渲染管线**，分支：`feature/pbrt-offline-renderer`。

> 上游完整介绍、特性列表、Python/CUDA/OptiX 说明见 [官方 README](https://github.com/NVIDIAGameWorks/Falcor/blob/master/README.md) 与 [文档索引](./docs/index.md)。

---

## 当前改动摘要

### 1. `Source/Samples/PBRTOfflineRenderer` — Kitchen 场景样例

- 加载 PBRT v4 场景（网格、材质、相机、面光源），输出 PNG
- 前向着色：`PBRTOfflineRenderer.3d.slang` 内接 Filament IBL split-sum、SSAO/GTAO、CSM 阴影采样
- UI 分组贴近 Filament `gltf_viewer`：Sun/IBL、阴影、后处理参数可调
- 默认场景：`scene-v4.pbrt`（可用 `--scene` / `--output` 指定）

### 2. `Source/RenderPasses/FilamentFX` — 后处理静态库

| 模块 | 说明 |
|------|------|
| `FilamentPostProcess` | 帧调度：TAA → DoF → Fog → Bloom → Color Grading → FXAA → FSR |
| `StructurePass` | 深度 Mipmap 金字塔，供 SSAO/SSR stub 使用 |
| `SSAO` / `GTAO` | SAO 默认路径；UI 可切换 GTAO；半分辨率 + 前向双边上采样 |
| `ShadowEVSM` | CSM 图集 + PCF/VSM/EVSM 矩模糊链 |
| `ColorGrading` | CPU 烘焙 3D LUT |
| `Fog` / `FSR` | 体积雾；FSR 为 RCAS 锐化（非完整 EASU） |
| `FilamentIBL` / `FilamentAO` / `FilamentShadow` | 共享 Slang 头：IBL、AO 评估、阴影采样 |

**帧序：** Shadow CSM → Depth prepass → Structure+SSAO/GTAO → Forward(Color+IBL+AO+Shadow) → Post

**资产：** `data/ibl/lightroom_14b/`、`dfg.dds` 部署至 `bin/Release/data/ibl/`

### 3. Mogwai 插件

- `FilamentPostProcess::execute()` 支持可选 depth / motionVec / shadowMap 输入，便于 RenderGraph 复用

### 4. 已实现 vs 简化

| 已实现（Wave 0–3） | 简化 / 未实现 |
|-------------------|---------------|
| IBL、CSM、TAA Halton jitter、SSAO 半分辨率 | 完整 SSR 光线步进 |
| VSM/EVSM、3D LUT、Fog、GTAO 可选 | FSR EASU 动态分辨率 |
| FSR RCAS、独立 depth prepass | Froxel 点光、SSCT、Lens Flare |

详细对照见 [Filament_vs_Falcor_Comparison.md](./docs/development/Filament_vs_Falcor_Comparison.md) §12。

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

产物：`build\windows-vs2022\bin\Release\PBRTOfflineRenderer.exe`

> **注意：** 勿对全 solution 使用 `-j` 或 `/m:8`，易触发 `LNK1104`/`LNK1181`（`ScriptBindings.obj`、`cmake_pch.obj` 竞争）。

## 运行

```bat
render_kitchen.bat
```

或手动：

```bat
build\windows-vs2022\bin\Release\PBRTOfflineRenderer.exe --scene D:\gitProject\VLR_WF\models\kitchen\scene-v4.pbrt --output scene-v4_render.png
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

- [current-architecture.md](./docs/development/current-architecture.md) §7 — PBRTOfflineRenderer 架构
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
