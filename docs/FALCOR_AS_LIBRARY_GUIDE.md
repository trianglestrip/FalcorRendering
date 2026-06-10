# 将 FalcorRendering 改造成可复用库工程的完整方法

> 目标: 把当前 FalcorRendering 从“仓库内构建和运行示例程序”的形态，整理成一个可被外部 C++ 项目稳定链接、部署和运行的 Falcor SDK。  
> 适用场景: 新开独立 Nanite 项目，但继续复用 Falcor 的 Device、Buffer、Texture、RenderPass、Scene、RenderGraph、Shader 编译、插件和资源加载能力。

---

## 1. 当前状态判断

当前仓库里的 Falcor 本身已经是动态库目标:

```cmake
add_library(Falcor SHARED)
```

Release 构建后已经能看到:

```text
build/windows-vs2022/bin/Release/Falcor.dll
build/windows-vs2022/Source/Falcor/Release/Falcor.lib
build/windows-vs2022/bin/Release/shaders/
build/windows-vs2022/bin/Release/data/
build/windows-vs2022/bin/Release/plugins/
build/windows-vs2022/bin/Release/settings.json
```

但这还不是完整 SDK。原因是 Falcor 运行不仅依赖 `Falcor.dll` 和头文件，还依赖:

- 第三方 DLL: Slang、GFX、DXC、Assimp、FreeImage、OpenEXR、OpenVDB、NRD、DLSS、CUDA runtime 等。
- Shader 文件: `shaders/` 目录。
- 插件 DLL 和 `plugins/plugins.json`。
- `data/`、`scripts/`、`settings.json`。
- 编译期依赖: `external/`、Packman 下载的 include/lib、CMake feature define。
- Windows D3D12 Agility SDK 目录: `D3D12/`。

因此，“只拷贝头文件 + Falcor.dll”不是可靠方案。正确做法是做一个完整 SDK 包。

---

## 2. 推荐目标结构

建议最终导出目录为:

```text
FalcorSDK/
  include/
    Falcor/
      Falcor.h
      Core/
      Scene/
      RenderGraph/
      Rendering/
      Utils/
      ...
    external/
      ...必要第三方头文件...
  lib/
    Falcor.lib
    ...必要 import/static libs...
  bin/
    Falcor.dll
    dxcompiler.dll
    dxil.dll
    gfx.dll
    slang.dll
    assimp-*.dll
    FreeImage.dll
    OpenEXR*.dll
    openvdb.dll
    NRD.dll
    nvngx_dlss.dll
    ...
    D3D12/
    shaders/
    data/
    scripts/
    plugins/
      plugins.json
      GBuffer.dll
      ToneMapper.dll
      ...
    settings.json
    setpath.bat
    setpath.ps1
  cmake/
    FalcorConfig.cmake
    FalcorTargets.cmake
    FalcorRuntime.cmake
```

外部项目只需要:

```cmake
find_package(Falcor CONFIG REQUIRED)
target_link_libraries(MyNaniteApp PRIVATE Falcor::Falcor)
```

运行时只需要让 exe 能找到 `FalcorSDK/bin` 下的 DLL 和资源目录。

---

## 3. 分阶段改造路线

## Phase 1: 手工 SDK 包验证

先不改太多 CMake，手动复制构建产物验证外部项目可用。

从当前构建目录复制:

```text
build/windows-vs2022/bin/Release/Falcor.dll
build/windows-vs2022/Source/Falcor/Release/Falcor.lib
build/windows-vs2022/bin/Release/*.dll
build/windows-vs2022/bin/Release/D3D12/
build/windows-vs2022/bin/Release/shaders/
build/windows-vs2022/bin/Release/data/
build/windows-vs2022/bin/Release/scripts/
build/windows-vs2022/bin/Release/plugins/
build/windows-vs2022/bin/Release/settings.json
build/windows-vs2022/bin/Release/setpath.bat
build/windows-vs2022/bin/Release/setpath.ps1
```

