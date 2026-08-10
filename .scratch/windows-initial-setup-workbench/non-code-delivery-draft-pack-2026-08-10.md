# 非代码交付物候选草案包

Status: draft-non-authoritative  
Review date: 2026-08-10  
Scope: 只整理实现前的治理、证据、发布和维护交付物；不开始产品代码，不改变主规格、CONTEXT、ADR 或事项状态

> **历史证据声明**：本文件仅保留截至 2026-08-10 09:03 CST 的工作树快照、当时未决判断和机械计数，不再作为现行实施合同。产品与工程结论已由[首版规格](spec.md)、[领域词汇](../../CONTEXT.md)、[ADR](../../docs/adr/)、本目录中 `Status: resolved` 的答案登记和[现行事项](issues/)取代；有冲突时以后者为准，本文计数与状态使用前必须重新验证。

本文把现有研究和审计中已经识别、但尚未形成正式文件的交付面集中成模板。它不是 `SECURITY.md`、`SUPPORT.md`、隐私政策、NOTICE、发布批准或法律意见；在维护者确认 owner、渠道、承诺和适用范围前，不得把本文内容复制为生效政策。

## 1. 交付面总表

| 交付面 | 现在可以准备 | 必须等待或由维护者确认 | 最终产物 | 当前状态 |
| --- | --- | --- | --- | --- |
| 安全报告与支持政策 | 文件结构、漏洞信息最小字段、公开 Issue 脱敏提示、支持版本表模板 | 私密报告入口、响应/披露承诺、首个 Release 后 latest 或 N/N-1 窗口 | `SECURITY.md`、`SUPPORT.md`、事项 30 的验收链接 | `needs-info` |
| 隐私与外部数据流 | 来源解析、目录更新、应用更新、紧急通知、诊断导出的数据流分类表 | 是否允许后台检查、保留期限、用户提示、跨境/第三方服务口径 | `PRIVACY.md` 或等价说明、数据流矩阵、事项 17/21/30 证据 | `needs-info` |
| 第三方许可与终端同意 | 资产身份、版本、摘要、许可/NOTICE、EULA/offer 字段模板 | 再分发权利、条款接受责任、可选推广默认值、未知条款回退 | BOM、`THIRD-PARTY-NOTICES`、逐项审核记录、`install_profile` consent 字段 | `needs-info` |
| 开发者上手与验证 | 工具链候选、环境变量命名、文档检查和目录校验命令模板 | Windows/VS/MSVC/SDK 锁版、构建系统和真实机器 | 开发者指南、验证矩阵和固定命令记录 | `ready-for-agent`（仅文档） |
| 发布 provenance 与制品清单 | 三轴状态、资产摘要、source commit、SBOM、attestation 字段模板 | Q8/Q10、正式 builder、保留期限、真实 Windows 证据 | 每个候选制品的 provenance/兼容性/限制记录 | `needs-info` |
| 紧急撤回运行手册 | 触发、阻止、解除、离线过期和事件字段草案 | 跨类别唯一 owner、控制元数据首次信任、真实演练 | 可执行 runbook、演练记录、通知修订历史 | `needs-info` |
| 目录维护审核 | 修订模板、来源/模式/安装档案/许可检查表、回退字段 | 审批人、复核周期、过期策略、首版软件集合 | 每次目录修订的审核包和 `release_state` 晋级记录 | `needs-info` |
| 诊断导出样例 | 脱敏样例、单文件读取合同、版本兼容和采集缺口字段 | 最终日志字段、隐私边界、跨版本保留策略 | 样例导出、读取说明、脱敏测试语料 | `ready-for-agent`（仅合同/样例） |

`needs-info` 表示不能在未决产品答案前把交付物视为完成；`ready-for-agent` 只表示可以先整理模板，不表示实现、实机或发行通过。

## 2. 每类交付物的最小字段

### 2.1 安全报告与支持

候选文件在生效前至少要回答：

- 私密漏洞入口由谁持有，是否有安全邮箱、平台表单或其他受控渠道；公开 Issue 明确不得提交凭据、利用细节或未脱敏日志。
- 收件确认、严重性分级、临时缓解、修复、披露和用户通知由谁负责；没有承诺时必须写“未承诺”，不能用模糊的“及时处理”。
- 首个公开 Release 后支持哪些版本：`latest`、`N`、`N-1` 或其他窗口；窗口结束后的用户迁移路径是什么。
- 高权限执行、目录控制元数据和第三方安装器风险如何转为安全公告、撤回或阻止动作。

