# 事项 01：实现可调整侧栏

Type: task
Status: needs-triage
Resolution: open
Owner: issue-01
Consumers: 06
Verification: 在无界面偏好合同、x64 WinUI UI 和跨重启场景验证 216/248/360 DIP、拖拽、键盘、compact 和偏好读写失败回退。
Evidence freshness: 绑定实现提交、`D:\azzs-codex\worktrees` 中的构建目录、Windows/SDK、DPI/窗口尺寸和实机观察；偏好或 XAML 合同变化后重跑。

## 问题

0.1.1 实测侧边栏仍不能手动拖拉。ADR-0050 已定义唯一偏好所有者、边界和键盘语义，但当前集成基线没有拖拽把手、宽度接线或偏好存储。

## 目标

- 展开态拖拽连续跟手、可中断、可反向；键盘具备等价增减路径。
- `216` 为最小、`248` 为默认、`360` 为最大；首次运行、非法值和读写失败回退默认。
- 关闭/重开恢复宽度；窄窗口 compact 导航不丢页面、焦点或业务事实。
- 不把宽度写入页面、批次、目录、硬件或日志业务状态。

## 依赖与边界

- 必须复用 ADR-0050 的偏好服务所有权，不在 `MainWindow` 建第二份设置。
- 必须保持 Windows 原生导航和无障碍语义，不用坐标点击或页面私有状态。

## 验收

- 偏好合同覆盖边界、非法值、持久化失败和重建。
- Windows x64 实机覆盖拖拽、键盘、窄窗、DPI 和关闭重开。

## Comments

- 基线事实：当前 `codex/v1-integration` 没有宽度实现；ADR-0050 已先行。
