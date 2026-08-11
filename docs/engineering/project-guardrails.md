# 项目架构与无界面测试门禁

## 唯一命令

在仓库根目录执行：

```sh
cmake --workflow --preset host-guardrails
```

该命令完成 host Debug 配置、构建和当前全部 CTest 门禁，不启动 WinUI 3，不要求管理员权限或网络，也不修改真实系统状态。Windows x64 的现有 `eng/build.ps1` 默认执行无筛选 CTest，因此自动包含同一组门禁；历史参数 `-SkipCoreSmoke` 会跳过整个 CTest 调用，带该参数的构建不构成门禁证据。ARM64 保持编译与链接检查，不在 x64 runner 上执行 ARM64 测试。

## 自动检查

依赖角色、允许边、外部运行库、MSBuild 固定输入和可机械识别的系统副作用模式只在 `cmake/guardrails/ArchitectureRules.cmake` 定义一次。门禁从已注册 CMake 目标的实际链接、创建目录、源码和 include 目录，WinUI MSBuild 项目，以及生产与测试 C++ 依赖取得项目图，并检查：

- domain 不依赖上层，application 只依赖 domain 和通用运行库；
- CMake 目标的创建目录、源码、自有 include 目录和预编译头必须与声明角色一致，外部目标的传递链接与项目内 interface 路径也进入同一依赖图；除登记的 WinUI 生成模块外，未登记、动态或配置时不可解析的生成源失败关闭；
- 平台与基础设施适配器只依赖核心接口或领域类型，不依赖 UI；
- 生产目标不依赖 `tests/support`，测试替身只能由测试目标使用；
- WinUI 项目只接受固定工具链导入、字面 include 路径、四条必要核心链接边、登记源码和唯一生成模块；动态属性、通配符和额外导入失败关闭；
- 未解析或动态 `#include`、未登记 C++ `import`、UI 或装配源码直接调用已登记系统修改 API 都会被拒绝；只有 composition root 可以同时认识具体 UI、核心和适配器；
- 所有 CTest 必须通过 `azzs_register_test` 注册并只使用 `headless`、`contract`、`architecture` 标签；
- CMake 目标、MSBuild 宿主和源码依赖图无环；
- 反向依赖、精确闭环、生产代码链接或编译测试替身、绕过装配入口、原始 CTest 注册、MSBuild 动态源码和 UI 直接系统副作用负例都会被拒绝，并检查诊断包含目标、实际边和允许边。

`core.smoke` 通过 `FixedPlatformInfo` 替换平台观测，标签为 `headless;contract`。它只链接 application 与测试支持目标，证明当前核心 smoke 不需要 WinUI、管理员权限、网络或真实电脑修改。

## Owner 注册场景套件

业务 owner 在自己的 `tests/<suite>/CMakeLists.txt` 创建测试目标后，只使用统一接缝声明依赖并注册普通 CTest：

```cmake
azzs_project_target(
  TARGET owner_suite
  ROLE test
  PRIVATE_LINKS
    azzs::application
    azzs::test_support
)

azzs_register_test(
  NAME owner.scenario
  TARGET owner_suite
  LABELS headless contract
)
```

事项 02、03、15、18、32 仍分别拥有其状态机、目录编辑、恢复、日志和诊断场景的输入与预期结果。本门禁只提供注册和执行接缝，不提供占位测试，也不定义第二份业务语义。

## 必须人工评审

自动图检查不能证明所有语义质量。每次评审仍必须检查：

- 每项业务状态是否只有一个明确的可写所有者；
- 删除检验是否表明模块真正隐藏复杂度，而不是浅层转发；
- 同一业务规则是否在 UI、核心、适配器或多个功能模块中重复；
- 是否引入跨功能共享的全局可变业务状态。
- 是否通过包装函数、COM、动态调用或尚未登记的 API 间接执行系统副作用；标识符扫描不能替代语义评审。

任一硬性缺陷都必须先修复；不能用事项内例外绕过。确需改变规则时，先修改规格与 ADR，再更新唯一规则矩阵。
