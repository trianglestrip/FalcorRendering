# Filament 与 FalcorRendering 移植对比文档

本文档对比 **Filament 官方实现**（`D:/gitProject/filament`）与 **FalcorRendering 当前移植**（`Source/RenderPasses/FilamentFX` + `Source/Samples/PBRTOfflineRenderer`），重点覆盖：**渲染管线顺序、Shader 架构、光照模型、阴影、后处理、配置/UI 映射**。

> 对比基准日期：2026-06；Falcor 分支：`feature/pbrt-offline-renderer`；Filament 参考路径：`filament/src/details/Renderer.cpp`、`PostProcessManager.cpp`、`shaders/src/`、`filament/include/filament/Options.h`。

---

## 1. 总体架构差异

| 维度 | Filament | FalcorRendering 移植 |
|------|----------|-------------------|
| **编排方式** | `FrameGraph` 资源生命周期 + Pass 依赖裁剪 | `FilamentPostProcess::executeCustom()` 手写顺序调用 |
| **几何着色** | 单通道 **Forward Lit**（8 通道 RenderPass：Depth→Color→Refract→Blended） | Falcor **RasterPass** + PBRT 材质 `mi.eval()` |
| **G-Buffer** | 无完整 G-Buffer；仅有 **Structure Pass**（深度+mipmap）供 SSAO/接触阴影 | 无 Structure Pass；直接用主渲染深度 `D32Float` |
| **材质系统** | `filamat` 编译变体（光照/阴影/VSM/SSR/Fog 等） | PBRT 导入材质 + 固定 `PBRTOfflineRenderer.3d.slang` |
| **点光源** | **Froxelizer** 3D 网格索引 + Record Buffer | Falcor `gScene.getLightCount()` 逐灯循环 |
| **集成入口** | `Engine` → `View` → `Renderer::render()` | `PBRTOfflineRenderer::onFrameRender()` 直接实例化 `FilamentPostProcess` |
| **插件形态** | N/A（引擎内核） | `FilamentFX.dll`（Mogwai 插件）+ `FilamentPostProcessLib`（静态库，PBRT 使用） |

**核心结论：** Falcor 移植在 **后处理 Compute Shader** 层面对齐 Filament 数学较多；**光照与阴影仍在 PBRT/Falcor 前向路径**，与 Filament 的「SSAO/阴影在 Surface Shader 内采样」有本质差异。

---

## 2. 帧级渲染管线对比

### 2.1 Filament 官方顺序（`Renderer.cpp` → `renderJob`）

```
[CPU] View::prepare()
  ├── 场景剔除、可见光源
  ├── Froxelizer（点光源网格化，异步）
  ├── ShadowMapManager::prepare（CSM/Spot/Point 图集规划）
  └── ColorPassDescriptorSet / Lighting UBO 更新

[GPU FrameGraph]
  1. Shadow maps          → ShadowMapManager::render()
  2. Structure pass       → 缩放视口深度 + mipmap（SSAO/SSCS/SSR 输入）
  3. [可选] Picking
  4. [可选] SSAO          → screenSpaceAmbientOcclusion()  （SAO 或 GTAO）
  5. [可选] SSR           → ssr() + mipmap
  6. Prepare color pass
  7. Color pass (Forward) → 绑定 shadows / SSAO / SSR / structure
  8. [可选] MSAA custom resolve
  9. [可选] Depth resolve（TAA/DoF 需要）
 10. [可选] TAA           → taa()（可在子通道内嵌 Color Grading）
 11. [可选] DoF           → dof()（刻意在 TAA 之后，避免 fireflies）
 12. [可选] Bloom         → bloom() + lens flare
 13. [可选] Color Grading → colorGrading()（或已在 Color/TAA 子通道完成）
 14. [可选] FXAA          → fxaa()（默认开启）
 15. [可选] DSR upscale   → FSR1 / SGSR
 16. Present / Blit
```

**Filament 后处理关键顺序：** `HDR Color → TAA → DoF → Bloom → ToneMap/Grading → FXAA → Upscale`

### 2.2 FalcorRendering 当前顺序（`FilamentPostProcess::executeCustom`）