复制头文件:

```text
Source/Falcor/**/*.h
Source/Falcor/**/*.slangh
external/**/include
external/imgui/*.h
external/imgui_addons/**/*.h
```

手工阶段的目的不是长期维护，而是确认:

- 外部 exe 能链接 `Falcor.lib`。
- 运行时能加载 `Falcor.dll`。
- Shader 能从 `shaders/` 找到。
- RenderPass 插件能从 `plugins/` 加载。
- Scene/texture/data 能从 `data/` 和 `settings.json` 找到。

---

## Phase 2: 新增 CMake SDK 导出目标

建议在根 `CMakeLists.txt` 或 `cmake/falcor_sdk.cmake` 中新增:

```cmake
option(FALCOR_ENABLE_SDK_INSTALL "Install Falcor as a reusable SDK" ON)
set(FALCOR_SDK_INSTALL_DIR "${CMAKE_BINARY_DIR}/FalcorSDK" CACHE PATH "Falcor SDK output directory")
```

新增安装目标:

```cmake
if(FALCOR_ENABLE_SDK_INSTALL)
    install(TARGETS Falcor
        EXPORT FalcorTargets
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION bin
        ARCHIVE DESTINATION lib
    )

    install(EXPORT FalcorTargets
        NAMESPACE Falcor::
        FILE FalcorTargets.cmake
        DESTINATION cmake
    )

    install(DIRECTORY Source/Falcor/
        DESTINATION include/Falcor
        FILES_MATCHING
            PATTERN "*.h"
            PATTERN "*.slangh"
    )

    install(DIRECTORY ${FALCOR_OUTPUT_DIRECTORY}/shaders/
        DESTINATION bin/shaders
    )

    install(DIRECTORY ${FALCOR_OUTPUT_DIRECTORY}/data/
        DESTINATION bin/data
    )

    install(DIRECTORY ${FALCOR_OUTPUT_DIRECTORY}/scripts/
        DESTINATION bin/scripts
    )

    install(DIRECTORY ${FALCOR_OUTPUT_DIRECTORY}/plugins/
        DESTINATION bin/plugins
    )

    install(FILES
        ${FALCOR_OUTPUT_DIRECTORY}/settings.json
        DESTINATION bin
    )
endif()
```

Windows 上还需要复制所有运行时 DLL。最稳的方式是直接复制 `bin/Release` 中除 exe/pdb/log 外的运行时文件:

```cmake
install(DIRECTORY ${FALCOR_OUTPUT_DIRECTORY}/
    DESTINATION bin
    FILES_MATCHING
        PATTERN "*.dll"
        PATTERN "D3D12/*"
        PATTERN "settings.json"
        PATTERN "setpath.bat"
        PATTERN "setpath.ps1"
)
```

注意: `install(DIRECTORY ...)` 的过滤规则容易遗漏子目录，实际实现时要用一次安装验证脚本检查 SDK 内容是否完整。

---

## Phase 3: 生成 `FalcorConfig.cmake`

新增模板文件:

```text
cmake/FalcorConfig.cmake.in
```

内容示例:

```cmake
@PACKAGE_INIT@

include("${CMAKE_CURRENT_LIST_DIR}/FalcorTargets.cmake")

set(FALCOR_SDK_ROOT "${PACKAGE_PREFIX_DIR}")
set(FALCOR_RUNTIME_DIR "${PACKAGE_PREFIX_DIR}/bin")
set(FALCOR_SHADER_DIR "${PACKAGE_PREFIX_DIR}/bin/shaders")
set(FALCOR_PLUGIN_DIR "${PACKAGE_PREFIX_DIR}/bin/plugins")
set(FALCOR_DATA_DIR "${PACKAGE_PREFIX_DIR}/bin/data")

if(WIN32)
    set(FALCOR_RUNTIME_PATH "${FALCOR_RUNTIME_DIR}")
endif()
```

