# Filament 后处理与阴影架构分析与移植指南

## 1. 架构分析

Filament 渲染器的后处理和阴影主要由两个核心管理器负责：`PostProcessManager` 和 `ShadowMapManager`。这些管线深度绑定了 Filament 的 `FrameGraph` 架构，使用 RenderPass 的形式来按顺序提交渲染任务。

### 1.1 阴影管线 (ShadowMapManager)

**主要特性：**
* **级联阴影贴图 (CSM)**：针对方向光（Directional Light），支持最多 4 级级联（`CONFIG_MAX_SHADOW_CASCADES`），并自动计算近/远裁剪面和分割比例。
* **聚光灯与点光源阴影 (Spot / Point Shadow)**：支持多光源阴影图。
* **方差阴影贴图 (VSM / EVSM)**：用于软阴影计算，包含高精度和低精度路径，并配套高斯模糊（`gaussianBlurSeparatedPass`）和 Mipmap 生成（`vsmMipmapPass`）。
* **屏幕空间接触阴影 (SSCS)**：针对细小物体补充屏幕空间阴影（Screen Space Contact Shadows）。

**主要配置项：**
* `cascadeSplits`：级联分割参数。
* `ShadowTechnique`：阴影技术（如 `SHADOW_MAP`, `SCREEN_SPACE` 等）。
* `SoftShadowOptions`：软阴影控制参数。
* `VsmShadowOptions`：VSM相关参数（如 blurWidth 等）。
* 阴影图图集（Texture Atlas）分辨率配置。

**实现流程：**
1. **更新阶段 (`update`)**：遍历光源和渲染物体，计算光源的视锥体，剔除不可见物体，配置阴影图集大小。
2. **渲染阶段 (`render`)**：构建 FrameGraph Pass。对于每个阴影图，渲染深度，如果使用 VSM 则还会调用模糊和 Mipmap Pass。

### 1.2 后处理管线 (PostProcessManager)

**主要特性与顺序：**
Filament 将后处理步骤紧密排列，大致顺序如下：
1. **Structure Pass (深度/法线)**：为后续屏幕空间特效提供基础数据。
2. **SSR (屏幕空间反射)**。
3. **SSAO (屏幕空间环境光遮蔽)**。
4. **DoF (景深)**：基于散景 (Bokeh) 模糊。
5. **Bloom (泛光)**：提取高光部分，生成不同级别的降采样，然后上采样合并。还会包含 Flare (镜头光晕)。
6. **Color Grading & Tone Mapping (色彩校正和色调映射)**：与 Bloom 结合合并，并处理 Vignette (暗角)。
7. **FXAA / TAA**：抗锯齿，TAA 在历史帧间进行 Jitter 并混合。
8. **Upscaling (超分辨率)**：如 FSR (FidelityFX Super Resolution) 或 Bilinear。

**主要配置项：**
* `BloomOptions`：泛光强度、阈值等。
* `ColorGradingConfig`：是否使用亮度输出，是否支持抖动(dithering)，后处理色域。
* `TemporalAntiAliasingOptions`：TAA相关（如反馈系数等）。
* `DepthOfFieldOptions`：景深相关（焦距，光圈等）。
* `AmbientOcclusionOptions`：SSAO配置（半径，强度等）。

---

## 2. 移植到 Falcor 的集成方案建议

Falcor 的架构也是基于渲染管线 (RenderGraph) 和渲染通道 (RenderPass) 的。
如果要将这些流程集成到 Falcor 中，尤其是供 `PBRTOfflineRenderer` 调用，我们有以下几种目录组织选项：

### 选项 A：集成到 `Source/RenderPasses`（强烈推荐）
在 `Source/RenderPasses` 中创建一个新文件夹 `FilamentFX`（包含 `FilamentPostProcess` 和 `FilamentShadowPass`）。
* **优势**：这是 Falcor 的标准做法。所有的 Pass 都可以高度复用。日后不仅 `PBRTOfflineRenderer` 可以用，可以通过 Falcor 的 RenderGraph Editor (Mogwai) 直接拖拽连线。
* **流程**：在 `PBRTOfflineRenderer` 中实例化这些 Pass，然后在 `onFrameRender` 中使用 `mpFilamentPostProcess->execute(...)` 将 `RasterPass` 的结果进行后处理。

### 选项 B：集成到 `Source/Samples/PBRTOfflineRenderer` 作为内部模块
在 `PBRTOfflineRenderer` 下建立 `PostProcessing/` 和 `Shadows/` 目录。
* **优势**：与当前应用强绑定，无需考虑 Falcor 全局 RenderPass 的规范封装，实现起来更随意。
* **劣势**：无法被其他项目复用。

### 选项 C：集成到 `plugins` 目录
* `plugins` 主要用于第三方非渲染器级别的插件（如导入器、特殊的工具）。对于核心的后处理和阴影渲染逻辑，不推荐放在 `plugins`。

**最终结论：**
为了保持 Falcor 架构的优雅性，并在 PBRTOfflineRenderer 中直观预览，推荐**在 `Source/RenderPasses/FilamentFX` 下新建独立的 RenderPass，然后在 `PBRTOfflineRenderer` 内直接调用它们。** 
作为初步集成的第一步，我们可以在 `PBRTOfflineRenderer` 的 UI 中直接先建立这些参数配置，以方便后续连线真实的渲染通道。
