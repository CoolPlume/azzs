# 实现系统设置目录生命周期

Type: task  
Status: ready-for-agent  
Resolution: open
Blocked by: 01, 02
Owner: issue-04
Claimed by: codex/issue-04-settings-catalog
Consumers: 10, 18, 20, 21
Verification: 系统设置目录加载、预览、导入、回退、独立性与错误关闭合同测试。
Evidence freshness: 绑定当前提交、模式版本和设置目录修订。

## Goal

建立与软件和驱动目录相互独立的系统设置目录包更新、调试模式手动导入和回退流程，并用同一组设置项承载用户可见的单项系统优化与推荐总体优化。

## Acceptance Criteria

- [ ] 系统设置目录包拥有独立版本、稳定标识、更新、调试模式手动导入和回退，并可同时包含系统优化方案与系统设置项；普通和高级视图没有导入入口。
- [ ] 更新前分别预览新增、变更和下架项目，用户确认后才加载。
- [ ] 整体错误或未知执行语义拒绝目录并保留上一份可用版本；未知展示或说明内容可忽略。
- [ ] “推荐总体优化”只按稳定标识引用同一目录中的设置项，并声明默认选择；不复制设置的执行、检测、历史或撤销定义。
- [ ] 总体优化中的引用失效或关系错误只禁用该方案并说明原因，不影响仍然有效的单项设置。
- [ ] 设置项可以声明说明来源、已知适用的 Windows 版本范围、默认选择、恢复要求以及资源管理器或 Windows 重启要求。
- [ ] 网页和文章只作为可查看的来源；其内容变化不会直接改变执行规则，只有经过维护并加载新的目录版本后才生效。
- [ ] 加载、升降级目录不会自动修改当前系统优化状态。
- [ ] 目录变化不会破坏已应用设置的恢复记录或撤销能力。
- [ ] 软件与驱动目录或软件优化目录的变化不联动改变系统设置目录，系统设置目录变化也不联动改变另外两类目录。

## References

`CAT-01` 至 `CAT-12`、`CAT-21` 至 `CAT-28`、`CAT-37` 至 `CAT-40`、`SET-15` 至 `SET-20`、`SET-21` 至 `SET-35`

## Comments

- 2026-08-11：基于 `origin/codex/v1-integration` 的精确提交 `fb4ee87a5dbbcca7b9f9b454dd7f91cf34640ba1`，实现提交为 `a4e302825f16aefde7a7fbcc6c7621c0950a647a`。新增独立的 `azzs_settings_catalog_domain`、`azzs_settings_catalog_lifecycle` 与 `azzs_settings_catalog_file_adapter` target，并通过统一 `azzs_project_target` / `azzs_register_test` 接缝注册 `settings-catalog.contract`；未修改事项 02 的状态、日志或占用模型，也未接入 WinUI、系统优化编排或系统设置执行器。
- 2026-08-11：合同覆盖独立模式与修订、稳定标识解析、更新新增/变更/下架预览及精确确认、调试导入与降级、回退、未知执行语义整包拒绝、未知展示内容忽略、方案缺失引用与循环的局部禁用、持久化失败保留旧目录、N-1 只读恢复、共享占用、恢复记录和另外两类目录聚合隔离。定向 `settings-catalog.contract` 为 1/1 Pass，`cmake --workflow --preset host-guardrails` 为 11/11 Pass，`git diff --check` 与提交前 GitNexus staged/PDG 检查通过。
- 2026-08-11：未执行真实 Windows 注册表写入、系统恢复记录创建或系统设置修改；Windows 10/11 实机、资源管理器/Windows 重启行为、事项 10 编排、事项 18 设置页宿主与事项 20 首版目录内容仍由各 owner 后续验收。本事项保持 `Resolution: open`，Draft PR 不据此结票、转 Ready 或合并。
- 2026-08-11：完成 Draft PR 独立双轴复核收口：设置与方案的不可变语义 tombstone 随设备状态跨目录代次持久化，覆盖下架后复用、执行/检测/恢复/重启、设置依赖、风险/强制尝试以及方案成员和顺序；调试导入改由生命周期注入的应用核心只读授权决定，并为加载失败、校验拒绝、确认与成功记录仅保留来源类型和末级文件名的结构化审计。设置目录文件格式补齐设置级依赖、类型化可比较 Windows 版本范围、风险与强制尝试规则，并兼容读取上一版格式。定向 `settings-catalog.contract` 为 1/1 Pass，`cmake --workflow --preset host-guardrails` 为 11/11 Pass；带 PDG 的 GitNexus 路径检查确认导入只到受限文件读取、解析和目录校验，持久化只消费事项 02 状态接缝。未扩大真实 Windows 或下游编排验证，`Resolution` 继续保持 `open`。
- 2026-08-11：第二轮独立复核后，状态格式 V1 因无法证明完整跨代身份 ledger 而保持只读并要求受控状态恢复，目录包格式 V1 因缺少设置依赖、风险与强制尝试等执行事实而明确拒绝；不再宣称无损兼容。调试导入的 `accepted` 审计仅在预览真正就绪后记录，同 revision 不同内容改为关联的 `rejected` 终态；来源标识保留末级文件名和规范化路径指纹，不保留绝对路径。新增合同覆盖 V1 N-2 tombstone、V1 双方案依赖有损、同 revision 冲突终态及同名不同路径来源，`Resolution` 继续保持 `open`。
