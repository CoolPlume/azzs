# 实现硬件检测与概览

Type: task  
Status: ready-for-agent  
Resolution: open
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