```
[PBRTOfflineRenderer::onFrameRender]
  0. [可选] Shadow depth raster   → ShadowDepth.3d.slang（光栅深度图）
  1. Main raster                  → PBRTOfflineRenderer.3d.slang → RGBA32F + D32 深度
  2. [后处理]
     0. Shadow visibility         → ShadowMap.cs.slang（屏幕空间 PCF）
     1. SSAO + 双边模糊           → SSAO.cs.slang（ssaoMain + ssaoBlur×2）
     1b. DoF                      → DoF.cs.slang
     2. Bloom down/up             → Bloom.cs.slang
     3. Color Grading 合成         → ColorGrading.cs.slang（AO/阴影/Bloom/晕影/分级/色调映射/抖动）
     4. TAA 或 FXAA               → TAA.cs.slang / FXAA.cs.slang
  3. Blit → Swapchain
```

### 2.3 管线顺序差异表

| 阶段 | Filament | Falcor 移植 | 对齐度 |
|------|----------|------------|--------|
| 阴影深度 | 图集 + CSM/Spot/Point + VSM 矩 | 单张正交深度图 | ⚠️ 低 |
| Structure / 深度金字塔 | 有（SSAO LOD 采样） | 无（`gMaxLevel=0`） | ❌ |
| SSAO 时机 | **Color Pass 之前** | Color Pass **之后**（后处理） | ⚠️ 时机不同 |
| SSAO 应用点 | **Surface IBL/indirect** | ColorGrading 按 alpha 权重近似 | ⚠️ 近似 |
| SSR | 有 | 无 | ❌ |
| 主着色 | Filament Standard + IBL | PBRT BSDF + 假天光 | ⚠️ |
| TAA vs DoF | **TAA → DoF** | **DoF → Bloom → Grading → TAA** | ❌ 顺序不同 |
| Bloom vs Grading | Bloom **先于** ToneMap | Bloom 在 Grading 内合成 | ✅ 意图接近 |
| FXAA 默认 | 默认 **开启** | 默认 AA=FXAA（`antiAliasing=1`） | ✅ |
| TAA 默认 | 默认 **关闭** | 可选 | ✅ |
| Fog | Shader 内 / 可选后处理 | 无 | ❌ |
| FSR / DSR | 有 | 无 | ❌ |

---

## 3. 光照模型对比

### 3.1 Filament 光照架构

**入口：** `shaders/src/surface_shading_lit.fs` → `evaluateLights()`

**求值顺序：**

1. **IBL / 间接光**（`surface_light_indirect.fs`）
   - 默认 `IBL_INTEGRATION_PREFILTERED_CUBEMAP`
   - Diffuse：SH 或预过滤 Cubemap
   - Specular：预过滤 Cubemap + **DFG LUT**（`sampler0_iblDFG`）
   - **SSAO 在此应用**：`diffuseAO = min(material.ambientOcclusion, ssao)`，含 Specular AO、Multi-bounce AO
2. **方向光**（`surface_light_directional.fs`）
   - 阴影：`surface_shadowing.fs`（PCF / EVSM / DPCF / PCSS）
   - Micro-shadowing 用材质 AO，**不用 SSAO 纹理**
3. **点/聚光**（`surface_light_punctual.fs`）
   - Froxel 记录查询，非全场景逐灯循环
4. **自发光**（`evaluateMaterial`）

**关键文件：**

| 文件 | 职责 |
|------|------|
| `surface_main.fs` | material() → evaluateMaterial() → fog |
| `surface_light_indirect.fs` | IBL + SSAO + SSR 混合 |
| `surface_light_directional.fs` | 太阳/方向光 |
| `surface_light_punctual.fs` | 点光源 |
| `surface_shadowing.fs` | 阴影采样 |
| `surface_ambient_occlusion.fs` | `evaluateSSAO()`、Specular AO |

### 3.2 Falcor 移植光照架构

**入口：** `Source/Samples/PBRTOfflineRenderer/PBRTOfflineRenderer.3d.slang`

