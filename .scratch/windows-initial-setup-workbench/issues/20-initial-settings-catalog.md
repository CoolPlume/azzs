# 补充首版系统设置目录内容

Type: task  
Status: completed  
Resolution: completed
Blocked by: 04, 10
Owner: issue-20
Consumers: 21, 22
Verification: 两项设置及总体方案的目录校验、测试适配器执行、检测、原值恢复与自动化检查。
Evidence freshness: 绑定当前提交、设置目录修订和声明的 Windows 范围；网页变化不替代已审核冻结规则。

## Goal

把推荐总体优化与两项已确认的 Windows 优化整理为首版正式精选设置目录内容；维护者以后补充的其他设置作为后续目录内容扩展，不阻塞这两项首版必达能力。

## Engineering Work Required

- 工程实现必须根据已经冻结的来源与锚点，完成两项 Windows 优化的精确目标值、当前状态检测、结果验证、原状态保留和完整恢复规则。
- 上述工作属于开发者的受控实现与审核，不再等待维护者提供注册表技术细节，也不得在运行时解析网页或直接导入网页中的 `.reg` 文件。
- 维护者以后可以继续提供其他系统优化清单；新增项目需要分别补齐效果、适用条件、风险、依赖、检测、恢复和重启语义。

## Confirmed Content

- 首版提供一个“推荐总体优化”，它只引用同一目录中的设置项并允许用户在执行前逐项调整，不复制设置的状态、历史或撤销规则。
- “切换 Windows 11 经典右键菜单”是系统优化项，方法来源为 [管理员右键命令 reg](https://www.zhihu.com/question/480356710)；提供直接应用和恢复 Windows 11 默认菜单，项目已知适用于 Windows 11 各版本至 25H2（含）。
- 经典右键菜单的受控实现锚点为 `HKCU\Software\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\InprocServer32`；目录必须明确冻结经审核的目标、检测和恢复操作。
- “切换 Windows 10 风格资源管理器”是系统优化项，方法来源为 [笔记本 reg 文件方法](https://zhuanlan.zhihu.com/p/690092810)；提供直接应用和恢复 Windows 11 默认资源管理器，项目已知适用于 Windows 11 各版本至 25H2（含）。
- Windows 10 风格资源管理器的受控实现锚点包括来源文章涉及的 CLSID `{2aa9162e-c906-4dd9-ad0b-3d24a8eef5a0}`、`{6480100b-5a83-4d1e-9f69-8ae5a88e9a33}` 及 `ITBar7Layout` 操作；目录必须逐项冻结并审核，不在事项或规格中复制网页的大段二进制值。
- 上述两项在推荐总体优化和单项列表中默认不选；执行任一方向前保留原状态和恢复数据，用户不需要导入 `.reg` 文件或运行命令。
- 上述两项需要时提供“立即重启资源管理器”和“稍后处理”；前者说明任务栏与资源管理器窗口会短暂关闭，后者进入“等待资源管理器重启”。
- 超出已知适用范围时，标准视图显示“可能不适用”并禁用普通执行；高级视图只在恢复方案可用时允许用户确认风险后“仍然尝试”。
- 来源文章仅作为可查看的依据，执行内容冻结在经过维护的目录版本中；网页变化不会自动改变或触发任何系统优化。
- 工作台运行时不解析网页或导入网页提供的 `.reg` 文件；上述锚点的具体键值、检测、验证和恢复规则由开发者审核并冻结，未形成受控实现前不得发布首版。

## Acceptance Criteria

- [x] 每项系统设置和推荐总体优化拥有唯一稳定标识，总体优化只引用单项设置且不复制执行定义。
- [x] 每项设置具备依赖、目标状态、验证方式和重启信息。
- [x] 可逆项具备明确原值记录与撤销规则。
- [x] 设置不按当前用户、软件账号或整台电脑分类或排除，目录以通俗文案准确说明每项操作的实际影响。
- [x] 目录不接受用户提供的任意脚本、命令或注册表内容；来自文章的注册表方法被整理为固定、可检测、可恢复的受控操作。
- [x] 两项已确认的 Windows 优化都具备默认不选、由工作台直接应用、恢复默认、原状态保留、版本范围、超范围风险确认及资源管理器重启语义；只提供命令、`.reg` 文件或手动说明不算完成。
- [x] Windows 自身的右键菜单与资源管理器样式归入系统优化；第三方软件提供的资源管理器右键扩展不在本目录重复收录。
- [x] 所有内容通过目录模式、身份和依赖规则校验。
- [x] 推荐总体优化和两项已确认的 Windows 优化均具备有效目录数据、受控执行器、检测器和自动化检查，可用于首版端到端流程；尚未进行真实 Windows 设备验证不阻塞发布，但开发者发布记录必须标为“未实机验证”。

## References

规格第 8、13、20 和 21 节、`GOAL-04`、`SET-21` 至 `SET-35`、`REL-08` 至 `REL-09`

## Comments

- 2026-08-12：事项 20 功能提交 `9d4cbe00077a5b8cbda6c10c3d67f4a63077d33f` 经 [PR #27](https://github.com/CoolPlume/azzs/pull/27) 转 Ready 后以普通 merge 合入 `codex/v1-integration`，integration merge SHA：`5974e5517972f7cbabe72331f2f67d8b0deb1c16`。
- PR #27 的 Windows read-only validation run `31607529540` 在 x64 Release 与 ARM64 Release 均成功；host guardrails `cmake --workflow --preset host-guardrails` 为 19/19 通过，包含 settings catalog 与 system settings apply 合同。
- 首版目录修订为 `1`，包含 `setting.classic-context-menu`、`setting.windows10-explorer` 与仅引用两项的 `plan.recommended`。ITBar7Layout 未进入生产适配器，已整理为 `.scratch/windows-initial-setup-workbench/issue-20-itbar7layout-windows-handoff.md`，待一次性 Windows 实机确认。
- 未验证真实 Windows Explorer 重启、实际界面效果、UAC 和 WinUI 真实交互；CI、合同测试和 host guardrails 不外推为这些边界通过。
