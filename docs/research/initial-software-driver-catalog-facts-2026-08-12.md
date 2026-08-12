# 首版软件与驱动目录事实记录

观察时间：2026-08-12（Asia/Shanghai）

本文只保存事项 19 所需的可复现官方原始事实。它不是许可意见、来源真实性批准、Windows 执行能力证明或正式发布证据；没有被官方页面确认的版本、架构、离线、静默、完成边界和结果检测事实保持 `unknown`，对应目录或受控档案不得据此猜测。

## 软件原始来源

| 稳定 ID | 官方原始入口 | 观察到的事实 | 未闭合边界 |
| --- | --- | --- | --- |
| `qq` | https://im.qq.com/index/#/windows | Windows 个人版发布页显示版本 `3.2.32`，日期 `2026-07-30` | 架构、安装包形态、静默、完成检测未在本事项中固化 |
| `sogou-input` | https://shurufa.sogou.com/windows | Windows 页面提供 `pinyin_guanwang_16.7.exe`；安装档案基线为 `16.7` | 受控 Windows 执行、完成边界、结果检测和重启验证未实现 |
| `game-cheats-manager` | https://github.com/dyang886/Game-Cheats-Manager/releases | 官方 Releases 最新非预发布版本为 `v2.4.6`，日期 `2026-03-16` | 安装器架构、静默、离线和完成检测未闭合 |
| `cheat-engine` | https://www.cheatengine.org/downloads.php | 官方下载页显示 Windows `7.7` | 安装器架构、静默、离线和完成检测未闭合 |
| `office-tool-plus` | https://otp.landian.vip/en-us/download.html | 官方下载页显示 `11.5.7.0`，提供 x64、Arm64 | 受控执行、完成边界、结果检测未实现 |
| `internet-download-manager` | https://www.internetdownloadmanager.com/download.html | 官方页面为 Windows 30 天试用下载入口 | 动态安装器版本、架构、静默、离线和完成检测未固化 |
| `the-geometers-sketchpad` | https://www.dynamicgeometry.com/General_Resources/Sketchpad_About.html | 官方页面确认 Version 5 及许可／授权码说明 | 尚未证实稳定官方下载资产；当前入口只作为官方事实页，保持发布门禁为 draft |
| `java-runtime` | https://www.oracle.com/java/technologies/downloads/ | Oracle 页面显示 JDK `26.0.2`，JDK `25` 为 LTS | 目录暂不启用；JDK/JRE 选择、架构、静默、离线和受控执行未闭合 |
| `dotnet-runtime` | https://dotnet.microsoft.com/download/dotnet | 官方页面显示 .NET `10.0.11` LTS | 目录暂不启用；安装资源冻结、完成检测和受控执行未闭合 |
| `directx-runtime` | https://www.microsoft.com/en-us/download/details.aspx?id=35 | DirectX End-User Runtime Web Installer `9.29.1974.1`，页面日期 `2024-07-15` | 目录暂不启用；Web Installer 的离线边界、静默和完成检测未闭合 |
| `powershell-7` | https://github.com/PowerShell/PowerShell/releases/tag/v7.6.4 | 官方 Release 提供 `win-x64.msi`、`win-arm64.msi` | 目录暂不启用；重启和结果检测未实现 |

目录只保留上述稳定原始入口，不固化随发行变化的最终 `.exe` 或 CDN 直链。五项需提示普通软件继续使用 `tier = "normal"`，提示只说明安全软件／反作弊、账户／联网游戏、商业许可或候选资源未验证等影响，不代表项目背书。

## 驱动原始来源

| 稳定 ID | 类型 | 官方入口 | 观察到的事实与边界 |
| --- | --- | --- | --- |
| `amd-auto-detect-and-install` | `assistant` | https://www.amd.com/en/support/download/drivers.html | AMD 支持页提供 Radeon/Ryzen 自动检测与安装工具；工作台只做官方入口交接，不下载或安装具体包 |
| `intel-gpu-driver-page` | `vendor_page` | https://www.intel.com/content/www/us/en/download-center/home.html | Intel 官方下载中心；硬件类型仅登记已注册的 `gpu` |
| `nvidia-gpu-driver-page` | `vendor_page` | https://www.nvidia.com/Download/index.aspx | NVIDIA 官方驱动选择页；工作台不匹配、下载或安装具体包 |

驱动入口不携带命令、脚本、驱动包地址、硬件选择器或安装参数。事项 19 不实施 Windows 驱动执行器或实机验证。

## 受控安装档案边界

`sogou-input-defaults-v1` 是本事项唯一包含安装阶段偏好的档案，记录：

- `sogou-input-disable-search-candidates-v1`：自定义安装阶段，关闭搜索候选，默认拒绝；
- `sogou-input-decline-tencent-yuanbao-v1`：安装完成阶段，不安装腾讯元宝，默认拒绝；
- 两项均使用固定回退顺序：受控自动处置、工作台确认、官方安装界面。

注册表还为十一项软件保存架构、离线和静默等类型化事实；未观察到的值显式为 `unknown`。本事项只注册声明，执行就绪状态为 `declaration_only`。因此当前目录可通过运行时结构校验，但正式发布门禁必须失败，并明确记录 `draft_release_state` 与 `install_profile_not_release_ready`；不能写成 Windows 已执行或已验证。