```slang
// 间接：固定天空/地面半球渐变（非 IBL）
float3 indirect = albedo * skylight * 0.35f;
float3 color = emission + indirect;

// 直射：Falcor 解析光 + PBRT BSDF
for (each light) color += mi.eval(sd, ls.dir, sg) * ls.Li;

// Alpha：间接光亮度占比 → 后处理 SSAO 权重
return float4(color, ambientWeight);
```

| 特性 | Filament | Falcor 移植 |
|------|----------|------------|
| BRDF | Filament Standard（金属/电介质/布料等变体） | PBRT 原生 `mi.eval()` |
| IBL | Cubemap + SH + DFG LUT | 法线 Y 插值 sky/ground；可选 Falcor `EnvMap` UI（非 Filament 管线） |
| `iblIntensity` / `iblRotation` | `frameUniforms.iblLuminance` 等 | **结构体有字段，未接入 Shader** |
| `sunIntensity` / `sunColor` | `LightManager` + UBO | **UI 有，仅 `sunDirection` 用于阴影矩阵** |
| SSAO | `sampler0_ssao` 在 `evaluateIBL()` | 后处理 `ColorGrading`：`color *= (1 - w + w*ao)` |
| 阴影 | Per-light 在 forward shader | 后处理全屏乘 `gShadow` |
| `FilamentPBR.slangh` | N/A | 存在 GGX 辅助函数，**未被任何 Shader include** |

### 3.3 光照数据流示意

**Filament（正确路径）：**

```
Depth/Structure → SSAO Pass → ssao texture
                                    ↓
Scene lights + Shadow maps ──→ Color Pass (forward)
                                    ↓
                              HDR color buffer
                                    ↓
                         TAA → DoF → Bloom → Grading → FXAA
```

**Falcor 移植（当前路径）：**

```
Shadow depth raster ──→ shadow map
                              ↓
PBRT raster (direct+fake indirect, alpha=w) ──→ color + depth
                              ↓
ShadowMap.cs (screen PCF) + SSAO.cs + DoF + Bloom
                              ↓
ColorGrading.cs (AO×w, shadow×all, bloom, tonemap)
                              ↓
TAA / FXAA
```

---

## 4. 阴影系统对比

### 4.1 Filament（`ShadowMapManager`）

| 能力 | 说明 |
|------|------|
| **CSM** | 方向光最多 4 级联；`cascadeSplitPositions` 默认 `{0.125, 0.25, 0.50}` |
| **Spot / Point** | 图集中独立 Shadow Map（Point 为 6 面） |
| **技术** | `View::ShadowType`：PCF（默认）、VSM、DPCF、PCSS |
| **VSM** | EVSM 矩 + 高斯模糊 + Mipmap（`evsm/gaussian.mat`、`vsmMipmap.mat`） |
| **SSCS** | 屏幕空间接触阴影（Structure 深度光线步进） |
| **采样** | `surface_shadowing.fs`；`sampler2DArrayShadow`（PCF）或普通 array（VSM） |
| **Descriptor** | PCF / VSM **两套** ColorPass 描述符布局 |

**默认（`LightManager::ShadowOptions`）：** `mapSize=1024`，`shadowCascades=1`，`constantBias=0.001`，`lispsm=true`

### 4.2 Falcor 移植

| 能力 | 实现 | 差距 |
|------|------|------|
| Shadow depth | `ShadowDepth.3d.slang` 单 Pass 光栅 | 无图集、无 LiSPSM |
| 光源矩阵 | `sunDirection` + 场景 AABB 正交投影 | 非 Filament CSM 分割 |
| 可见性 | `ShadowMap.cs.slang` 重建世界坐标 → 光空间 PCF | 仅 cascade 0 有效；2–4 级为 identity |
| VSM | UI 可选，C++ 强制 `shadowType==2 → 1` | **未实现** |
| 应用方式 | `ColorGrading` 全屏 `color *= shadow` | 非 per-light forward 阴影 |
| 接触阴影 | 无 | ❌ |

**参考文件：**

