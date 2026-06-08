# Lumen 在 Falcor 中的实现计划

> **目标**: 在 FalcorRendering 框架中实现 UE5 Lumen 风格的实时动态全局光照系统。
> **基础**: Falcor 已有的 DXR 光追、SDF 体系、RenderGraph、RTXDI、后处理管线。
> **日期**: 2026-06-09

---

## 目录

1. [Lumen 系统概述](#1-lumen-系统概述)
2. [Falcor 现有能力评估](#2-falcor-现有能力评估)
3. [总体架构设计](#3-总体架构设计)
4. [分阶段实现计划](#4-分阶段实现计划)
5. [Phase 1: 基础设施层](#5-phase-1-基础设施层)
6. [Phase 2: Surface Cache](#6-phase-2-surface-cache)
7. [Phase 3: Screen Probe Gather](#7-phase-3-screen-probe-gather)
8. [Phase 4: Radiance Cache](#8-phase-4-radiance-cache)
9. [Phase 5: 反射系统](#9-phase-5-反射系统)
10. [Phase 6: 硬件光追集成](#10-phase-6-硬件光追集成)
11. [Phase 7: 集成与优化](#11-phase-7-集成与优化)
12. [文件清单](#12-文件清单)
13. [里程碑与时间估算](#13-里程碑与时间估算)

---

## 1. Lumen 系统概述

Lumen 是 UE5 的实时动态全局光照系统，核心特性：

| 特性 | 说明 |
|------|------|
| **完全不依赖预烘焙** | 所有光照实时计算，支持动态几何/光源/材质 |
| **多层级追踪** | 屏幕空间追踪 → 距离场追踪 → 硬件光追 (自动降级) |
| **Surface Cache** | 将场景几何参数化为 Card，缓存表面属性 |
| **Radiance Cache** | 世界空间 3D 探针网格存储辐照度 |
| **Screen Probe Gather** | 屏幕空间探针 + 世界空间探针的混合 GI |
| **高质量反射** | 支持多次弹射的镜面反射 |

### 1.1 Lumen 管线流程图

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Lumen GI Pipeline                            │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌──────────┐    ┌──────────────┐    ┌───────────────────┐         │
│  │ GBuffer   │───▶│ Surface Cache│───▶│ Screen Probe      │         │
│  │ (已有)    │    │ (Card Atlas) │    │ Gather (SS GI)    │         │
│  └──────────┘    └──────┬───────┘    └────────┬──────────┘         │
│                         │                     │                     │
│                         ▼                     ▼                     │
│                  ┌──────────────┐    ┌───────────────────┐         │
│                  │ Mesh Distance │    │ Radiance Cache    │         │
│                  │ Fields (SDF)  │    │ (World Probes)    │         │
│                  └──────┬───────┘    └────────┬──────────┘         │
│                         │                     │                     │
│                         └──────────┬──────────┘                     │
│                                    ▼                                │
│                           ┌───────────────────┐                     │
│                           │ Lumen Reflections │                     │
│                           │ (Specular GI)     │                     │
│                           └────────┬──────────┘                     │
│                                    │                                │
│                                    ▼                                │
│                           ┌───────────────────┐                     │
│                           │ Final Lighting    │                     │
│                           │ Composite         │                     │
│                           └───────────────────┘                     │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────┐      │
│  │ 可选硬件光追后端 (DXR) - 替代 SDF 追踪提供更高精度       │      │
│  └──────────────────────────────────────────────────────────┘      │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 2. Falcor 现有能力评估

### 2.1 ✅ 可直接复用

| Falcor 能力 | 对应 Lumen 需求 | 复用程度 |
|-------------|----------------|----------|
| **DXR 光追基础设施** (`Core/API/Raytracing.h`, `RtAccelerationStructure`, `RtStateObject`, `ShaderTable`) | 硬件光追后端 | 🔥 100% |
| **SDF 体系** (`Scene/SDFs/`, `SDFSVSVoxelizer.cs.slang`, `SDFVoxelTypes`) | Mesh Distance Field 生成与追踪 | 🔥 80% |
| **RenderGraph** (`RenderGraph.h/cpp`, `RenderPass.h/cpp`) | 所有 Pass 的调度与资源管理 | 🔥 100% |
| **GBuffer** (`GBuffer/` pass, Forward PBR) | Surface Cache 数据源 | 🔥 90% |
| **球谐函数** (`FilamentIBL` 9-band SH, `EnvMapSampler`) | Radiance Cache 探针存储 | 🔥 70% |
| **RTXDI** (`Rendering/RTXDI/`) | 直接光照采样 (与 GI 互补) | 🔥 80% |
| **NRD 降噪** (`NRDPass/`, NVIDIA Realistic Denoising) | GI/反射降噪 | 🔥 90% |
| **TAA** (`TAA/` pass) | 时序累积 | 🔥 90% |
| **NanoVDB** (依赖) | 稀疏体素 (可选加速结构) | 🔥 60% |
| **Slang 着色器编译器** | 所有 Lumen 着色器 | 🔥 100% |
| **后处理管线** (Bloom, DoF, ToneMapping) | 最终合成 | 🔥 100% |

### 2.2 ⚠️ 需要扩展

| 需求 | 现状 | 差距 |
|------|------|------|
| **Card 表示** | 无 | 需从零实现 Card 生成/选择/Atlas |
| **Mesh SDF 生成** | 有基础 SDF 体系但无 Mesh→SDF 工具链 | 需写 Compute Shader 生成 Mesh SDF |
| **Global Distance Field** | 无 | 需合并 Mesh SDF 为全局低精度 SDF |
| **Screen Probe Gather** | 无 | 需实现屏幕探针放置/追踪/积分 |
| **Radiance Cache 3D Grid** | 无 | 需实现世界空间探针网格 |
| **Surface Cache 反馈** | 无 | 需实现 Card 更新策略 |
| **多次弹射 GI** | 无 (现有 GI 仅一次弹射) | 需实现递归/迭代弹射 |

### 2.3 ❌ 缺失

- 无任何形式的实时 GI 缓存系统
- 无 Card/Surfel 表示
- 无世界空间辐照度探针
- 无 Mesh Distance Field 预计算管线

---

## 3. 总体架构设计

### 3.1 模块划分

```
Source/
├── RenderPasses/
│   ├── Lumen/                              # ★ 新目录: Lumen 所有 Pass
│   │   ├── LumenSurfaceCache/              # Surface Cache (Card 生成与管理)
│   │   ├── LumenMeshSDF/                   # Mesh Distance Field 生成
│   │   ├── LumenGlobalSDF/                 # Global Distance Field (合并)
│   │   ├── LumenScreenProbeGather/         # 屏幕探针收集
│   │   ├── LumenRadianceCache/             # 世界空间探针网格
│   │   ├── LumenReflections/               # Lumen 反射
│   │   ├── LumenSceneLighting/             # 最终光照合成
│   │   └── LumenVisualize/                 # 调试可视化
│   └── ... (现有 Pass)
│
├── Shaders/
│   └── Lumen/                              # ★ 新目录: Lumen Slang 着色器库
│       ├── LumenCard.slang                 # Card 数据结构
│       ├── LumenSurfaceCache.slang         # Surface Cache 着色器
│       ├── LumenMeshSDF.slang              # Mesh SDF 生成
│       ├── LumenGlobalSDF.slang            # 全局 SDF 追踪
│       ├── LumenScreenProbeGather.slang    # 屏幕探针
│       ├── LumenRadianceCache.slang        # 辐照度缓存
│       ├── LumenReflections.slang          # 反射追踪
│       ├── LumenTracing.slang              # 统一追踪接口
│       └── LumenUtils.slang                # 共享工具函数
│
├── Samples/
│   └── LumenDemo/                          # ★ 新 Sample: Lumen 演示程序
│       └── LumenDemo.cpp/h
│
└── Falcor/
    └── Rendering/
        └── Lumen/                          # ★ 新目录: Lumen C++ 基础设施
            ├── LumenTypes.h                # 共享数据类型
            ├── LumenCard.h/cpp             # Card 生成与管理
            ├── LumenMeshSDF.h/cpp          # Mesh SDF 管理
            └── LumenSettings.h             # 全局配置
```

### 3.2 数据流设计

```
每帧数据流:

1. GBuffer Render (已有)
   ├── Albedo, Normal, Depth, Roughness, Metallic, Emissive
   └── → Surface Cache Update

2. Surface Cache Update
   ├── 输入: GBuffer + 上一帧 Card 状态
   ├── Card 选择 (哪些 Mesh 需要更新)
   ├── Card Rasterize (渲染 Card 到 Atlas)
   └── → Card Atlas (Albedo, Normal, Depth, Emissive)

3. Mesh Distance Field Update (异步 / 按需)
   ├── 输入: Mesh 几何
   ├── Compute Shader: 32³/64³ jump flood → SDF
   ├── Mip SDF 金字塔
   └── → Per-Mesh SDF Textures

4. Global Distance Field Update
   ├── 输入: 所有动态 Mesh 的 SDF + 变换
   ├── Object→World min SDF 合并
   └── → Global SDF Volume

5. Screen Probe Gather
   ├── 输入: GBuffer + Card Atlas + Global SDF / HW RT
   ├── 自适应屏幕探针放置
   ├── 半球追踪 (SDF 或 HW RT)
   ├── Card 采样积分
   └── → Screen Probe Irradiance (SH)

6. Radiance Cache Update
   ├── 输入: Screen Probe 结果 / 上一帧 Cache
   ├── 世界空间探针标记 (哪些需要更新)
   ├── 探针追踪积分
   └── → World Space Radiance Cache (SH)

7. Lumen Reflections
   ├── 输入: GBuffer Roughness + Radiance Cache + Global SDF
   ├── 屏幕空间追踪 → 距离场追踪 → HW RT
   └── → Specular Indirect

8. Final Lighting Composite
   ├── Direct Light (RTXDI / 已有)
   ├── + Indirect Diffuse (Radiance Cache)
   ├── + Indirect Specular (Reflections)
   └── → HDR Output → PostFX Pipeline
```

---

## 4. 分阶段实现计划

```
Phase 1  ████████░░░░░░░░░░░░  基础设施 (Mesh SDF, Card 数据结构)
Phase 2  ████████████████░░░░  Surface Cache
Phase 3  ████████████████████  Screen Probe Gather
Phase 4  ████████████████░░░░  Radiance Cache
Phase 5  ████████████████░░░░  反射系统
Phase 6  ████████░░░░░░░░░░░░  硬件光追集成
Phase 7  ████████████████████  集成优化 & Demo
```

---

## 5. Phase 1: 基础设施层

### 5.1 Mesh Distance Field 生成

**目标**: 为每个 Mesh 生成高精度 SDF，合并为全局低精度 SDF。

#### 5.1.1 技术方案

```
Mesh SDF 生成流程:
┌──────────────┐     ┌──────────────────┐     ┌─────────────────┐
│ Triangle Mesh│────▶│ Voxelize +        │────▶│ Jump Flood      │
│ (已有)       │     │ Signed Distance   │     │ Algorithm       │
│              │     │ Init (Compute)    │     │ (2×JFA Passes)  │
└──────────────┘     └──────────────────┘     └────────┬────────┘
                                                       │
                                                       ▼
                                              ┌─────────────────┐
                                              │ Mip SDF Pyramid │
                                              │ (逐级下采样)    │
                                              └────────┬────────┘
                                                       │
                                                       ▼
                                              ┌─────────────────┐
                                              │ Global SDF      │
                                              │ (Object→World   │
                                              │  min 合并)      │
                                              └─────────────────┘
```

**Jump Flood Algorithm (JFA)**:
- 输入: 二值体素 (内部/外部)
- 2×JFA Pass: 每个 Pass 传播最近表面点距离
- 输出: 32³ 或 64³ 有符号距离场 3D Texture
- 性能: O(N³ log N) → 实际 ~2ms @ 64³

#### 5.1.2 实现文件

| 文件 | 类型 | 职责 |
|------|------|------|
| `LumenMeshSDF.h/cpp` | C++ | SDF 资源管理, CPU 端生成调度 |
| `LumenMeshSDF.cs.slang` | Slang Compute | 体素化 + JFA SDF 生成 |
| `LumenGlobalSDF.h/cpp` | C++ | 全局 SDF 合并管理 |
| `LumenGlobalSDF.cs.slang` | Slang Compute | Object→World SDF 合并 |

#### 5.1.3 关键数据结构

```cpp
// LumenTypes.h
struct LumenMeshSDFData {
    uint3   resolution;      // 32 or 64
    float3  localBoundsMin;
    float3  localBoundsMax;
    float3  localToWorldScale;
    // GPU resources
    ref<Texture> pSDFTexture;      // 3D R32F
    ref<Texture> pMipSDFTextures[4]; // Mip chain
};

struct LumenGlobalSDFData {
    uint3   resolution;       // e.g. 128³ or 256³
    float3  worldBoundsMin;
    float3  worldBoundsMax;
    float   voxelSize;
    ref<Texture> pGlobalSDF;  // 3D R32F
    // Clipmap levels for large worlds
    uint    numClipmapLevels;
    ref<Texture> pClipmapSDF[4];
};
```

#### 5.1.4 接口设计

```cpp
class LumenMeshSDF : public Object {
public:
    static ref<LumenMeshSDF> create(ref<Device> pDevice);
    
    // 为指定 Mesh 生成/更新 SDF
    void generateSDF(ref<RenderContext> pRenderContext, 
                     const ref<TriangleMesh>& pMesh);
    
    // 获取某个 Mesh 的 SDF (如果已生成)
    const LumenMeshSDFData* getSDF(uint32_t meshID) const;
    
    // 当 Mesh Transform 变化时更新 Global SDF 中的对应区域
    void markDirty(uint32_t meshID);
    
    // 绑定 SDF 数据到着色器
    void bindShaderVars(const ShaderVar& var);
    
private:
    std::unordered_map<uint32_t, LumenMeshSDFData> mMeshSDFs;
};
```

### 5.2 Card 数据结构定义

#### 5.2.1 Card 模型

```
Card (面片) = 场景中可见表面的轴对齐矩形表示

Card 属性:
┌──────────────────────────────────────────┐
│  World Position (center)                 │
│  World Normal                           │
│  Extent (half-size in UV space)         │
│  Atlas UV Rect (在 Card Atlas 中的位置)  │
│  Origin Mesh ID                         │
│  Last Update Frame                      │
│  Visible / Occluded Flag                │
└──────────────────────────────────────────┘
```

#### 5.2.2 Card Atlas

```
Card Atlas: 2048×2048 (或更大) 纹理图集
┌────────────────────────────────────────┐
│ Card 0  │ Card 1  │ Card 2  │ Card 3  │
│ (128²)  │ (128²)  │ (64²)   │ (128²)  │
├─────────┼─────────┼─────────┼─────────│
│ Card 4  │ Card 5  │ Card 6  │  ...    │
│ ...     │         │         │         │
└────────────────────────────────────────┘

每个 Card 存储:
  - AtlasAlbedo (RGB8 or R11G11B10F)
  - AtlasNormal (RGB10A2 or RGBA8)
  - AtlasDepth  (R16F)
  - AtlasEmissive (RGB8)
```

#### 5.2.3 实现文件

| 文件 | 职责 |
|------|------|
| `LumenCard.h/cpp` | Card 生成/选择/淘汰逻辑 (CPU) |
| `LumenCard.slang` | Card 数据结构 (GPU) |
| `LumenSurfaceCache.slang` | Card Atlas 读写 |

### 5.3 统一追踪接口

**目标**: 提供统一的射线追踪接口，自动选择追踪方法。

```slang
// LumenTracing.slang

// 追踪质量等级
enum TracingQuality {
    kScreenSpace = 0,   // 屏幕空间追踪 (最快)
    kDistanceField = 1, // 距离场追踪
    kHardwareRT = 2,    // 硬件光追 (最优)
};

// 追踪结果
struct LumenTraceResult {
    float3  hitPosition;
    float3  hitNormal;
    float3  hitAlbedo;      // 从 Surface Cache 采样
    float3  hitEmissive;
    float   hitDistance;
    bool    bHit;
    uint    tracingMethod;  // 实际使用的追踪方法
};

// 统一追踪函数
LumenTraceResult traceLumenScene(
    float3 rayOrigin,
    float3 rayDirection,
    float  maxDistance,
    TracingQuality minQuality
);
```

---

## 6. Phase 2: Surface Cache

### 6.1 概述

Surface Cache 是 Lumen 的核心创新：将场景表面参数化为独立的面片 (Card)，缓存表面属性 (Albedo, Normal, Depth, Emissive) 到纹理图集中。后续 GI 计算不再访问原始几何，而是采样 Card Atlas。

### 6.2 Card 生成策略

```
每帧流程:

1. Card 选择 (CPU)
   ├── 可见性查询: 哪些 Mesh 在视锥体内?
   ├── 距离 LOD: 近处用高精度 Card, 远处用低精度
   ├── 增量更新: 只更新变化的 Card (位置/材质)
   └── → Card Update List

2. Card Rasterize (GPU)
   ├── 为每个待更新的 Card 渲染一个小视口
   ├── 输出到 Card Atlas 对应 UV 区域
   └── → Card Atlas Textures

3. Card 淘汰 (CPU+GPU)
   ├── 不可见 Card: 延迟淘汰 (保留 N 帧)
   ├── 被遮挡 Card: 降低精度
   └── → Free Atlas Slots
```

### 6.3 Card Page 系统

借鉴虚拟纹理思想，Card Atlas 使用 Page Table 管理:

```
Card Page Table (256×256 pages):
┌─────────────────────────────────┐
│ Page(0,0)  Page(1,0)  ...      │  ← 每个 Page 指向 Physical Atlas 中的一个区域
│ Page(0,1)  ...                 │
│ ...                            │
└─────────────────────────────────┘

Physical Atlas (2048×2048 or 4096×4096):
  Page Size: 128×128 pixels
  Total Pages: 16×16 = 256 (for 2048 Atlas)
  或 32×32 = 1024 (for 4096 Atlas)
```

### 6.4 实现文件

| 文件 | 类型 | 职责 |
|------|------|------|
| `LumenSurfaceCache.h/cpp` | C++ RenderPass | Surface Cache Pass 主控 |
| `LumenSurfaceCache.slang` | Slang Compute+Vertex+Pixel | Card 光栅化, Atlas 管理 |
| `LumenCardPageManager.h/cpp` | C++ | Page Table 管理 |
| `LumenCardPageManager.slang` | Slang Compute | Page 分配/回收 |

### 6.5 接口设计

```cpp
class LumenSurfaceCache : public RenderPass {
public:
    static ref<LumenSurfaceCache> create(ref<Device> pDevice, 
                                          const Dictionary& dict = {});

    // RenderPass 标准接口
    virtual void execute(RenderContext* pRenderContext, 
                         const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    
    // Surface Cache 专用接口
    void updateCards(const Scene& scene, const Camera& camera);
    const ref<Texture>& getCardAtlasAlbedo() const;
    const ref<Texture>& getCardAtlasNormal() const;
    const ref<Texture>& getCardAtlasDepth() const;
    const ref<Texture>& getCardAtlasEmissive() const;
    const ref<Buffer>& getCardDataBuffer() const;  // StructuredBuffer<LumenCard>

    // 设置
    uint2 getAtlasResolution() const { return {2048, 2048}; }
    uint  getCardResolution() const { return 128; }
    uint  getMaxCards() const { return 256; }

private:
    // Atlas 纹理
    ref<Texture> mpCardAtlasAlbedo;
    ref<Texture> mpCardAtlasNormal;
    ref<Texture> mpCardAtlasDepth;
    ref<Texture> mpCardAtlasEmissive;
    
    // Card 元数据
    ref<Buffer> mpCardDataBuffer;
    ref<Buffer> mpCardPageTable;
    
    // 追踪状态
    std::vector<LumenCard> mPendingCards;
    std::vector<LumenCard> mActiveCards;
};
```

### 6.6 关键着色器伪代码

```slang
// LumenSurfaceCache.slang

// Card 光栅化入口 (per-card viewport)
[shader("vertex")]
void vsCardRasterize(
    uint cardID : SV_InstanceID,
    uint vertexID : SV_VertexID,
    out float4 svPos : SV_Position,
    out float2 uv : TEXCOORD0
) {
    LumenCard card = gCardData[cardID];
    // Render a quad covering the card's extent
    float3 worldPos = card.center 
        + card.tangentU * (uv.x - 0.5) * card.extent.x
        + card.tangentV * (uv.y - 0.5) * card.extent.y;
    svPos = mul(gCameraViewProj, float4(worldPos, 1.0));
}

[shader("pixel")]
void psCardRasterize(
    float4 svPos : SV_Position,
    float2 uv : TEXCOORD0,
    out float4 outAlbedo : SV_Target0,
    out float4 outNormal : SV_Target1,
    out float  outDepth  : SV_Target2,
    out float4 outEmissive : SV_Target3
) {
    // 采样原始 GBuffer (重投影到原始屏幕)
    float2 screenUV = svPos.xy * gScreenResInv;
    GBufferData gbuffer = sampleGBuffer(screenUV);
    
    outAlbedo = float4(gbuffer.albedo, 1.0);
    outNormal = float4(gbuffer.worldNormal * 0.5 + 0.5, 1.0);
    outDepth = gbuffer.depth;
    outEmissive = float4(gbuffer.emissive, 1.0);
}
```

---

## 7. Phase 3: Screen Probe Gather

### 7.1 概述

Screen Probe Gather 是 Lumen 的屏幕空间 GI 方案：
- 在屏幕像素上自适应放置探针
- 每个探针在半球内追踪多条射线
- 射线命中后从 Surface Cache 采样光照
- 结果积分并降噪

### 7.2 探针放置策略

```
自适应探针放置:
┌────────────────────────────────────┐
│  16×16 pixel tiles                │
│  ┌──┬──┬──┬──┐                    │
│  │  │  │  │  │  ← 每个 tile 内   │
│  ├──┼──┼──┼──┤     根据深度/法线  │
│  │  │● │  │● │     变化决定探针数  │
│  ├──┼──┼──┼──┤                    │
│  │● │  │  │  │     均匀区域: 1 探针│
│  ├──┼──┼──┼──┤     高频区域: 4 探针│
│  │  │● │  │  │                    │
│  └──┴──┴──┴──┘                    │
└────────────────────────────────────┘

探针密度控制:
  - Uniform: 每 16×16 像素 1 个探针
  - Adaptive: 根据 depth variance 动态调整
  - Max probes per frame: ~16K (1080p), ~36K (4K)
```

### 7.3 半球追踪

```
每个探针的追踪:
┌─────────────────────┐
│ Probe Position      │
│ Probe Normal        │
│                     │
│ 发射 N 条射线:      │
│  ────────────       │
│  8 rays (低质量)    │
│  16 rays (中等)     │
│  32 rays (高质量)   │
│                     │
│ 每条射线:           │
│  1. 从探针出发      │
│  2. SDF 追踪        │
│  3. 命中 → 采样     │
│     Surface Cache   │
│  4. 未命中 → 采样   │
│     Sky/EnvMap      │
└─────────────────────┘

射线方向: Fibonacci 半球分布
最大距离: 根据场景尺度自适应 (默认 200-500 cm)
```

### 7.4 SDF 追踪算法

使用 UE5 同款的 **Sphere Tracing** (Enhanced):

```slang
// LumenTracing.slang
LumenTraceResult traceDistanceField(
    float3 rayOrigin,
    float3 rayDirection,
    float maxDistance,
    float minHitDistance
) {
    const uint MAX_STEPS = 64;
    const float CONE_STEP_FACTOR = 1.0;
    
    float t = 0.0;
    float lastDist = 0.0;
    
    for (uint i = 0; i < MAX_STEPS; i++) {
        float3 pos = rayOrigin + rayDirection * t;
        
        // 采样全局 SDF (Clipmap + Mip)
        float dist = sampleGlobalSDF(pos);
        
        // Cone step optimization (UE5 关键优化)
        // 距离越远，步进越大
        float stepScale = 1.0 + CONE_STEP_FACTOR * (t / maxDistance);
        t += dist * stepScale;
        
        if (dist < minHitDistance) {
            // 命中
            float3 hitPos = rayOrigin + rayDirection * (t - lastDist * 0.5);
            float3 hitNormal = computeSDFNormal(hitPos);
            
            // 采样 Surface Cache
            LumenCardSample cardSample = sampleSurfaceCache(hitPos, hitNormal);
            
            LumenTraceResult result;
            result.hitPosition = hitPos;
            result.hitNormal = hitNormal;
            result.hitAlbedo = cardSample.albedo;
            result.hitEmissive = cardSample.emissive;
            result.hitDistance = t;
            result.bHit = true;
            result.tracingMethod = kDistanceField;
            return result;
        }
        
        if (t > maxDistance) break;
        lastDist = dist;
    }
    
    // 未命中 → 环境光
    LumenTraceResult result;
    result.bHit = false;
    return result;
}
```

### 7.5 实现文件

| 文件 | 类型 | 职责 |
|------|------|------|
| `LumenScreenProbeGather.h/cpp` | C++ RenderPass | 屏幕探针收集 Pass |
| `LumenScreenProbeGather.cs.slang` | Slang Compute | 探针放置 + 追踪 + 积分 |
| `LumenScreenProbeFilter.cs.slang` | Slang Compute | 空间滤波降噪 |
| `LumenScreenProbeTemporal.cs.slang` | Slang Compute | 时序累积 |

### 7.6 接口设计

```cpp
class LumenScreenProbeGather : public RenderPass {
public:
    static ref<LumenScreenProbeGather> create(ref<Device> pDevice);

    virtual void execute(RenderContext* pRenderContext, 
                         const RenderData& renderData) override;
    
    // 输出
    const ref<Texture>& getScreenProbeIrradiance() const;
    const ref<Texture>& getScreenProbeHitDistance() const;

    // 设置
    uint getNumRaysPerProbe() const { return mNumRaysPerProbe; }
    void setNumRaysPerProbe(uint v) { mNumRaysPerProbe = v; }

private:
    uint mNumRaysPerProbe = 16;
    float mMaxTraceDistance = 500.0f; // cm
    ref<Texture> mpScreenProbeIrradiance; // RGBA16F
    ref<Texture> mpScreenProbeHitDistance; // R16F
    ref<Texture> mpHistoryIrradiance; // For temporal
};
```

---

## 8. Phase 4: Radiance Cache

### 8.1 概述

Radiance Cache 是世界空间的 3D 探针网格，弥补屏幕空间 GI 的不足：
- 覆盖屏幕外/被遮挡区域
- 跨帧持久化，提供稳定低频 GI
- 探针存储球谐系数 (SH) 表示入射辐照度

### 8.2 探针网格设计

```
世界空间探针网格:
┌─────────────────────────────────────────┐
│  Clip 0 (最密): 覆盖摄像机周围          │
│  ┌───┬───┬───┬───┐                      │
│  │ ● │ ● │ ● │ ● │  Cell Size: ~200 cm  │
│  ├───┼───┼───┼───┤  探针间隔: ~400 cm   │
│  │ ● │ ● │ ● │ ● │                      │
│  ├───┼───┼───┼───┤                      │
│  │ ● │ ● │ ● │ ● │                      │
│  └───┴───┴───┴───┘                      │
│                                          │
│  Clip 1 (较疏): 覆盖更远区域             │
│  Cell Size: ~400 cm, 间隔: ~800 cm      │
│                                          │
│  Clip 2 (最疏): 覆盖整个场景             │
│  Cell Size: ~800 cm, 间隔: ~1600 cm     │
└─────────────────────────────────────────┘

每个探针存储:
  float3 SH[9]  (9 × float3 = 27 floats, 108 bytes)
  或 Second Order SH: float3 SH[16] (更好的高频细节)
  
网格大小 (典型):
  Clip 0: 32×32×16 = 16,384 probes
  Clip 1: 16×16×8  =  2,048 probes
  Clip 2: 8×8×4    =    256 probes
  Total: ~18,688 probes × 108 bytes ≈ 2 MB
```

### 8.3 探针更新策略

```
每帧更新流程:

1. 探针标记 (GPU Compute)
   ├── 摄像机动了 → 新探针需要追踪
   ├── 光照变化 → 受影响的探针标记脏
   ├── 时间限制: 每帧最多追踪 N 个探针
   └── → Probe Update List

2. 探针追踪 (GPU Compute)
   ├── 每个探针: 64-256 条射线半球追踪
   ├── 追踪方法: SDF (默认) 或 HW RT (可选)
   ├── 命中 → 采样 Surface Cache
   └── → 入射辐照度 (每个方向)

3. SH 投影 (GPU Compute)
   ├── 将半球采样结果投影到 SH 基
   ├── 与上一帧 SH 混合 (历史权重: ~0.8)
   └── → Final SH Coefficients

4. 探针插值 (Pixel Shader / Compute)
   ├── 对每个屏幕像素: 找周围 8 个探针
   ├── 三线性插值 SH 系数
   └── → Per-Pixel Indirect Irradiance
```

### 8.4 与 Screen Probe Gather 的融合

```
最终 Diffuse GI:
┌──────────────────────┐   ┌──────────────────────┐
│ Screen Probe Gather  │   │ Radiance Cache        │
│ (高频, 有屏幕边界)   │   │ (低频, 无屏幕边界)   │
└────────┬─────────────┘   └──────────┬───────────┘
         │                            │
         │  根据距离混合:             │
         │  weight_screen = smoothstep(│
         │    nearPlane, farPlane,     │
         │    hitDistance)             │
         │                            │
         └────────┬───────────────────┘
                  ▼
         ┌──────────────────────┐
         │ Final Diffuse GI     │
         │ (混合 + 时序累积)    │
         └──────────────────────┘
```

### 8.5 实现文件

| 文件 | 类型 | 职责 |
|------|------|------|
| `LumenRadianceCache.h/cpp` | C++ RenderPass | Radiance Cache Pass 主控 |
| `LumenRadianceCache.cs.slang` | Slang Compute | 探针追踪 + SH 投影 |
| `LumenRadianceCacheUpdate.cs.slang` | Slang Compute | 探针标记与调度 |
| `LumenRadianceCacheInterpolate.cs.slang` | Slang Compute | 探针三线性插值 |

### 8.6 接口设计

```cpp
class LumenRadianceCache : public RenderPass {
public:
    static ref<LumenRadianceCache> create(ref<Device> pDevice);
    
    virtual void execute(RenderContext* pRenderContext, 
                         const RenderData& renderData) override;
    
    // 获取辐照度数据
    const ref<Buffer>& getRadianceProbeBuffer() const;
    const ref<Texture>& getProbeIndirectionTexture() const; // 3D R32UI
    
    // 插值结果 (per-pixel irradiance)
    const ref<Texture>& getFinalIrradiance() const;
    
    // 设置
    uint3 getClip0Resolution() const { return {32, 32, 16}; }
    float getProbeSpacing() const { return 400.0f; } // cm
    uint  getNumProbeRays() const { return 128; }

private:
    // 探针数据
    ref<Buffer> mpProbeSHBuffer;           // StructuredBuffer<float3[9]>
    ref<Texture> mpProbeIndirectionTexture; // 3D R32UI (probe index or INVALID)
    ref<Buffer> mpProbeWorldPositions;      // StructuredBuffer<float3>
    ref<Buffer> mpProbeUpdateList;          // Indirect dispatch args
    
    // 输出
    ref<Texture> mpIrradianceTexture;       // RGBA16F (per-pixel)
    ref<Texture> mpHistoryIrradiance;       // For temporal
};
```

---

## 9. Phase 5: 反射系统

### 9.1 概述

Lumen Reflections 提供高质量镜面反射，同样使用多层级追踪策略：
1. **屏幕空间追踪** (SSR) → 最快
2. **距离场追踪** → 回退方案
3. **硬件光追** → 最高质量回退

### 9.2 追踪流程

```
每个像素的反射追踪:
┌─────────────────────────────────────┐
│ 输入: GBuffer (Normal, Roughness)   │
│                                     │
│ if (roughness > threshold)         │
│     skip (粗糙表面用 Diffuse GI)    │
│                                     │
│ Step 1: Screen-Space Trace         │
│   ├── Hi-Z Buffer 加速             │
│   ├── 命中 → 采样颜色              │
│   └── 未命中 → Step 2              │
│                                     │
│ Step 2: Distance Field Trace       │
│   ├── Global SDF 追踪              │
│   ├── 命中 → 采样 Surface Cache    │
│   ├── 采样 Radiance Cache          │
│   └── 未命中 → Step 3 / Sky        │
│                                     │
│ Step 3: Hardware Ray Trace (可选)  │
│   ├── DXR TraceRay                 │
│   ├── 命中 → 采样 Surface Cache    │
│   └── 未命中 → Sky/EnvMap          │
│                                     │
│ Step 4: Temporal Filter + Denoise  │
│   └── NRD / Custom Denoiser        │
└─────────────────────────────────────┘
```

### 9.3 屏幕空间追踪 (Hi-Z)

```slang
// 基于 Hi-Z 的屏幕空间反射追踪
float2 traceScreenSpaceReflection(
    float3 rayOriginView,
    float3 rayDirectionView,
    float maxSteps
) {
    // Hi-Z 加速: 每步跳过多个像素
    float t = 0.02;
    uint mipLevel = 0;
    
    for (uint i = 0; i < maxSteps; i++) {
        float3 samplePos = rayOriginView + rayDirectionView * t;
        float2 uv = viewToScreen(samplePos);
        
        if (any(uv < 0) || any(uv > 1)) break; // 出屏幕
        
        float sampleDepth = gHiZ.SampleLevel(gSampler, uv, mipLevel);
        float surfaceDepth = samplePos.z;
        
        if (surfaceDepth > sampleDepth) {
            // 在表面后面 → 缩小步长
            if (mipLevel > 0) {
                mipLevel--;
                t -= rayDirectionView.z * cellSize(mipLevel);
            } else {
                // 最精细 mip, 二分搜索
                return uv;
            }
        } else {
            mipLevel = min(mipLevel + 1, MAX_MIP);
            t += rayDirectionView.z * cellSize(mipLevel);
        }
    }
    
    return float2(-1, -1); // 未命中
}
```

### 9.4 实现文件

| 文件 | 类型 | 职责 |
|------|------|------|
| `LumenReflections.h/cpp` | C++ RenderPass | 反射 Pass 主控 |
| `LumenReflections.cs.slang` | Slang Compute | 分层追踪 (SSR→SDF→HW) |
| `LumenReflectionDenoise.cs.slang` | Slang Compute | 反射降噪 |
| `LumenHiZ.cs.slang` | Slang Compute | Hi-Z 金字塔生成 |

---

## 10. Phase 6: 硬件光追集成 (DXR Backend)

### 10.1 概述

利用 Falcor 已有的 DXR 基础设施，为 Lumen 提供最高质量的追踪回退。

### 10.2 集成方案

```
DXR 后端架构:
┌──────────────────────────────────────────┐
│ LumenRayTracing.h/cpp (新)              │
│ ─────────────────────────────            │
│ - 封装 Falcor RtStateObject             │
│ - 管理 TLAS/BLAS (复用 Scene 中的)      │
│ - 提供 "Lumen Ray Gen" Shader           │
│ - 可选的 Surface Cache 采样             │
└──────────────┬───────────────────────────┘
               │
               ▼
┌──────────────────────────────────────────┐
│ LumenHardwareRayTracing.rt.slang (新)    │
│ ─────────────────────────────────        │
│ RayGeneration Shader:                    │
│   - GI Rays (diffuse hemisphere)         │
│   - Reflection Rays (specular)           │
│   - Shadow Rays (visibility)             │
│                                          │
│ ClosestHit Shader:                       │
│   - 采样 Surface Cache (替代实际材质)    │
│   - 返回 Albedo + Emissive              │
│                                          │
│ Miss Shader:                             │
│   - 采样 Sky/EnvMap                      │
└──────────────────────────────────────────┘
```

### 10.3 追踪方法选择逻辑

```cpp
// 每帧自动选择追踪方法
TracingMethod selectTracingMethod(const LumenSettings& settings) {
    if (settings.forceMethod != kAuto) 
        return settings.forceMethod;
    
    // 从高到低尝试
    if (isHardwareRTSupported() && 
        settings.useHardwareRT && 
        !isHighQualityMode()) 
    {
        // 高质量模式优先用 HW RT
        return kHardwareRT;
    }
    
    if (isDistanceFieldReady()) 
        return kDistanceField;
    
    return kScreenSpace; // Fallback
}

// 混合模式: 近处 HW RT, 远处 SDF
float getTracingMethodForDistance(float distance) {
    if (distance < kNearFieldThreshold) 
        return kHardwareRT;
    else 
        return kDistanceField;
}
```

### 10.4 实现文件

| 文件 | 类型 | 职责 |
|------|------|------|
| `LumenHardwareRayTracing.h/cpp` | C++ | DXR 后端管理 |
| `LumenHardwareRayTracing.rt.slang` | Slang RT | RayGen/ClosestHit/Miss Shaders |
| `LumenHardwareRayTracingUtils.slang` | Slang | 共享 SH 投影/采样工具 |

---

## 11. Phase 7: 集成与优化

### 11.1 与现有管线的集成

```
PBRTOfflineRenderer 管线修改:

Frame Sequence (修改后):
┌─ CPU: Load scene, update cameras, build task graph
│
├─ GPU: Shadow Depth Pass (不变)
│
├─ GPU: Depth Prepass (不变)
│
├─ GPU: GBuffer Render (扩展)
│  └─ 额外输出: Emissive Buffer (R11G11B10F)
│
├─ GPU: Lumen Surface Cache Update ★新增
│  ├─ Card Selection
│  ├─ Card Rasterize → Card Atlas
│  └─ Card Data Output
│
├─ GPU: Lumen Mesh SDF Update ★新增 (async/按需)
│
├─ GPU: Lumen Global SDF Update ★新增
│
├─ GPU: Lumen Screen Probe Gather ★新增
│  ├─ Probe Placement
│  ├─ Distance Field Trace
│  └─ Card Atlas Sampling
│
├─ GPU: Lumen Radiance Cache Update ★新增
│  ├─ Probe Marking
│  ├─ Probe Trace + SH Project
│  └─ Per-Pixel Interpolation
│
├─ GPU: Lumen Reflections ★新增
│  ├─ Hi-Z Build
│  ├─ Multi-Level Trace
│  └─ Denoise
│
├─ GPU: Lumen Final Lighting Composite ★新增
│  ├─ Direct Light (RTXDI)
│  ├─ + Diffuse GI (Screen Probe + Radiance Cache)
│  ├─ + Specular GI (Reflections)
│  └─ → HDR Color Buffer
│
├─ GPU: Post-Processing (修改)
│  ├─ Shadow Evaluation (不变)
│  ├─ SSAO (可降级/禁用, Lumen 已提供 AO)
│  ├─ Bloom, DoF, ColorGrading (不变)
│  └─ TAA/FXAA (不变)
│
└─ Frame Display
```

### 11.2 RenderGraph 配置示例

```cpp
// PBRTOfflineRenderer.cpp 修改
void PBRTOfflineRenderer::onFrameRender(RenderContext* pRenderContext, 
                                         const Fbo::SharedPtr& pTargetFbo) 
{
    // ... existing setup ...
    
    if (mLumenEnabled) {
        // ★新增 Lumen Passes
        renderGraph->addPass("LumenSurfaceCache", "LumenSurfaceCache", 
            [](RenderPass& pass, const RenderData& renderData) {
                // Input: GBuffer textures
                // Output: Card Atlas textures
                pass.addInput("gbuffer_albedo");
                pass.addInput("gbuffer_normal");
                pass.addInput("gbuffer_depth");
                pass.addOutput("card_atlas_albedo");
                pass.addOutput("card_atlas_normal");
                pass.addOutput("card_atlas_depth");
                pass.addOutput("card_data_buffer");
            });
        
        renderGraph->addPass("LumenScreenProbeGather", "LumenScreenProbeGather",
            [](RenderPass& pass, const RenderData& renderData) {
                pass.addInput("card_atlas_albedo");
                pass.addInput("card_atlas_normal");
                pass.addInput("global_sdf");
                pass.addOutput("screen_probe_irradiance");
            });
        
        renderGraph->addPass("LumenRadianceCache", "LumenRadianceCache",
            [](RenderPass& pass, const RenderData& renderData) {
                pass.addInput("card_atlas_albedo");
                pass.addInput("global_sdf");
                pass.addOutput("radiance_cache_sh");
                pass.addOutput("per_pixel_irradiance");
            });
        
        renderGraph->addPass("LumenReflections", "LumenReflections",
            [](RenderPass& pass, const RenderData& renderData) {
                pass.addInput("card_atlas_albedo");
                pass.addInput("global_sdf");
                pass.addInput("hiz_buffer");
                pass.addOutput("reflection_color");
            });
        
        renderGraph->addPass("LumenComposite", "LumenSceneLighting",
            [](RenderPass& pass, const RenderData& renderData) {
                pass.addInput("direct_lighting");
                pass.addInput("screen_probe_irradiance");
                pass.addInput("per_pixel_irradiance");
                pass.addInput("reflection_color");
                pass.addOutput("final_lighting");
            });
    }
    
    // ... existing post-processing ...
}
```

### 11.3 性能优化策略

| 优化 | 方法 | 预期收益 |
|------|------|----------|
| **Card LOD** | 远处使用低精度 Card (64²), 近处 128² | ~30% Card Atlas 节省 |
| **自适应探针密度** | 平坦区域少探针, 高频区域多探针 | ~40% 探针数减少 |
| **时序累积** | 探针结果跨帧混合 (历史权重 0.7-0.9) | ~50% 每帧射线减少 |
| **SDF Mip** | 远处使用低精度 SDF Mip | ~3× 追踪速度提升 |
| **Cone Tracing** | SDF 追踪使用锥形步进 | ~2× 步数减少 |
| **异步 Compute** | SDF 更新在 Async Compute Queue | 隐藏延迟 |
| **NRD 集成** | 复用已有 NRD 降噪 | 免费获得高质量降噪 |
| **Half-Res** | 探针/反射在半分辨率计算 | ~4× 像素减少 |

### 11.4 调试与可视化

```cpp
// LumenVisualize Pass
enum class LumenVisualizeMode {
    kNone = 0,
    kSurfaceCacheCards,     // 显示 Card 边界
    kSurfaceCacheAlbedo,    // 显示 Card Atlas
    kMeshSDF,               // 显示 Mesh SDF 切片
    kGlobalSDF,             // 显示 Global SDF 切片
    kScreenProbes,          // 显示屏幕探针位置
    kScreenProbeIrradiance, // 显示探针积分结果
    kRadianceCache,         // 显示世界探针网格
    kReflections,           // 显示反射结果
    kTracingMethod,         // 显示追踪方法 (颜色编码)
    kNumModes
};
```

---

## 12. 文件清单

### 12.1 新建文件 (C++)

```
Source/Falcor/Rendering/Lumen/
├── LumenTypes.h                          # 共享数据类型定义
├── LumenSettings.h                       # 全局配置参数
├── LumenCard.h                           # Card 数据结构
├── LumenCard.cpp
├── LumenMeshSDF.h                        # Mesh SDF 管理
├── LumenMeshSDF.cpp

Source/RenderPasses/Lumen/
├── LumenSurfaceCache/
│   ├── LumenSurfaceCache.h               # Surface Cache Pass
│   ├── LumenSurfaceCache.cpp
│   ├── LumenCardPageManager.h            # Card Page 管理
│   └── LumenCardPageManager.cpp
├── LumenMeshSDF/
│   ├── LumenMeshSDFPass.h                # Mesh SDF Pass (RenderPass 封装)
│   └── LumenMeshSDFPass.cpp
├── LumenGlobalSDF/
│   ├── LumenGlobalSDFPass.h              # Global SDF Pass
│   └── LumenGlobalSDFPass.cpp
├── LumenScreenProbeGather/
│   ├── LumenScreenProbeGather.h          # Screen Probe Gather Pass
│   └── LumenScreenProbeGather.cpp
├── LumenRadianceCache/
│   ├── LumenRadianceCache.h              # Radiance Cache Pass
│   └── LumenRadianceCache.cpp
├── LumenReflections/
│   ├── LumenReflections.h                # Reflections Pass
│   └── LumenReflections.cpp
├── LumenSceneLighting/
│   ├── LumenSceneLighting.h              # Final Composite Pass
│   └── LumenSceneLighting.cpp
├── LumenHardwareRayTracing/
│   ├── LumenHardwareRayTracing.h         # DXR 后端
│   └── LumenHardwareRayTracing.cpp
└── LumenVisualize/
    ├── LumenVisualize.h                  # Debug 可视化
    └── LumenVisualize.cpp
```

### 12.2 新建文件 (Slang 着色器)

```
Source/Shaders/Lumen/
├── LumenTypes.slang                      # GPU 端数据类型
├── LumenCard.slang                       # Card 结构 & 采样
├── LumenTracing.slang                    # 统一追踪接口
├── LumenUtils.slang                      # 共享工具函数 (SH, Sampling, etc.)
├── LumenSurfaceCache.slang               # Card 光栅化 & Atlas 管理
├── LumenSurfaceCachePageManager.cs.slang # Page Table 管理
├── LumenMeshSDF.cs.slang                 # Mesh SDF JFA 生成
├── LumenGlobalSDF.cs.slang               # Global SDF 合并 & 追踪
├── LumenScreenProbeGather.cs.slang       # 屏幕探针 + SDF 追踪
├── LumenScreenProbeFilter.cs.slang       # 探针空间滤波
├── LumenScreenProbeTemporal.cs.slang     # 探针时序累积
├── LumenRadianceCache.cs.slang           # 世界探针追踪 & SH 投影
├── LumenRadianceCacheUpdate.cs.slang     # 探针标记 & 更新调度
├── LumenRadianceCacheInterpolate.cs.slang # 探针插值
├── LumenReflections.cs.slang             # 分层反射追踪
├── LumenReflectionDenoise.cs.slang       # 反射降噪
├── LumenHiZ.cs.slang                     # Hi-Z 金字塔
├── LumenHardwareRayTracing.rt.slang      # DXR RayGen/Hit/Miss
├── LumenHardwareRayTracingUtils.slang    # DXR 共享工具
├── LumenSceneLighting.cs.slang           # 最终光照合成
└── LumenVisualize.cs.slang               # 调试可视化
```

### 12.3 新建文件 (Sample Application)

```
Source/Samples/LumenDemo/
├── LumenDemo.h                           # Lumen 演示 App
├── LumenDemo.cpp

data/LumenDemo/
└── LumenDemo.py                          # Python 启动脚本

LumenDemo.bat                              # Windows 启动脚本
```

### 12.4 修改的现有文件

| 文件 | 修改内容 |
|------|----------|
| `Source/Samples/PBRTOfflineRenderer/PBRTOfflineRenderer.h` | 添加 Lumen 成员变量, GI 开关 |
| `Source/Samples/PBRTOfflineRenderer/PBRTOfflineRenderer.cpp` | 添加 Lumen Pass 到 RenderGraph |
| `Source/Samples/PBRTOfflineRenderer/PBRTOfflineRenderer.3d.slang` | Forward Pass 增加 Emissive 输出 |
| `Source/RenderPasses/GBuffer/` | 可能的 Emissive G-Buffer 扩展 |
| `CMakeLists.txt` (RenderPasses) | 添加 Lumen 子目录 |
| `CMakeLists.txt` (FalcorCore) | 添加 Lumen 核心文件 |
| `Source/Falcor/Scene/Scene.h` | 添加 Lumen 相关查询接口 |

---

## 13. 里程碑与时间估算

### 13.1 里程碑

| 里程碑 | 内容 | 可验证产出 | 时间估算 |
|--------|------|-----------|----------|
| **M1: 基础架构** | Mesh SDF + Card 数据结构 + 统一追踪接口 | Mesh SDF 可视化, Card 结构编译通过 | 2-3 周 |
| **M2: Surface Cache** | Card Atlas 渲染 + Page 管理 | Card Atlas 可视化 (显示在屏幕上) | 2-3 周 |
| **M3: SDF GI** | Screen Probe Gather (仅 SDF 追踪) | 屏幕上有 SDF 追踪的间接光照 | 3-4 周 |
| **M4: Radiance Cache** | 世界探针网格 + SH 存储/插值 | 离线区域的稳定 GI | 2-3 周 |
| **M5: 反射** | 分层反射 (SSR→SDF) | 场景中有镜面反射 | 2-3 周 |
| **M6: HW RT** | DXR 后端集成 | 硬件光追 GI 正确显示 | 1-2 周 |
| **M7: 集成优化** | 完整管线 + 降噪 + 性能优化 | 30fps+ @ 1080p on RTX 3080 | 2-4 周 |

**总计**: 14-22 周 (约 3.5-5.5 个月, 单人全职)

### 13.2 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| SDF 追踪精度不足 | GI 漏光/暗斑 | 使用 Enhanced Sphere Tracing, 调优 minHitDistance |
| Card Atlas 容量不足 | 远处表面缺失 | 实现 Card LOD + 自适应 Page 分配 |
| 性能不达标 | 帧率过低 | Half-res 探针, 时序累积, Async Compute |
| SH 低频光照不足 | 细节丢失 | 使用 2nd Order SH (16 系数) 替代 1st Order (9 系数) |
| 与现有管线冲突 | 渲染错误 | 通过 RenderGraph 依赖明确分离 |

---

## 附录 A: 关键参考

- **UE5 Lumen 技术论文**: [Lumen: Real-time Global Illumination in Unreal Engine 5](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf)
- **Falcor 文档**: `FalcorRendering/README.md`, `FalcorRendering/docs/`
- **NVIDIA RTXDI**: `FalcorRendering/Source/Falcor/Rendering/RTXDI/`
- **Jump Flood Algorithm**: Rong & Tan, "Jump Flooding in GPU with Applications to Voronoi Diagram and Distance Transform"
- **Enhanced Sphere Tracing**: "Improved Sphere Tracing" (Keinert et al.)

## 附录 B: 术语对照

| UE5 Lumen 术语 | Falcor 对应 |
|----------------|-------------|
| Surface Cache / Card | `LumenSurfaceCache` + `LumenCard` |
| Mesh Distance Field | `LumenMeshSDF` |
| Global Distance Field | `LumenGlobalSDF` |
| Screen Probe Gather | `LumenScreenProbeGather` |
| Radiance Cache | `LumenRadianceCache` |
| Lumen Reflections | `LumenReflections` |
| Hardware Ray Tracing | DXR `RtStateObject` + `LumenHardwareRayTracing` |
| Nanite | N/A (不在本计划范围) |
| Virtual Shadow Maps | 已有 CSM (可后续升级) |