根 CMake 增加:

```cmake
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/FalcorConfig.cmake.in
    ${CMAKE_BINARY_DIR}/FalcorConfig.cmake
    INSTALL_DESTINATION cmake
)

install(FILES
    ${CMAKE_BINARY_DIR}/FalcorConfig.cmake
    DESTINATION cmake
)
```

---

## Phase 4: 处理外部 include 依赖

Falcor 的 public link 里包含:

```cmake
fmt
pybind11::embed
Vulkan::Headers
slang
slang-gfx
imgui
imgui_addons
nanovdb
external_includes
rtxdi
```

这意味着导出 SDK 时有两种策略。

### 策略 A: SDK 内复制所有外部头和库

优点:

- 外部项目最简单。
- 不要求外部项目知道 Falcor 的 Packman 结构。

缺点:

- 包较大。
- 需要维护复制清单。

适合 Nanite 独立项目。

### 策略 B: 外部项目仍把 Falcor 仓库作为 submodule

外部项目:

```cmake
add_subdirectory(external/FalcorRendering)
target_link_libraries(MyNaniteApp PRIVATE Falcor)
```

优点:

- 最少 SDK 打包工作。
- Falcor 的依赖仍由原仓库 CMake 管。

缺点:

- 外部项目会被 Falcor 的全量 CMake 结构影响。
- 不是真正意义上的二进制 SDK。

适合早期开发。

### 推荐

先用策略 B 快速验证 Nanite 独立项目结构，再做策略 A 的完整 SDK。

---

## Phase 5: 外部 Nanite 项目模板

建议新项目结构:

```text
NaniteProject/
  CMakeLists.txt
  src/
    main.cpp
    NaniteViewerApp.cpp
    NaniteViewerApp.h
  assets/
    ...
  external/
    FalcorSDK/
```

`CMakeLists.txt` 示例:

```cmake
cmake_minimum_required(VERSION 3.25)
project(NaniteProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/external/FalcorSDK")
find_package(Falcor CONFIG REQUIRED)

add_executable(NaniteProject
    src/main.cpp
    src/NaniteViewerApp.cpp
    src/NaniteViewerApp.h
)

target_link_libraries(NaniteProject PRIVATE Falcor::Falcor)

if(WIN32)
    add_custom_command(TARGET NaniteProject POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FALCOR_RUNTIME_DIR}"
            "$<TARGET_FILE_DIR:NaniteProject>"
    )
endif()
```

最小 `main.cpp`:

```cpp
#include <Falcor/Falcor.h>
#include <Falcor/Core/SampleApp.h>

using namespace Falcor;

class NaniteApp : public SampleApp
{
public:
    using SampleApp::SampleApp;

    void onLoad(RenderContext* pRenderContext) override
    {
    }

    void onFrameRender(RenderContext* pRenderContext, const ref<Fbo>& pTargetFbo) override
    {
        pRenderContext->clearFbo(pTargetFbo.get(), float4(0.02f, 0.02f, 0.025f, 1.f), 1.f, 0, FboAttachmentType::All);
    }
};

int main(int argc, char** argv)
{
    NaniteApp::Config config;
    config.windowDesc.title = "Nanite External Viewer";
    config.windowDesc.resizableWindow = true;
    NaniteApp app(config);
    return app.run();
}
```

---

## Phase 6: 运行时资源路径

Falcor 运行时需要正确找到:

```text
shaders/
plugins/
data/
settings.json
```

外部项目必须满足以下任一方式:

1. 把 SDK 的 `bin/` 内容复制到外部 exe 同目录。
2. 启动前执行 `setpath.bat` 或设置环境变量。
3. 在程序初始化时显式添加 shader/data/plugin 搜索路径。

推荐第一种，最少出错:

```text
NaniteProject/build/Release/
  NaniteProject.exe
  Falcor.dll
  dxcompiler.dll
  gfx.dll
  ...
  shaders/
  plugins/
  data/
  settings.json
```