- Filament：`filament/src/ShadowMapManager.cpp`，`materials/shadowmap.mat`
- Falcor：`PBRTOfflineRenderer.cpp::renderShadowMap()`，`ShadowMap.cs.slang`，`ShadowDepth.3d.slang`

---

## 5. SSAO 对比（重点）

### 5.1 Filament SAO 实现

| 项目 | 内容 |
|------|------|
| **算法** | 默认 **SAO**（`sao.mat` + `saoImpl.fs`）；可选 **GTAO** |
| **输入** | Structure 深度（带 mipmap，`mipmapDepth.mat`） |
| **核心公式** | `ssDiskRadius = -(projectionScaleRadius / origin.z)` |
| **深度线性化** | `depthUtils.fs`：`getViewFromClipMatrix()` |
| **法线** | `computeViewSpaceNormalMediumQ/HighQ`（`geometry.fs`） |
| **模糊** | 可分离双边高斯（`bilateralBlur.mat`），深度打包在 AO 纹理 GB 通道 |
| **输出** | `sampler0_ssao`：**2D Array**；R=可见度，GB=打包深度；Layer1=弯曲法线（可选） |
| **质量档** | LOW/MEDIUM/HIGH/ULTRA → sampleCount、spiralTurns、kernelSize |
| **默认** | `enabled=false`，`radius=0.3`，`resolution=0.5`，`intensity=1.0`，`bias=0.0005` |
| **SSCT** | 集成在 SAO Pass（`ssctImpl.fs`） |

### 5.2 Falcor SSAO 实现

| 项目 | 内容 |
|------|------|
| **Shader** | `SSAO.cs.slang`（`ssaoMain` + `ssaoBlur`×2） |
| **算法** | SAO 螺旋采样（已对齐 `saoImpl.fs` 快路径） |
| **深度** | 主渲染 `D32Float`；**D3D [0,1]**：0=近，1=远/天空（清空为 1） |
| **线性化** | `gInvProj` 行向量乘 clip（对齐 `depthUtils.fs`） |
| **模糊** | 11-tap 可分离双边（MEDIUM 档 stddev=4） |
| **深度金字塔** | `gMaxLevel=0`（**无 mipmap**） |
| **输出格式** | `RG32Float`：R=AO，G=归一化 view-Z |
| **应用** | **后处理**间接权重近似，非 Surface `evaluateSSAO()` |
| **未实现** | GTAO、Bent Normals、SSCT、半分辨率上采样、depth mipmap |

### 5.3 SSAO 参数映射

| Filament `AmbientOcclusionOptions` | Falcor `FilamentSettings` | 备注 |
|-----------------------------------|---------------------------|------|
| `enabled` | `enableSSAO`（默认 **true**） | Filament 默认 **false** |
| `radius` | `ssaoRadius`（0.3） | ✅ |
| `bias` | `ssaoBias`（0.001） | Filament 默认 0.0005 |
| `power` | `ssaoPower`（内部 ×2） | ✅ |
| `intensity` | `ssaoIntensity` | ✅ |
| `bilateralThreshold` | `ssaoBilateralThreshold`（0.05） | ✅ |
| `quality` → sampleCount | `ssaoSampleCount`（11） | MEDIUM≈11 |
| `spiralTurns` | `ssaoSpiralTurns`（6） | MEDIUM=6 |
| `resolution`（0.5） | 无（全分辨率） | ❌ |
| `lowPassFilter` | 固定 11-tap | 部分 |
| `upsampling` | 无 | ❌ |
| `aoType`（GTAO） | 仅 SAO | ❌ |
| `bentNormals` | 无 | ❌ |
| `minHorizonAngleRad` | `ssaoMinHorizonAngleSineSquared` | 默认 0 |

---

## 6. 后处理各 Pass 对比

### 6.1 Bloom

| 项目 | Filament | Falcor |
|------|----------|--------|
| **Shader** | `bloomDownsample.mat`、`bloomUpsample.mat`、`bloomDownsample9.mat` 等 | `Bloom.cs.slang` |
| **降采样** | 13-tap box（多质量档） | 13-tap box（对齐 Jimenez） |
| **上采样** | Tent 滤波累加 | Tent 滤波 |
| **默认** | `enabled=false`，`strength=0.10`，`levels=6`，`resolution=384` | `enableBloom=true`，`strength=0.25`，`levels=6` |
| **Threshold** | `bool threshold`（阈值 1.0）+ `highlight` | `bloomThreshold` float（默认 0） |
| **混合** | ADD / INTERPOLATE | Add / Screen |
| **Lens flare** | `flare.mat` | 无 |

