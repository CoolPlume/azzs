# Windows 初装工作台 {{VERSION}}

Windows 初装工作台：帮助新装 Windows 完成驱动准备、系统优化、常用软件安装和软件优化。

<!-- 发布时替换双花括号占位符；删除未发布的占位说明后再创建 GitHub Release。 -->

## 本次发行

- 版本：`{{VERSION}}`
- 发布日期：`{{RELEASE_DATE}}`
- 发行通道：`{{RELEASE_CHANNEL}}`
- 主要变化：{{CHANGE_SUMMARY}}
- 已知问题：{{KNOWN_ISSUES}}

## 下载选择

请根据 Windows 设备架构和需要的发行形态选择制品。x64 默认优先展示；ARM64 Windows 应使用 ARM64 工作台包。实际携带内容以本次 Release 的制品清单为准。

| 制品 | 下载 |
| --- | --- |
| Windows 初装工作台 标准版 x64 便携版 | {{ASSET_STANDARD_X64_PORTABLE}} |
| Windows 初装工作台 标准版 ARM64 便携版 | {{ASSET_STANDARD_ARM64_PORTABLE}} |
| Windows 初装工作台 标准版 x64 机器级安装版 | {{ASSET_STANDARD_X64_MACHINE_INSTALLER}} |
| Windows 初装工作台 标准版 ARM64 机器级安装版 | {{ASSET_STANDARD_ARM64_MACHINE_INSTALLER}} |
| Windows 初装工作台 断网救援版 x64 便携版 | {{ASSET_RESCUE_X64_PORTABLE}} |
| Windows 初装工作台 断网救援版 ARM64 便携版 | {{ASSET_RESCUE_ARM64_PORTABLE}} |
| Windows 初装工作台 超大离线版 x64 便携版 | {{ASSET_LARGE_OFFLINE_X64_PORTABLE}} |
| Windows 初装工作台 超大离线版 ARM64 便携版 | {{ASSET_LARGE_OFFLINE_ARM64_PORTABLE}} |

## 使用前请注意

本项目提供的 Windows 应用发行包未签名。

项目不提供发布者或文件完整性校验信息，也不提供 SHA-256 校验值、源码提交关联或 provenance。

下载或启动时可能出现 SmartScreen 提示。

第三方来源未获项目验证；工作台记录来源、执行与结果事实，不表示来源、文件或安全已经验证。

Windows 10 22H2 是最低目标版本，并同时面向 Windows 11；这不是跨版本兼容性保证。更早版本会显示风险警告并允许继续运行，但不提供正常运行保证，实际缺失的系统能力可能使具体功能不可用。

## 安装与启动

{{INSTALLATION_AND_STARTUP_NOTES}}

## 反馈

请在 GitHub Issues 中说明所用版本、发行形态、架构、Windows 版本、复现步骤和可公开的错误信息；请勿提交密码、验证码、付款信息或其他敏感资料。
