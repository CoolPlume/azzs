# 实现系统优化撤销

Type: task  
Status: completed
Resolution: completed
Blocked by: 10
Owner: issue-11
Claimed by: codex/issue-11-system-settings-undo
Consumers: 12, 15, 18, 21
Verification: 撤销状态机测试覆盖原值恢复、下架、目录变化、失败重试、删除保护和重启验证。
Evidence freshness: 绑定 feature head、integration merge SHA、恢复记录格式和当前验证能力；恢复语义或平台映射变化后必须重验。目录变化仍以执行快照和恢复记录为准，不得用新目录证据替代旧快照。

## Goal

依据设备级恢复记录撤销设置，并在目录变化、失败和重启后保持撤销语义。

## Acceptance Criteria

- [ ] 撤销恢复该项最近一次修改前的记录值，而非统一默认值。
- [ ] 从“推荐总体优化”执行的设置仍逐项撤销，不创建与单项设置相互冲突的总体方案撤销状态。
- [ ] 已下架但有恢复记录的设置仍可撤销且不能再次应用。
- [ ] 撤销失败时保留入口、显示原因并允许重试。
- [ ] 撤销等待重启时，验证成功后才从可操作列表移除。
- [ ] 目录更新或下架后，撤销和重启验证仍使用原执行快照与恢复记录。
- [ ] 用户主动删除恢复记录前看到影响范围并再次确认。
- [ ] 存在等待重启或待恢复流程时禁止删除恢复记录。
- [ ] “恢复 Windows 11 默认”作为可直接选择的目标操作时仍在执行前保存当前状态；它不替代依据历史原值进行的“撤销”。
- [ ] 撤销经典右键菜单或资源管理器样式后，如需重启资源管理器，继续提供立即处理或稍后处理并保留“等待资源管理器重启”状态，验证成功前不移除恢复入口。

## References

`SET-07` 至 `SET-20`、`SET-29` 至 `SET-35`

## Comments

- 2026-08-12：事项 11 feature head `84389f6ef5676133e1d93748d90e8223af39038d` 经 [PR #24](https://github.com/CoolPlume/azzs/pull/24) 以普通 merge 合入 `codex/v1-integration`，integration merge SHA：`3b7e0728e34cf6bf698767d7b1a8bc19f0cb7de3`。
- PR #24 的 Windows read-only validation run `31603057206` 在 x64 Release 与 ARM64 Release 均成功；已执行定向 `azzs_system_settings_apply_contract` 构建与 `system-settings-apply.contract` CTest，UI design contract 与 GitNexus staged 门禁均通过。
- 实现覆盖最近原值撤销、下架条目撤销、失败重试、Explorer 重启等待与验证、冻结执行快照、恢复记录删除保护及 Windows 11 默认恢复；恢复存储 v2 兼容读取 v1，并提供事项 11 可消费的类型化恢复记录接口。
- 未验证真实 Windows 实机上的 Explorer 实际重启、UAC 和 WinUI 真实交互；自动测试、静态门禁和 CI 不外推为这些边界通过。
