# 实现离线资源与缓存管理

Type: task  
Status: ready-for-agent  
Resolution: completed
Blocked by: 02, 03, 05
Owner: issue-07
Claimed by: codex/issue-07-evidence
Consumers: 08, 14, 18, 21
Verification: 下载与缓存适配器合同测试，覆盖断网、续传、并发、半成品、清理、介质失效和重试。
Evidence freshness: 绑定当前提交、缓存模式和资产身份；执行或候选构建使用的来源可用性重新观察。

## Goal

支持离线资源识别、可靠下载和可配置缓存，使断网和重复安装场景可恢复。

## Acceptance Criteria

- [x] 区分超大离线版内置包、下载缓存、仅联网和暂不支持状态。
- [x] 超大离线版可以携带五个需要额外提示的普通软件及其他候选软件的安装资源；具体包含范围按目录与制品构建清单决定。未被当前制品携带且无可用缓存的软件在断网时准确显示“等待联网”。
- [x] 在线安装器即使已下载到本机仍标记为需要联网，不把它误报为离线软件包。
- [x] 依赖不完整的软件等待联网，不阻断无依赖软件和系统优化。
- [x] 软件包缓存覆盖官方完整安装包、在线安装器和项目离线软件包，按稳定标识、版本、架构和来源区分，并由便携版和安装版共享。
- [x] 只有可下载文件来源进入软件包缓存；Microsoft Store、`winget` 等托管安装来源不生成伪造缓存文件。
- [x] 支持系统目录、本机其他磁盘、网络共享和 U 盘位置及空间检查。
- [x] 支持立即删除、7 天、30 天和不自动删除，清理不删除内置资源。
- [x] 联网“安装最新版”不回退旧缓存，断网使用缓存时显示具体版本。
- [x] 切换缓存位置不自动迁移旧内容，自定义位置失效时不静默回退。
- [x] 当前目录移除内置资源对应项时不绕过目录安装，并可通过兼容有效目录重新启用。
- [x] 使用临时文件和完成标记保护并发读取，中断后清理半成品。
- [x] 暂停下载时保留受保护的临时文件；来源支持续传时继续已有进度，不支持或资源变化时说明后重新下载。
- [x] 暂停进度只在当前运行期间保留；关闭工作台时清理未完成文件，下次不跨启动续传。
- [x] 相同软件、版本、架构和来源可复用已完成缓存；在线安装器被缓存后仍保持需要联网状态。
- [x] 下载失败自动重试 2 次，仍失败时继续批次并允许单项重试。

## References

`OFF-01` 至 `OFF-09`、`CACHE-01` 至 `CACHE-23`、`SRC-18` 至 `SRC-20`、`SRC-24` 至 `SRC-25`

## Answer

- 功能提交为 `879436b0fe300190d9c5b978b54389b0fb691b37`；PR [#26](https://github.com/CoolPlume/azzs/pull/26) 已以合并提交 `1b31f768e63bde3896d7d031778664b70e24214d` 进入 `codex/v1-integration`。先核对该 integration 合并提交的 tree 与已验证 tree `56741910699810a9ab14a181dc80695fdcfd9961` 完全相同，故本票据的纯文档证据提交不重复运行构建、合同测试或 CI。
- 在 Windows 11 x64 本机，`eng/build.ps1 -Architecture x64 -SkipCoreSmoke` 的 Release/WinUI 链接成功；`offline-package-cache.contract` 与 `local-package-cache-adapter.contract` 均通过，合计 2/2。
- GitHub Actions run `31704941827` 的 x64 job `94463011817` 在功能 head 上成功。host configure/build 成功；24 个测试中 22 个通过。仅 `execution-log.contract` 与 `windows-device-data.contract` 因未准备 ACL root 及 directory-symlink/isolation root 而失败，未将这两个环境前置条件失败写作通过。
- 自动矩阵的 ARM64 job `94463011904` 显示编译/链接成功，仅作信息性观察：未主动执行 ARM64 运行时合同或验收，完整 ARM64/八制品验证延期，不能作为事项 07 结票证据。
- 真实网络共享、USB 介质与网络传输未在本次验证中执行，保持未验证状态。

## Comments

- 2026-08-10：Q4 不设项目级信任门槛，Q5 已冻结同机并发与 `ProgramData` 权威状态，Q9 已允许候选资源随包与公开发布；缓存提交、同机锁、外置介质和安装档案属于本事项应实现并验证的工程工作，不再等待维护者补充产品决定。本事项改为 `ready-for-agent`，其依赖仍只按 `Blocked by` 的完成结果机械解除。
- 2026-08-13：依据已合入 integration 的功能树、x64 本机与 GitHub x64 证据完成结票；环境未准备导致的两个 host 合同失败、真实网络共享/USB/网络传输，以及主动 ARM64 验收均明确保留为未验证或延期，未扩大为通过结论。
