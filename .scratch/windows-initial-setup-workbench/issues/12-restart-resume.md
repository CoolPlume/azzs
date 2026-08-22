# 实现重启屏障与流程恢复

Type: task  
Status: ready-for-agent  
Resolution: completed
Blocked by: 02, 05, 08, 10, 11, 27
Owner: issue-12
Claimed by: issue-12-x64
Consumers: 14, 15, 16, 21
Verification: 无界面恢复状态机与 Windows 登录恢复集成测试，覆盖只读重检、取消、屏障、异常退出和紧急撤回。
Evidence freshness: 绑定当前提交、检查点与存储模式和具体 Windows build；新修改前重新检测当前状态。

## Goal

在驱动、系统优化、软件安装或软件优化要求重启 Windows 时安全暂停，并在下次登录后由用户决定是否继续；需要重启资源管理器的操作使用更轻量的独立状态。

## Acceptance Criteria

- [ ] 重启屏障分别保存流程记录、可能存在的系统优化执行记录、安装批次或优化批次，以及已执行但仍等待重启的操作状态；软件安装只有冻结的受控安装档案明确要求重启时才进入屏障，未知后置行为保持“安装结果待确认”。
- [ ] 工作台不自动重启 Windows，提供“立即重启”和“稍后重启”并提示保存其他工作。
- [ ] 下次登录后工作台一次性打开并载入暂停内容。
- [ ] 恢复后可以自动重新检测架构、必要设备状态、目标软件版本与状态及有效紧急撤回信息；已启动安装器还按冻结安装档案验证完成边界、后置行为和重启后结果，仍无法判断时保持“安装结果待确认”。来源手动交接及其未完成列表项目仍等待用户主动选择“重新检测”；启动任何新修改前必须等待用户明确继续。
- [ ] 恢复后先只读验证已执行、待重启或安装结果待确认步骤，再允许后续修改；紧急撤回不阻止既有结果验证，但阻止受影响的新修改。
- [ ] 普通关闭后重新打开与重启恢复都不会静默继续管理员操作。
- [ ] 来源手动交接跨关闭和 Windows 重启保留；它本身不建立重启屏障或触发登录后自动打开，用户下次主动打开任一发行形态时恢复原始地址、最近状态和“重新检测”，已识别成功时保留外部安装标记，暂时跳过且尚未成功时继续列入未完成列表并保留原失败原因，且不自动执行检测。
- [ ] 推荐流程在外部安装已识别但用户尚未点击“继续”时关闭或重启，恢复后仍停留在该续接点并显示“继续”，不自动执行剩余步骤。
- [ ] 取消前展示未执行操作、已生效设置、已完成优化和待重启验证状态，并要求确认。
- [ ] 取消只放弃未开始操作；已经完成的设置或优化不恢复，待重启验证状态继续保留。
- [ ] 流程记录、系统优化执行记录、安装批次和优化批次分别保留只读历史，尚未进入相应阶段时不虚构记录或批次。
- [ ] 只有未完成软件按当前目录和设备状态生成新批次，未执行设置作为新操作重新选择。
- [ ] 优化重试按当前目录、目标软件和紧急撤回状态创建新批次；下架或紧急撤回方案不能重试。
- [ ] “等待资源管理器重启”不建立 Windows 重启屏障，也不阻止没有依赖关系的后续步骤；用户可稍后重启资源管理器、重启 Windows 或重新打开工作台后完成检测。
- [ ] 只有用户选择“立即重启资源管理器”时才执行该操作，并提前说明任务栏和已打开的资源管理器窗口会短暂关闭；选择“稍后处理”会持久保留状态和入口。

## References

`FLOW-04` 至 `FLOW-12`、`RUN-01` 至 `RUN-04`、`RUN-06` 至 `RUN-07`、`SW-46` 至 `SW-49`、`RST-01` 至 `RST-14`、`HIST-07`、`SET-20`、`SET-33` 至 `SET-35`、`OPT-33`、`OPT-55`

## Answer

- 功能提交为 `7fa985c4ef23e615468828abc64742b1cf2ef682`，状态闭环修复为 `2b8d61937b9157e6d9f7bc3eb877cec76727c319`。PR [#43](https://github.com/CoolPlume/azzs/pull/43) 已以普通 merge 合入 `codex/v1-integration`，merge SHA 为 `7ffcfe07a43580dcda5dcf9e7fefcf4a18e78a75`。
- 已实现持久化重启检查点、Windows HKCU `RunOnce` 登录启动、登录后的只读恢复、显式继续或取消决定，以及安装批次和软件优化批次在“等待重启”事实持久化后建立统一屏障。安装页在继续或停止恢复批次前先写入相应的显式决定；只有全部参与者的只读恢复成功，组合根才进入 `awaiting_user_decision`。失败、无活动批次或租约失败保留在只读验证状态，不会错误放行；明确决定清除检查点后，后续等待重启可重新建立一次性登录启动。
- 本次 x64 实测：`restart-resume.contract`、`installation-batch-runner.contract` 与 `software-optimization-batch-runner.contract` 为 3/3 通过；`eng/build.ps1 -Architecture x64 -SkipCoreSmoke` 通过，包含 C++ 核心、Windows 适配器和 WinUI 宿主，0 error。GitHub Actions run `31800156597` 的 x64 Release 在最终候选 `2b8d619` 上成功。
- x64 主机边界：本次按变更风险只重跑上述 focused 合同和完整 x64 build，未重跑全量 CTest 或 host guardrails；此前候选的全量 x64 CTest 中 `execution-log.contract` 与 `windows-device-data.contract` 仅因当前主机缺少 ACL 根目录和目录符号链接创建前置失败，不能计为通过，也未因本事项改动。
- 未执行真实 Windows 重启、登录后实机交互、DPI/辅助功能、安装生命周期、签名、WiX 或发布验收。ARM64 未在本机执行，且未作为 PR 合入门禁；GitHub ARM64 job 的状态不外推为 ARM64 本机验证，ARM64 硬件验证延期。

## Comments

- 2026-08-17：PR [#95](https://github.com/CoolPlume/azzs/pull/95) 的 feature `9de76facc30a48bd0682d3beafb06eafe5944566` 已由普通 merge `b65dc23` 合入 `codex/v1-integration`。`windows_restart_resume_registration.cpp` 的 `GetModuleFileNameW` 读取从固定 `MAX_PATH` 改为 512 至 32768 的增长缓冲，接受合法的最后字符槽位并在上限处 fail-closed；RunOnce 注册、命令行 token、注册失败回滚和清理语义未改动。证据仅为源码、Git diff、PR 范围和空白静态核对，未运行构建、重启恢复合同、EXE、调试器或真实 Windows 长路径；事项 12 的 `Resolution: completed` 依据既有证据保持不变。
