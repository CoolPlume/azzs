# 加固启动装配生命周期

Type: task  
Status: ready-for-agent  
Resolution: open
Blocked by: 01, 31
Owner: issue-36
Consumers: 21
Verification: 对精确源码提交运行启动装配合同、无界面核心合同与 x64 Release 定向构建；审查每条启动失败路径的服务关闭、类型化结果传播和预检时序。
Evidence freshness: 绑定当前源码提交、启动装配接口、失败呈现合同和 Windows 工具链；装配入口、失败结果类型、服务生命周期或启动合同变化后重跑。

## Goal

把启动装配失败收束为可处理、可诊断边界内的类型化瞬时结果，确保已经创建的服务在失败离开前完成有序关闭，并让失败结果的安全状态可被唯一调用方观察。该切片只处理启动装配生命周期，不承诺进程内重试、跨启动 bootstrap 日志或失败窗口自身失败后的备用呈现。

## Ownership Boundary

本事项只拥有 `src/composition/windows/composition_root.cpp`、`src/composition/windows/composition_root.hpp`、`src/adapters/ui/winui/App.xaml.cpp` 及其启动装配合同测试中与生命周期收束、异常边界、结果传播和预检时序直接相关的实现。事项 21 汇总本事项针对发行候选提交的门禁结果；本事项不拥有八制品打包、安装器、目录资源或外部 Release。

## Acceptance Criteria

- [ ] 核心诊断不可读、服务构造失败、主窗口导航失败、主窗口激活失败和健康确认失败等路径，在已经创建服务后都执行一次幂等且有序的 `shutdown()`，再离开装配入口；不把服务析构当作业务关闭协议。
- [ ] `App::OnLaunched` 与装配入口对 WinRT 错误及标准 C++ 异常建立总边界，把未预期异常转换为受限的启动装配失败结果；异常不得逃出启动入口并绕过服务关闭。
- [ ] `StartupAssemblyResult` 的 `StartupAssemblyStatus` 与受限设备环境失败结果由唯一调用方保留或安全投影，不能在返回后销毁而使失败窗口只能显示模糊文本；不得把设备身份、路径或原始敏感诊断带入失败窗口。
- [ ] 预检只在主窗口已激活且达到“已正常进入工作台”健康边界后启动；任何导航、激活、诊断或装配失败结果都明确为 `not_attempted`，合同测试不再构造“失败但预检已启动”的不可能状态。
- [ ] 启动失败路径不提交初始化业务状态，不启动新的后台任务；成功路径的既有关闭、预检和主窗口状态语义保持不变。

## References

`CONTEXT.md` 中的“已正常进入工作台”“启动装配失败”“启动失败呈现”“启动诊断可用性”、事项 21、事项 01、事项 31、`docs/engineering/architecture-and-code-quality.md`。

## Comments

- 2026-08-17：切片源自启动边界后置源码审查的四项发现：诊断不可读分支未调用服务关闭、WinUI 启动入口缺少标准异常总边界、类型化启动结果未被调用方保留，以及启动合同构造了导航失败但预检已启动的不可能状态。当前仅建立执行合同，未修改源码或声称验证通过。
- 2026-08-17：只读静态核对确认，源码实现已由 feature `2ec3bd748667ad7a9bf8e22340f156c8cb22f113` 经 integration merge `d7d15f500c71a6b87710202dba93647ab74717ab` 落地到当时 integration 基线 `6896ff364f6c7a3a63949b85cbc68cff281cba5d`；本次只完成静态源码检查，未运行启动装配合同、无界面核心合同、x64 Release、EXE 或真实 Windows，因此 `Resolution: open` 不变。
- 2026-08-17：PR #83 的实现已由 feature head `d339d159eade6fdd084785e0f941b05fbd0408ff` 普通合入 `codex/v1-integration`，merge SHA 为 `ae1ffaaa425a2e8430922c2a46427fc174260fbf`。修复范围为 `src/application/include/azzs/application/emergency_withdrawal_service.hpp` 与 `src/application/src/emergency_withdrawal_service.cpp` 的执行期异常类型化失败、`src/composition/windows/composition_root.cpp` detached 线程最后 `catch (...)`，以及 `tests/emergency-withdrawal-contract/main.cpp` 的服务层异常边界文案与异常注入合同源码；不声称合同或运行时执行通过。已完成的证据只有源码、文本与 Git 静态核对（包括上述 diff 范围和空白检查）；未运行启动装配合同、无界面合同、构建、x64 Release、EXE、调试器、真实 Windows 或 CI。因此 `Resolution: open` 保持不变；真实 x64 候选仍需观察主窗口激活后的预检异常是否被收束、进程是否存活、类型化失败日志/快照和退出码。
- 2026-08-17：PR [#88](https://github.com/CoolPlume/azzs/pull/88) 的 feature `2d234ae9711b33aceaf92b2b45c0179aedd5d524` 已普通合入 `codex/v1-integration`，merge SHA 为 `337e7da9a198fc38fa5399cf78d4d476b6a0678a`。`src/adapters/ui/winui/App.xaml.cpp` 现在捕获 `App::App()` 的 `InitializeComponent()` 异常，并在启动装配或失败窗口呈现异常时记录受限的意外装配失败、通过无 XAML 依赖的 `MessageBoxW` 给出最后一级可见错误；`composition_root.cpp` 的 `startup_failure()` 在失败窗口自身抛错时保留类型化 `StartupAssemblyStatus`，调用方随后走平台兜底；同文件同时修正 `GetModuleFileNameW` 在完整路径恰好占用缓冲区最后字符槽位时的截断误判。证据仅为源代码、Git、暂存范围和空白静态核对；未运行启动装配合同、无界面合同、构建、x64 Release、EXE、调试器、真实 Windows、UI 自动化或 CI。设备环境失败结果的安全投影、真实进程存活/退出码和窗口行为仍未验证，`Resolution: open` 保持不变。
- 2026-08-17：静态源码与 Git 核对确认，feature `bcb2550f260c94f7e292290020e37aaf3ae3a299` 在 `OfflinePackageCache` 中加入 `shutdown_` 拒绝门禁和类型化 `rejected_after_shutdown`，`application_settings` 将其映射到既有 `ApplicationSettingsActionCode::rejected`，无界面合同源码表达关闭后零存储访问、零删除的断言。该修复已由 PR #85 普通合入 `codex/v1-integration`，merge SHA 为 `d7589f2eb057752d2cf63e397e739cbcbf4f03e6`。本次只做源码、文本和 Git 静态检查，未运行构建、CTest、无界面合同、EXE、调试器、真实 WinUI 时序、x64 Release、CI 或 UI 自动化；仍不能证明实际关闭后对话框恢复时不会删除缓存，`Resolution: open` 继续保持。
