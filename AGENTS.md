# FalcorRendering Agent 协作规则

本文件适用于整个 `F:\project\FalcorRendering` 仓库。子目录若新增更具体的 `AGENTS.md`，其规则只覆盖对应子树；未覆盖部分继续遵守本文件。

## 1. 默认工作方式：先解耦，再最大化安全并行

每次开始非平凡任务时，root/integrator 必须先做一次简短的并行性分析：

1. 明确最终目标、输入、输出、Gate 和禁止修改范围。
2. 将工作拆成依赖 DAG，标出可立即执行、互不依赖的节点。
3. 只要存在两个或以上边界清晰、可独立验证的子任务，就应在可用并发槽内最大化使用 subagent 并行执行。
4. root 在 subagent 工作期间继续处理接口、只读分析、集成准备或其他不冲突任务，不空等。
5. 若任务只能串行，root 应简短记录原因，例如共享文件、未冻结接口、单一 GPU、单一构建目录或严格前后依赖。

不要为了数量机械创建 subagent。单行修改、同一函数内的强耦合修复、必须由同一 GPU/构建结果驱动的连续诊断可以由 root 串行完成。

## 2. 子任务解耦要求

每张 subagent 任务卡必须包含：

- 单一、可验证的目标；
- 明确的允许修改文件或目录；
- 明确的禁止修改文件；
- 已冻结的输入/输出接口、资源格式、坐标空间或数据结构；
- 最小验证命令和完成 Gate；
- 需要返回的内容：修改文件、测试结果、接口偏差、残余风险和 artifact 路径。

禁止直接下发“完成全部集成”“修复所有失败”“优化整个系统”之类边界不明的任务。先拆成单接口、单文件/目录或单验证目标，再并行发布。

优先按以下边界拆分：

- Host/C++ 资源与调度；
- Shader/Slang 算法；
- CPU 单测；
- GPU/Mogwai 测试脚本；
- 图像/性能分析；
- 文档与证据整理。

如果两项工作必须修改同一文件，则采用以下任一方式，不得并发直接编辑：

1. 先由 root 冻结共享接口，再拆到不同文件；
2. 分成串行 Wave；
3. 一个 agent 只读分析并返回建议，唯一文件所有者负责落盘；
4. 使用独立 worktree，最后由 root 按明确顺序集成。

## 3. 文件所有权与共享入口

- 同一 Wave 中，一个文件只能有一个写入 owner。
- Subagent 不得修改任务卡以外的文件，不得执行全仓格式化、批量重命名或自行合并共享入口。
- `LumenGI.cpp/.h`、插件/中央 `CMakeLists.txt`、公共 RenderGraph、共享 Slang 数据结构、`task.md` 和权威执行计划默认由 root/integrator 所有，除非任务卡显式转让唯一所有权。
- 发现必须越界修改时，subagent 应暂停该修改并报告，不得先改后说明。
- Subagent 在写入前应重新读取目标文件并检查工作区状态；发现其他 agent 或用户的新修改时不得覆盖、回退或假定其可丢弃。
- 工作区可能已有用户修改和未跟踪文件；禁止 reset、clean、覆盖或顺手整理无关内容。
- 未经用户明确要求，不得自行 stage、commit、push 或创建 PR。

## 4. 并发资源约束

代码、只读分析、CPU 单测和离线图像 diff 在文件与输出目录互不冲突时可并行。

以下资源必须串行并由 root/integrator 统一调度：

- 仓库级 CMake/MSBuild；只允许一个构建进程，使用 `--parallel 1` 或 `/m:1`，避免 C1041/PDB 冲突。
- 单物理 GPU 上的 Mogwai、GPU image test、D3D12/RT Validation、性能和 soak。
- 会写同一 artifact、日志、XML、截图或缓存目录的测试。
- 修改共享 ABI、公共 C++/Slang layout 或 RenderGraph 契约后的最终集成。

并行 subagent 不得各自启动仓库级构建或争抢 GPU。它们可以准备代码、CPU 测试和独立测试资产，由 root 统一构建并依次运行 GPU Gate。
并行测试或分析必须使用各自唯一的 artifact/log 输出路径，禁止覆盖其他任务的证据。

## 5. CodeGraph 与代码定位

- 仓库根存在 `.codegraph/` 时，理解或定位代码必须先使用 `codegraph explore`，再使用 `rg` 或定点读取补充。
- 源码发生实质变化后，在最终调用路径审查前运行 `codegraph sync .`。
- `.codegraph/` 不得提交。
- 不允许仅凭文件名、shader 存在、checkbox 或 debug output 非零推断功能已经进入生产主链；必须追踪调用者、资源生产者、消费者和最终输出。

## 6. Wave 集成流程

每个并行 Wave 按以下顺序执行：

1. root 冻结接口、文件所有权、测试输出目录和 Gate。
2. 启动所有 ready 且互不冲突的 subagent，尽量占满可用并发槽，同时为 root 保留集成能力。
3. root 并行准备共享接口或只读验证，不修改已分配给 subagent 的文件。
4. subagent 返回后，root 先审查越界修改和接口偏差，再按 Host → Shader → Tests/Docs 的依赖顺序集成。
5. root 运行唯一构建，然后串行执行 GPU/validation Gate。
6. 当前 Wave 失败时保留第一个错误、复现命令和 artifact；不启动依赖该失败结果的下游 Wave。
7. 与失败节点无依赖且文件/资源不冲突的任务可继续并行，不因单点失败闲置整个任务池。

## 7. Subagent 返回格式

每个 subagent 完成时必须报告：

```text
目标：
状态：完成 / 部分完成 / 阻塞
修改文件：
未修改的共享文件：
接口或假设：
验证命令与结果：
artifact：
残余风险：
建议的下一依赖节点：
```

没有实际修改时必须明确写出，不得用模糊的“已处理”代替。

## 8. LumenGI 专项约束

- 当前权威计划是 `docs/LumenGI_Production_Chain_Closure_Plan.md`；按 C0–C12 和 Gate 顺序推进。
- C0–C9 是强生产依赖链，默认顺序执行；每个批次内部仍应把 Host、Shader、Tests、分析和证据尽量解耦并行。
- C0–C9 未闭环前，不并行接入 C10 Radiance Cache、C11 Quality Preset 或 C12 发布矩阵。
- 新或修改的 shader 必须经过 Mogwai 实机运行时编译；文件存在或 CMake 成功不算验证。
- 画质正确性和性能是同级 Gate；不得通过降低参考质量或放宽阈值掩盖失败。