---

## Phase 7: 插件处理

Falcor RenderPass 目前通过插件机制组织。完整 SDK 需要包含:

```text
bin/plugins/*.dll
bin/plugins/plugins.json
```

如果外部 Nanite 项目新增自己的 RenderPass，有两种方式:

### 方式 A: 外部项目自己的插件目录

```text
NaniteProject.exe
plugins/
  NaniteRaster.dll
  plugins.json
```

优点: 模块清晰。

### 方式 B: 直接把 RenderPass 编译进 exe

适合早期验证，不适合长期插件化。

建议 NaniteRaster 长期做插件:

```text
Source/RenderPasses/NaniteRaster
```

外部项目成熟后再迁移到:

```text
NaniteProject/plugins/NaniteRaster
```

---

## Phase 8: 编译宏和 ABI 注意点

外部项目必须和 Falcor SDK 使用相同 ABI:

- 同一编译器版本: 当前 Windows 是 VS2022。
- 同一架构: x64。
- 同一配置: Debug 对 Debug，Release 对 Release。
- 同一运行库策略。
- 不要在外部项目里定义 `FALCOR_DLL`。
- 外部项目需要使用 Falcor public compile definitions:
  - `NOMINMAX`
  - `UNICODE`
  - `_USE_MATH_DEFINES`
  - `FALCOR_HAS_D3D12`
  - `FALCOR_HAS_VULKAN`
  - `FALCOR_HAS_CUDA`
  - `FALCOR_HAS_D3D12_AGILITY_SDK`
  - `IMGUI_USER_CONFIG="Utils/UI/ImGuiConfig.h"`

这些最好通过 `Falcor::Falcor` imported target 自动传播，而不是手工写在外部项目里。

---

## Phase 9: Debug/Release 双配置

SDK 最好输出:

```text
FalcorSDK/
  Debug/
    include/
    lib/
    bin/
    cmake/
  Release/
    include/
    lib/
    bin/
    cmake/
```

或者:

```text
FalcorSDK/
  include/
  lib/Debug/
  lib/Release/
  bin/Debug/
  bin/Release/
  cmake/
```

Visual Studio 多配置生成器下，推荐第二种。

外部项目用 generator expression:

```cmake
IMPORTED_LOCATION_DEBUG
IMPORTED_LOCATION_RELEASE
IMPORTED_IMPLIB_DEBUG
IMPORTED_IMPLIB_RELEASE
```

---

## Phase 10: 推荐的 SDK 构建脚本

新增脚本:

```text
scripts/export_falcor_sdk.ps1
```

职责:

1. 调用 CMake configure。
2. 构建 `Falcor`、插件、必要 tools。
3. 调用 `cmake --install`。
4. 检查 SDK 目录完整性。
5. 用一个最小外部项目做链接和运行 smoke test。

示例:

```powershell
param(
    [string]$Preset = "windows-vs2022",
    [string]$Config = "Release",
    [string]$SdkDir = "build/FalcorSDK"
)

tools/.packman/cmake/bin/cmake.exe --preset $Preset
tools/.packman/cmake/bin/cmake.exe --build build/$Preset --config $Config --target Falcor
tools/.packman/cmake/bin/cmake.exe --build build/$Preset --config $Config --target Mogwai
tools/.packman/cmake/bin/cmake.exe --install build/$Preset --config $Config --prefix $SdkDir

if (!(Test-Path "$SdkDir/bin/Falcor.dll")) { throw "Missing Falcor.dll" }
if (!(Test-Path "$SdkDir/lib/Falcor.lib")) { throw "Missing Falcor.lib" }
if (!(Test-Path "$SdkDir/bin/shaders")) { throw "Missing shaders" }
if (!(Test-Path "$SdkDir/bin/plugins/plugins.json")) { throw "Missing plugins.json" }
if (!(Test-Path "$SdkDir/cmake/FalcorConfig.cmake")) { throw "Missing FalcorConfig.cmake" }
```