### 6.2 Depth of Field

| 项目 | Filament | Falcor |
|------|----------|--------|
| **架构** | 多 Pass：CoC、tiles、median、dilate、mipmap、combine | 单 Pass `dofMain` |
| **Shader** | `dof/dofCoc.mat`、`dof.mat`、`dofCombine.mat` 等 8+ 文件 | `DoF.cs.slang` |
| **默认** | `enabled=false` | `enableDoF=false` |
| **CoC** | 物理相机光圈 + `cocScale` | `dofFocalDistance`、`dofAperture`、`dofMaxCoC` 简化 |
| **与 TAA 关系** | 明确在 TAA **之后** | 在 TAA **之前** |

### 6.3 Color Grading & Tone Mapping

| 项目 | Filament | Falcor |
|------|----------|--------|
| **实现** | `colorGrading.mat` + **3D LUT**（`ColorGrading.h`） | `ColorGrading.cs.slang` 实时公式 |
| **Tone map** | ACES Legacy（默认）、AgX、Linear、PBR Neutral 等 | ACES（Narkowicz）、Filmic、Linear、Display |
| **变换顺序** | Exposure → … → ToneMap → Gamut（见 `ColorGrading.h`） | Exposure → Contrast → Sat/Vibrance → Vignette → Bloom → **ToneMap** |
| **子通道模式** | 可在 Color Pass / TAA Pass 内嵌 Subpass | 仅独立 Compute Pass |
| **Dithering** | `View::Dithering::TEMPORAL`（默认） | Interleaved gradient noise（可选） |
| **Vignette** | 在 `colorGrading` / `postProcess` 内 | `ColorGrading.cs.slang` |

### 6.4 抗锯齿

| 项目 | Filament | Falcor |
|------|----------|--------|
| **FXAA** | `antiAliasing/fxaa/fxaa.mat`；**默认开启** | `FXAA.cs.slang` |
| **TAA** | Halton jitter、`taa.mat`、历史重投影、可选 RCAS | 同 UV 历史混合；**无 jitter/运动矢量** |
| **MSAA** | 独立 `MultiSampleAntiAliasingOptions` | 无 |
| **默认** | FXAA on，TAA off | `antiAliasing=1`（FXAA） |

---

## 7. Shader 文件对照表

### 7.1 后处理 / 屏幕空间

| Filament 材质/Shader | Falcor Slang | 对齐说明 |
|---------------------|--------------|----------|
| `ssao/sao.mat` + `saoImpl.fs` | `SSAO.cs.slang::ssaoMain` | SAO 核心公式已对齐；缺 depth mipmap |
| `ssao/bilateralBlur.mat` | `SSAO.cs.slang::ssaoBlur` | 可分离双边；深度存 RG 而非 GB pack |
| `ssao/gtao.mat` | — | 未移植 |
| `bloom/bloomDownsample.mat` | `Bloom.cs.slang::bloomDownsample` | 13-tap 对齐 |
| `bloom/bloomUpsample.mat` | `Bloom.cs.slang::bloomUpsample` | Tent 对齐 |
| `dof/*.mat`（8 个） | `DoF.cs.slang::dofMain` | 大幅简化 |
| `colorGrading/colorGrading.mat` | `ColorGrading.cs.slang` | 无 LUT；合成逻辑不同 |
| `antiAliasing/fxaa/fxaa.mat` | `FXAA.cs.slang` | 基本实现 |
| `antiAliasing/taa/taa.mat` | `TAA.cs.slang` | 缺 jitter/reprojection |
| `shadowmap.mat` | `ShadowDepth.3d.slang` | 仅深度光栅 |
| — | `ShadowMap.cs.slang` | Filament 无对应「屏幕空间阴影可见性」Pass |