未获得维护者确认前，README/CONTRIBUTING 只能说明项目仍处规格阶段和公开反馈入口，不能声称已经提供安全响应服务。

### 2.2 隐私与数据流

先建立一张按事件而不是按“是否遥测”分类的矩阵：

| 事件 | 是否用户主动 | 可能访问的数据 | 外部目的地/处理者 | 本地保留 | 用户可见状态 | 失败语义 |
| --- | --- | --- | --- | --- | --- | --- |
| 来源解析与软件下载 | 待确认 | 原始地址、重定向、版本、架构、摘要 | 上游站点/托管服务 | 解析快照、缓存 | 连接/解析状态 | 等待联网、暂时无法解析或外部交接 |
| 目录包更新 | 待确认 | 目录修订、控制元数据 | 项目发布源 | 当前/上一可信修订 | 更新结果与时效 | 保留上一份可信目录 |
| 应用自更新 | 必须单独确认 | 版本、资产、设备状态迁移结果 | 项目发布源 | N/N-1 资产和证据 | 安装事务/健康/回退分层 | 阻止、回退或结果未知 |
| 紧急安全通知 | 启动或高风险操作触发 | 通知修订、目标身份、时效 | 项目通知源 | 上次可信修订 | 可能过期/已阻止 | 继续使用最后可信结果或失败关闭 |
| 诊断导出 | 用户主动 | 已脱敏环境、操作链和错误 | 用户选择的保存位置/交接渠道 | 本机日志 | 导出成功/缺口 | 不自动上传，缺口必须显式记录 |

表中“待确认”不允许被实现者改成静默后台网络行为；每一行都要绑定 owner、`observed_at`、策略修订和证据新鲜度。

### 2.3 第三方许可、EULA 与 offer

项目再分发权利和终端用户同意是两个独立字段。每个启用软件的 `install_profile` 候选记录至少应包含：

```text
profile_id
supported_product_identity
supported_versions_and_architectures
installer_kind_and_parameters
eula_id / eula_revision / eula_uri
privacy_notice_id / privacy_revision / privacy_uri
optional_offer_id / offer_revision / default_behavior
user_consent_required / consent_timestamp / consent_result
unknown_terms_behavior
result_detector_id / detector_revision
reviewer / reviewed_at / evidence_location
```

条款、隐私说明或可选推广无法可靠识别时，候选默认进入交互式确认或外部交接；不能以项目已经取得镜像再分发权来代替用户同意，也不能把用户拒绝可选推广记成安装失败。

### 2.4 开发者上手与验证

当前没有构建系统、源码或测试框架，因此只能先写“待填命令”模板。最终指南必须区分：

1. 文档/结构检查：Markdown 链接、规格 ID、事项依赖、TOML 解析、公开卫生、GitNexus。
2. 无界面核心检查：无需 WinUI、管理员权限、网络或真实电脑的测试命令。
3. 适配器合同检查：隔离 Windows 环境、失败/取消/重复调用和资源清理。
4. 实机证据：Windows 版本、架构、目标软件版本、UAC、安装器、重启、崩溃和原始日志。
5. 发布门禁：最终资产摘要、source commit、SBOM、许可/NOTICE、provenance 和证据保留。

任何命令都要记录工具版本、输入快照、退出码、结果文件和适用范围；“命令尚未存在”应作为 `not-started`，不能写成通过。

### 2.5 发布证据包

每个开发构建、候选 Release 和公开 Release 分别生成一份记录，至少包含：

```text
artifact_id / artifact_kind / package_shape / content_edition / architecture
source_commit / builder_identity / toolchain_versions
sha256 / sbom_ref / attestation_ref / notice_ref
catalog_revision / policy_revision / license_review_revision
windows_build / target_software_versions / raw_log_locations
engineering_status / release_status / hardware_status
known_gaps / freshness_class / observed_at / reviewer
```

这三种记录不可互相升级：CI 绿灯不能代替实机证据，实机通过不能代替许可审核，本机试用目录不能进入正式制品。

### 2.6 紧急撤回运行手册

在 Q4g-h 冻结前，只能准备字段和演练模板。最终 runbook 要明确：

