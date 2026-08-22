# 实现硬件检测与概览

Type: task  
Status: ready-for-agent  
Resolution: completed
Blocked by: 01, 02, 24
Owner: issue-13
Consumers: 14, 21
Verification: 硬件观测适配器测试及代表性 Windows 设备人工检查，覆盖失败、取消和刷新。
Evidence freshness: 绑定当前构建和设备环境；票内缓存失效事件发生后旧观察失效。

## Goal

按需识别寻找驱动所需的关键硬件，并生成简洁、可恢复的硬件概览。

## Acceptance Criteria

- [ ] 识别 CPU、显卡、主板、网卡和整机 OEM 型号。
- [ ] 检测仅生成概览，不跑分、烤机或安装驱动。
- [ ] 检测逻辑自行实现，不复制第三方工具代码或资源。
- [ ] 结果只在会话内缓存 10 分钟，超时、硬件变化或用户主动请求时刷新。
- [ ] 失败、权限不足或取消时显示“未识别”。
- [ ] 检测失败不隐藏固定驱动获取入口。

## References

`DRV-04` 至 `DRV-09`

## Comments

- 2026-08-12：功能实现提交 `ae470bd6e05de5591061193f8c3ded800dbe8ad2` 经 PR [#16](https://github.com/CoolPlume/azzs/pull/16) 普通合入 `codex/v1-integration`，合入提交 `af149b5e12bdec081c76cd626a12105de37b8e5a`。feature head 的 host guardrails 为 16/16 通过；GitHub Actions run `31590067714` 的 x64 Release 与 ARM64 Release 均通过。真实 Windows 代表性设备人工检查尚未执行，未作为通过证据。