---

## Phase 11: SDK 完整性检查清单

导出的 SDK 必须通过以下检查:

- [ ] `include/Falcor/Falcor.h` 存在。
- [ ] `include/Falcor/Core/API/Device.h` 存在。
- [ ] `include/Falcor/Scene/Scene.h` 存在。
- [ ] `lib/Falcor.lib` 存在。
- [ ] `bin/Falcor.dll` 存在。
- [ ] `bin/dxcompiler.dll` 存在。
- [ ] `bin/dxil.dll` 存在。
- [ ] `bin/gfx.dll` 存在。
- [ ] `bin/shaders/` 存在且包含 Falcor shader。
- [ ] `bin/plugins/plugins.json` 存在。
- [ ] `bin/data/` 存在。
- [ ] `bin/settings.json` 存在。
- [ ] 外部项目能 `#include <Falcor/Falcor.h>`。
- [ ] 外部项目能链接 `Falcor::Falcor`。
- [ ] 外部项目 exe 启动不报 DLL 缺失。
- [ ] 外部项目能创建 `Device`。
- [ ] 外部项目能编译一个简单 Slang shader。
- [ ] 外部项目能加载一个 RenderPass 插件。

---

## Phase 12: 对 Nanite 的推荐组织

Nanite 不建议一开始就完全绑死 Falcor。推荐拆成两层:

```text
NaniteCore/
  NaniteAsset.h
  NaniteAsset.cpp
  NaniteBuild.h
  NaniteBuild.cpp
  NaniteCluster.h
  NanitePage.h
  NaniteHierarchy.h

NaniteFalcor/
  NaniteGpuResources.h
  NaniteGpuResources.cpp
  NaniteRasterPass.h
  NaniteRasterPass.cpp
  NaniteShared.slangh
  NaniteCull.cs.slang
  NaniteRasterize.cs.slang
  NaniteResolve.cs.slang
```

职责:

- `NaniteCore`: 不依赖 Falcor，负责资产格式、构建、Cluster、LOD、page、debug 数据。
- `NaniteFalcor`: 依赖 Falcor，负责 GPU Buffer、RenderPass、RenderGraph、Shader、Scene 集成。

这样做的好处:

- Builder 可以独立运行。
- Loader 可以独立测试。
- Falcor SDK 不成熟时，NaniteCore 不受影响。
- 后续也可以把 NaniteCore 迁移到其他渲染框架。

---

## 13. 最稳落地顺序

建议按下面顺序推进:

1. 保持当前仓库内开发 Nanite 工具和 Viewer。
2. 新增 `FalcorSDK` 导出文档和脚本。
3. 新增 `cmake/FalcorConfig.cmake.in`。
4. 新增 `install(TARGETS Falcor EXPORT FalcorTargets ...)`。
5. 新增 headers、runtime、shader、plugin、data 安装规则。
6. 生成 `build/FalcorSDK`。
7. 新建最小外部项目 `ExternalFalcorSmokeTest`。
8. 验证外部项目链接 Falcor 并清屏运行。
9. 把 `NaniteCore` 从 `Source/Tools/Nanite/Common` 抽成独立库。
10. 让外部 Nanite 项目链接 `Falcor::Falcor` 和 `NaniteCore`。
11. 再迁移 `NaniteViewer` / `NaniteRaster` 到外部项目。

---

## 14. 结论

FalcorRendering 可以改造成库工程，但应按“SDK 包”处理，而不是简单复制 DLL。

最合理的长期形态是:

```text
FalcorRendering -> 生成 FalcorSDK
NaniteCore      -> 独立 Nanite 资产和构建库
NaniteProject   -> 外部项目，链接 FalcorSDK + NaniteCore
```

短期为了开发效率，Nanite 仍放在当前仓库内推进；等 `FalcorSDK` 导出和外部 smoke test 稳定后，再迁移到独立项目。