### 7.2 表面光照（Filament 有 / Falcor 无对应）

| Filament | Falcor 替代 |
|----------|------------|
| `surface_shading_lit.fs` | `PBRTOfflineRenderer.3d.slang` |
| `surface_light_indirect.fs` | 假天光 + 后处理 AO |
| `surface_shadowing.fs` | `ShadowMap.cs.slang`（后处理） |
| `surface_ambient_occlusion.fs` | `ColorGrading.cs.slang`（近似） |
| `surface_fog.fs` | 无 |
| `FilamentPBR.slangh` | 未接入 |

---

## 8. 配置与 UI 参数映射

### 8.1 View 级

| Filament `View` / Options | Falcor `FilamentSettings` + UI |  wired |
|---------------------------|-------------------------------|--------|
| `hasPostProcessPass` | `postProcessingEnabled` | ✅ |
| `AntiAliasing::FXAA`（默认） | `antiAliasing`（0/1/2） | ✅ |
| `TemporalAntiAliasingOptions` | `taaFeedback` | ⚠️ 简化 TAA |
| `Dithering::TEMPORAL` | `dithering` | ✅ |
| `DynamicResolutionOptions` | 无 | ❌ |
| `RenderQuality::hdrColorBuffer` | `RGBA32Float` 中间纹理 | 格式不同 |

### 8.2 光照 / IBL

| Filament | Falcor UI | wired |
|----------|-----------|-------|
| `Light` Sun intensity/color/direction | Light (Sun) 组 | ⚠️ 仅 direction→阴影 |
| IBL cubemap + SH + rotation | Environment Map (Falcor EnvMap) | ⚠️ 非 Filament 路径 |
| `iblIntensity` / `iblRotation` | 结构体字段 | ❌ |

### 8.3 阴影

| Filament | Falcor UI | wired |
|----------|-----------|-------|
| `shadowingEnabled`（默认 true） | Enable Shadows | ✅ |
| `ShadowType` PCF/VSM/DPCF/PCSS | PCF Hard / PCF Low / VSM | ⚠️ VSM 假 |
| `shadowCascades` 1–4 | Cascades slider | ⚠️ UI only |
| `cascadeSplitPositions` | 结构体有，UI 无 | ❌ |
| `SoftShadowOptions` | 无 | ❌ |
| `VsmShadowOptions` | 无 | ❌ |

### 8.4 后处理

| Filament 默认值 | Falcor 默认值 | 差异 |
|----------------|--------------|------|
| SSAO off | SSAO **on** | 行为不同 |
| Bloom off, strength 0.10 | Bloom **on**, strength 0.25 | 行为不同 |
| DoF off | DoF off | ✅ |
| Vignette（在 grading 内） | `enableVignette=true` | 接近 |
| ACES Legacy tone map | ACES（Narkowicz 近似） | 曲线不同 |
| SSAO resolution 0.5 | 全分辨率 | 性能/质量差 |

---

## 9. 深度缓冲与坐标系

| 项目 | Filament | Falcor 移植 |
|------|----------|------------|
| 深度范围 | Reversed-Z（近=1，远=0）常见 | **D3D [0,1]**：近=0，远=1 |
| 天空判定 | 深度≈0（reversed-Z） | 深度 **> 0.9999**（清空为 1） |
| 线性化 | `getViewFromClipMatrix()` | `gInvProj` + Filament 公式 |
| View Z 符号 | 视空间 Z 常为负（OpenGL 风格） | 与 `invProj` 一致；`ssDiskRadius` 用负号公式 |
| TAA jitter | 写入投影矩阵 | 无 |

此差异是早期 SSAO「全屏发灰」的主要原因之一；当前已通过 D3D 约定 + `invProj` 修复。

---

## 10. 缺失能力与优先级建议

### 10.1 高优先级（影响视觉正确性）