- 触发条件、目标身份、最小阻止范围、修订号、到期和解除条件；
- 软件来源、安装档案、系统设置、软件优化、其他受控能力和工作台版本是否都能被阻止；
- 离线时使用的最后可信修订、过期提示和失败关闭规则；
- 普通目录回退、应用回退和卸载不得降低安全阻止版本；
- 谁发布、谁审核、谁值守、谁复核解除，以及一次演练如何证明第二实例和重启场景不会绕过阻止。

### 2.7 目录维护审核包

每次修改 `catalog/software-catalog.toml` 或目录包前，保留一份变更记录：

```text
catalog_revision / parent_revision / change_owner / reviewer / reviewed_at
changed_stable_ids / source_revisions / install_profile_revisions
dependency_and_conflict_result / runtime_load_result / release_gate_result
license_and_notice_evidence / eula_offer_evidence
rollback_target / emergency_notice_revision / freshness_result
```

`enabled = true`、`release_state = "release"`、本机试用目录和公开制品清单必须分别审核；任何一项不能由另一项推导。

### 2.8 诊断导出样例

样例应覆盖至少：下载失败、安装器退出码与目标状态矛盾、要求重启、目录加载失败、恢复冲突、权限失败、更新健康超时和回退失败。只给读取端一个导出文件时，仍能还原：构建与环境摘要、用户命令、关联事件链、失败阶段、原始错误、最后可信状态和采集缺口。样例中不得使用真实邮箱、用户目录、计算机名、MAC/IP、SSID、凭据或可回溯的敏感 URL。

## 3. 明天接手顺序

### 维护者先回答

按[晨间决策影响图](morning-decision-impact-map.md)一次回答第一轮：Q4a-b、Q10、Q8、Q9、Q2、Q3、Q11a-b、Q12-Q15、Q5a-e、Q6。Q1 继续由另一任务回答，不在本文件代答。回答可写“接受建议”“修改为……”“暂缓”，但“暂缓”不能被当作默认接受。

### 回答后重算

1. 重算 Q4c-h、Q5f-l、Q7、Q16-Q19 及对应 `REDIST-D*`、`RELEASE-D*`、`INSTALLER-D*`。
2. 给每个原子需求指定一个 owner、consumer、verification kind/id、evidence location 和 freshness policy；不要用引用次数代替所有权。
3. 拆开事项 31 的早期架构/测试门禁与发布期综合门禁，解除 31/32 的语义闭环；拆开事项 30 的本地公开卫生与外部仓库动作。
4. 只有正式文档同步、负向验收、证据登记和 triage 重跑后，才提升对应事项状态。

## 4. 收尾检查命令

以下命令只做文档/结构检查，不证明产品功能或 Windows 实机通过：

```sh
date '+%Y-%m-%d %H:%M:%S %Z %z'
git diff --check
gitnexus status
gitnexus check --cycles
python3 -c 'import tomllib, pathlib; tomllib.loads(pathlib.Path("catalog/software-catalog.toml").read_text())'
rg -n '^(Owner|Consumer|Verification|Evidence):' .scratch/windows-initial-setup-workbench/issues
rg -n -i '幂等|重放|并发|锁|原子|fresh|stale|expires|operation_id|batch_id' .scratch/windows-initial-setup-workbench/spec.md .scratch/windows-initial-setup-workbench/issues docs/adr
```

链接、规格覆盖、事项依赖和敏感信息扫描仍应使用晨间审计中已经复现的脚本/命令；没有脚本文件时不要把手工抽样写成全仓通过。

## 5. 不得提前做的事

- 不把本文或研究推荐复制进正式规格、ADR、CONTEXT 或事项，除非维护者明确确认并记录 supersede/同步关系。
- 不创建生效的 `SECURITY.md`、`SUPPORT.md`、隐私政策或 NOTICE 来暗示渠道、承诺或许可已确定；只能在 owner/内容确认后落地。
- 不把 `ready-for-agent`、TOML 可解析、GitNexus 无环或文档链接通过写成应用、功能、实机或发布通过。
- 不将候选审核稿、`project_backup`、本机试用目录或未审核第三方资源加入公开制品清单。
- 不 stage、commit、push、创建 Release 或更改远端设置；这些是独立授权动作。

## 6. 当前判断

本轮治理/安全/支持/许可/隐私扫描没有发现新的独立 P0/P1 根问题。当前最省时的下一步不是继续扩写模板，而是让维护者回答第一轮决策；答案一到，按依赖图同步正式文档并重跑 owner/证据/triage。 
