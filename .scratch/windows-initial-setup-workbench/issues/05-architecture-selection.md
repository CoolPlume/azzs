# 实现软件包架构选择规则

Type: task  
Status: ready-for-agent  
Resolution: open
Blocked by: 02, 03
Owner: issue-05
Consumers: 06, 07, 12, 18, 21
Verification: x64、ARM64、检测失败、偏好回退和重启变化的架构决策表无界面测试。
Evidence freshness: 绑定当前提交；每次启动或恢复使用当次架构观察，旧观察不得复用。

## Goal

依据当前 Windows 架构和用户偏好，为软件选择兼容安装包并处理检测失败或架构变化。

## Acceptance Criteria

- [ ] 每次启动和重启恢复时重新检测 Windows 架构。
- [ ] x64 系统选择 x64 兼容包，仅有 ARM64 包的软件被禁用。
- [ ] ARM64 系统优先 ARM64 包，缺少时按当前偏好提示或回退 x64。
- [ ] 默认逐项确认回退；拒绝只跳过当前软件，重试时重新评估。
- [ ] 检测失败只暂停需要架构选择的软件，不阻断设置和架构无关项目。
- [ ] 架构变化不改写旧批次或已安装状态，并留下详细日志。

## References

`ARCH-01` 至 `ARCH-18`

## Comments