1. **SSAO 接入 Forward Shader**（或延迟 G-Buffer）：对齐 `evaluateSSAO()`，Specular AO，Multi-bounce
2. **阴影 Per-light Forward 采样**：替换后处理全屏乘；实现真实 CSM 图集
3. **Structure Pass + 深度 Mipmap**：SSAO LOD 与接触阴影
4. **管线顺序**：调整为 `TAA → DoF → Bloom → Grading`
5. **TAA**：Halton jitter + 运动矢量/深度拒绝

### 10.2 中优先级（功能完整度）

6. Filament 风格 IBL（DFG LUT + 预过滤 Cubemap）
7. VSM + EVSM 模糊链
8. SSAO 半分辨率 + 双边上采样（`evaluateSSAO` 双线性权重）
9. Color Grading 3D LUT
10. 连接 `sunIntensity` / `sunColor` 到场景光源

### 10.3 低优先级 / 扩展

11. SSR、Fog、Lens Flare
12. GTAO、SSCT、Bent Normals
13. FSR / DSR 超分
14. Froxel 点光源
15. Mogwai 插件 `execute()` 传入 depth/settings

---

## 11. 代码路径速查

### Filament 参考

| 模块 | 路径 |
|------|------|
| 帧调度 | `filament/src/details/Renderer.cpp` |
| 后处理 | `filament/src/PostProcessManager.cpp` |
| 视图准备 | `filament/src/details/View.cpp` |
| 阴影 | `filament/src/ShadowMapManager.cpp` |
| 配置默认值 | `filament/include/filament/Options.h` |
| 表面光照 | `shaders/src/surface_*.fs` |
| SSAO | `filament/src/materials/ssao/*.mat`、`saoImpl.fs` |
| Bloom | `filament/src/materials/bloom/*.mat` |
| Color Pass 绑定 | `filament/src/ds/ColorPassDescriptorSet.cpp` |

### Falcor 移植

| 模块 | 路径 |
|------|------|
| 后处理调度 | `Source/RenderPasses/FilamentFX/FilamentPostProcess.cpp` |
| 配置结构体 | `Source/RenderPasses/FilamentFX/FilamentPostProcess.h` |
| 后处理 Shader | `Source/RenderPasses/FilamentFX/*.cs.slang` |
| 样例入口 | `Source/Samples/PBRTOfflineRenderer/PBRTOfflineRenderer.cpp` |
| 前向光照 | `Source/Samples/PBRTOfflineRenderer/PBRTOfflineRenderer.3d.slang` |
| 阴影深度 | `Source/Samples/PBRTOfflineRenderer/ShadowDepth.3d.slang` |
| 构建 | `Source/RenderPasses/FilamentFX/CMakeLists.txt` |
| 设计笔记 | `docs/development/Filament_Analysis.md`、`Filament_Strict_Implementation.md` |

---

## 12. 总结

| 层级 | 对齐程度 | 说明 |
|------|----------|------|
| **UI 布局** | ★★★★☆ | 分组贴近 `gltf_viewer`；部分参数未接线 |
| **后处理 Shader 数学** | ★★★☆☆ | Bloom/SAO/FXAA 较完整；DoF/TAA/Grading 简化 |
| **后处理管线顺序** | ★★☆☆☆ | DoF/TAA 顺序与 Filament 相反；无 Structure/SSR |
| **光照模型** | ★★☆☆☆ | PBRT 路径；无 Filament Standard + IBL |
| **阴影** | ★☆☆☆☆ | 单图 + 后处理 PCF；无 CSM/VSM/SSCS |
| **SSAO 数据流** | ★★☆☆☆ | 生成较对齐；**应用点**仍与 Filament 不同 |

**一句话：** 当前 FalcorRendering 是「**PBRT 前向渲染 + Filament 风格后处理叠层**」，而非 Filament 的「**统一 Forward + FrameGraph + Surface 内 SSAO/阴影/IBL**」。后处理层可作为继续严格对齐的坚实基础；若要达到 Filament 观感一致，下一步应优先把 **SSAO 与阴影移回光照阶段**，并补齐 **Structure Pass 与 CSM**。

---

*相关文档：* [Filament_Analysis.md](./Filament_Analysis.md) · [Filament_Strict_Implementation.md](./Filament_Strict_Implementation.md) · [current-architecture.md](./current-architecture.md)
