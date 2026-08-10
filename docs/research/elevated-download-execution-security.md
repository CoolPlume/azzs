# 常驻管理员工作台的下载、缓存、执行与自更新安全研究

- 状态：研究结论与候选工程门禁，**不是已接受的产品决定或 ADR**
- 研究日期：2026-08-10
- 访问日期：2026-08-10
- 适用范围：WinUI 3/C++ 初装工作台的 URL 解析、重定向、第三方安装器下载与缓存、外置介质交接、管理员子进程启动、应用自更新、在线控制元数据、紧急安全阻止、UI Automation、强制终止和托管安装来源
- 来源边界：优先采用 Microsoft Learn、Windows SDK 文档、IETF RFC 和 IANA 注册表；没有运行安装器，也没有在真实 Windows 上验证 SmartScreen、证书吊销或文件系统竞态

## 1. 结论摘要

现行文档组合尚不能形成可发布的特权下载与执行安全合同。最重要的原因不是缺少风险提示，而是以下行为同时成立：工作台全程持有管理员令牌、自动解析并下载可执行文件、允许从可写外置或网络位置复用缓存、启动安装器，同时明确不验证第三方签名或哈希；一键自更新还长期不签名且不提供项目摘要。管理员父进程通过 `CreateProcessW` 创建的进程运行在调用进程的安全上下文中，UAC 文档也明确说明子进程继承父进程的访问令牌，因此被替换的安装器不会再遇到一次能弥补前序验证缺口的独立提权边界。[S01][S26]

需要把四种不同保证分开：

1. **HTTPS/TLS** 保护与当前服务器连接中的机密性和完整性，但不证明下载到的程序属于预期软件发布者，也不防止受控账号、源站或合法证书对应的错误站点提供恶意文件。[S05]
2. **完整文件摘要** 可标识精确字节并发现缓存或竞态中的替换；若预期摘要与文件来自同一条未认证通道，摘要不能独立证明发布者。
3. **Authenticode** 可验证受信证书链、代码签名发布者及签名覆盖内容未被修改；它不等于软件无恶意，也不能单独覆盖所有安装包格式和所有文件字节。[S15][S16][S18]
4. **Attachment Services、SmartScreen 和恶意软件扫描** 是附加的策略、信誉和检测层，不是来源真实性证明。SmartScreen 会参考 URL、文件、证书和下载信誉；未知或未签名文件可能警告，企业策略还可能完全禁止继续。[S19][S20][S23][S24]

研究建议把以下事项作为实现前 P0 决策，而不是在代码阶段临时猜测：

- 是否改变“第三方安装器不验证签名或哈希”；若不改变，建议取消工作台自动提权执行，退化为外部交接。
- 是否改变“应用不签名、无项目摘要、仍提供一键自更新”；四者无法组成独立可认证的更新链。
- 是否继续允许 HTTP 原始地址和 HTTP 最终安装文件；在没有独立制品认证时，建议安装器取得链只允许 HTTPS。
- 自定义缓存位于 U 盘、FAT/exFAT 或 SMB 时，是只作存储，还是允许直接执行；建议一律先复制到工作台受保护的本机暂存区后重新验证。
- U 盘厂商驱动继续作为外部交接，还是由工作台导入并安装；后者会取代 ADR-0009 的既有边界。
- 是否维持整个网络解析、HTML 解析和文件处理都在常驻管理员进程内；若维持，必须明确接受远程输入解析缺陷拥有高完整性进程的全部影响面。
- 三类在线目录、应用更新清单和紧急安全通知采用什么共同的真实性、防回滚、防冻结、防混搭与密钥轮换合同；HTTPS 和“用户主动更新”都不能替代控制元数据认证。
- 软件优化以外的高权限能力怎样被紧急阻止；当前只有软件优化方案拥有紧急撤回通道，无法及时停止已知有害的软件来源、发布者、安装档案、系统设置、受控能力或工作台版本。

另有两类实现门禁不应留给页面代码临时决定：UI Automation 必须绑定受支持安装器的真实进程、窗口、版本和控件结构；强制关闭或终止必须操作已捕获并重新验证的进程对象，不能按进程名、前台窗口或稍后重新查询的 PID 猜目标。Microsoft Store、`winget` 等托管来源也必须使用精确包标识、精确来源和受控调用目标，不能依赖模糊搜索或 `PATH` 中碰巧出现的同名程序。

## 2. 研究口径

本文使用三种标签：

- **官方事实**：由引用的一手资料直接支持。
- **项目建议**：根据官方机制和本项目威胁面推导出的候选门禁，尚未获维护者确认。
- **维护者决定**：会改变现行产品语义、ADR 或发布范围，本文不代答。

引用形式 `[Sxx]` 指向第 11 节。每条来源均列出原始 URL、文档标题和访问日期。引用只能证明其邻近事实；“项目建议”仍是本项目的工程判断。

## 3. 威胁模型

### 3.1 需要保护的资产

- 工作台持有的管理员令牌和目标 Windows 系统完整性。
- 安装批次、恢复记录、执行历史、目录快照与当前有效目录。
- 已完成缓存、离线资源、自更新候选和上一版本备份。
- 用户看到的软件身份、版本、架构、来源和发布者结论。
- 三类目录、紧急安全通知和应用更新清单的最高已接受版本、到期状态、签名角色与密钥轮换状态。
- 日志中可能出现的原始 URL、重定向地址、本地路径和证书信息。

### 3.2 不可信输入与边界

- 正式目录、手动导入目录中的 URL，以及 URL 的每一个重定向目标。
- DNS 结果、代理、HTTP 状态行和标头、HTML、JSON、文件名、MIME、`Content-Disposition` 与响应体。
- 官网、CDN、GitHub Release、项目备用源和在线安装器的实际字节。
- 用户可写目录、网络共享、U 盘、缓存元数据和另一个工作台实例。
- 安装器签名、证书链、吊销响应、时间戳和安装器启动后的子进程树。
- 自更新清单、新版程序、替换助手、回退副本和启动健康结果。
- 在线目录包、紧急安全通知、签名根元数据、时间戳/快照元数据和密钥轮换材料。
- UI Automation 元素属性、前台窗口、窗口句柄、进程名、PID、进程树和托管包搜索结果。

### 3.3 应纳入测试的攻击者

- 能控制网络但不能伪造受信 TLS 服务器身份的攻击者。
- 能控制某个目录包、重定向端点、合法 HTTPS 域名或发布账号的攻击者。
- 以同一 Windows 用户的非提权进程运行、能写入自定义缓存或外置介质的本地攻击者。
- 与当前实例并发、能在检查与使用之间替换路径、重解析点或文件的进程。
- 提供有效签名但发布者不符合该软件预期身份的第三方。
- 正常厂商在线安装器在启动后下载工作台无法观察的后续载荷。
- 能重放旧的合法签名元数据、冻结最后一份过期元数据、混搭不同修订目标，或控制单个在线签名密钥的攻击者。
- 在目标 UI 附近创建同名窗口/控件，或利用 PID 复用、相同进程名和可写 `PATH` 诱导工作台操作错误对象的本地进程。

### 3.4 本基线不能证明的事情

- 有效 Authenticode 签名不证明软件没有恶意，只证明签名身份与签名覆盖内容的完整性。[S18]
- TLS 不证明源站账号、发布流水线或合法发布者没有被攻陷。[S05]
- 工作台只能验证在线安装器自身，不能据此声称它之后下载的所有载荷已经由工作台验证。
- 把更新公钥嵌入未签名工作台只能建立取得该工作台之后的连续性；它不能独立证明首次取得的工作台或其中公钥确实属于本项目。
- 已拥有管理员权限、内核权限或能修改系统信任库的攻击者不在该下载边界能够解决的范围内。

## 4. 官方事实

### 4.1 常驻管理员与子进程

**官方事实 F-PRIV-01**：`CreateProcessW` 创建的新进程运行在调用进程的安全上下文中；UAC 文档说明父子进程是提示规则的例外，子进程继承父进程的访问令牌，并要求父子进程具有相同完整性级别。[S01][S26]

**项目含义**：常驻管理员工作台启动下载的安装器时，安装器直接获得高完整性上下文。界面上的风险披露不能降低这一令牌，也不是第二次真实性校验。

**官方事实 F-PRIV-02**：`CreateProcessW` 在 `lpApplicationName = NULL` 且路径含空格时可能把错误的前缀解释为可执行文件；微软明确把可放置 `Program.exe` 的情形称为危险，并建议显式传入应用路径。`bInheritHandles = TRUE` 会让所有可继承句柄进入子进程，也可用 `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` 限定清单。[S26][S28]

**官方事实 F-PRIV-03**：DLL 搜索路径中任何攻击者可写目录都可能造成 DLL preloading/binary planting；若进程以管理员权限运行，攻击者可能取得权限提升。微软建议使用完整路径并收紧 DLL 搜索路径。[S27]

### 4.2 URL、重定向与网络目的地

**官方事实 F-URL-01**：`WinHttpCrackUrl` 会分解 URL，但文档明确说它在分解前不检查 URL 的有效性或正确格式，甚至可能对格式错误字符串返回错误结果；它只支持 HTTP/HTTPS，并提供 `ICU_REJECT_USERPWD` 拒绝嵌入凭据。[S02]

**官方事实 F-URL-02**：RFC 9110 要求收到不可信 HTTP(S) URI 时把 `userinfo` 视为错误，因为它可能用于混淆真实 authority；scheme 和 host 不区分大小写，其他部分通常区分大小写；相对 `Location` 必须相对原目标 URI 解析。[S04]

**官方事实 F-URL-03**：WinHTTP 默认自动跟随除 HTTPS 降级到 HTTP 之外的重定向，默认最大 10 次；也可完全关闭自动重定向。RFC 9110 要求重定向时更新或移除连接、代理、源站、缓存、资源相关标头，并要求检测循环。[S03][S04]

**官方事实 F-URL-04**：WinHTTP 可以关闭自动身份验证、Cookie 和重定向，也可以拒绝 URL 中的用户名/密码。它允许通过安全标志忽略证书主机名、日期、CA 或用途错误；这些标志的语义就是接受相应无效证书。[S03]

**官方事实 F-URL-05**：TLS 1.3 的设计目标包括防窃听、篡改和消息伪造；RFC 9110 要求 HTTPS 客户端只接受受保护的响应。该保证属于传输通道，不是安装器发布者身份模型。[S04][S05]

**官方事实 F-URL-06**：IANA 把 loopback、private-use、link-local、shared、documentation、reserved 等 IPv4/IPv6 网段标记为非全局可达或特殊用途。WinHTTP 可在收到响应后查询实际连接的本地和远端地址；自动重定向时该结构只报告最终非 30x 响应对应连接。[S06][S07][S08]

### 4.3 缓存、ACL、重解析点与竞态

**官方事实 F-FILE-01**：文件和目录是 Windows 可保护对象；新文件默认 ACL 从父目录继承，也可以在创建时指定安全描述符。卷是否真正保存并执行 ACL，必须看 `FILE_PERSISTENT_ACLS`；FAT 不提供这一保证。[S09][S12][S13]

**官方事实 F-FILE-02**：卷是否支持命名流和重解析点分别由 `FILE_NAMED_STREAMS` 和 `FILE_SUPPORTS_REPARSE_POINTS` 表示，不能假设任意 U 盘、共享位置或本机卷都具备 NTFS 能力。[S12]

**官方事实 F-FILE-03**：未指定 `FILE_FLAG_OPEN_REPARSE_POINT` 时，`CreateFileW` 打开符号链接会得到目标句柄，截断或删除也作用于目标；指定该标志才尝试打开重解析点本身。微软专门提醒操作支持重解析点的文件系统时需要特殊处理。[S09][S10]

**官方事实 F-FILE-04**：`GetFinalPathNameByHandleW` 可从已打开句柄得到完全解析后的最终路径，例如把指向 D 盘的 C 盘符号链接解析为 D 盘路径；这证明句柄可用于确认实际对象，但文档没有承诺“检查路径后再按名称打开”不会发生竞态。[S11]

**官方事实 F-FILE-05**：`CREATE_NEW` 只在名称尚不存在时创建文件；不共享写和删除访问的打开句柄可阻止冲突的后续写入、删除或重命名请求。`ReplaceFileW` 把保存、重命名、替换和删除组合为一个 API，并尝试保留 ACL，但文档列出了部分失败后名称或属性可能处于中间状态的错误结果，且 `REPLACEFILE_WRITE_THROUGH` 不受支持。[S09][S14]

### 4.4 摘要、Authenticode 与发布者

**官方事实 F-AUTH-01**：`WinVerifyTrust` 的 `WINTRUST_ACTION_GENERIC_VERIFY_V2` 使用 Authenticode 策略；只有返回值严格等于零才表示受信，不能用 `SUCCEEDED` 判断。Software Publisher Trust Provider 可以验证 PE 来自受信软件发布者且签名后未被修改。[S15]

**官方事实 F-AUTH-02**：微软的 PE 验签示例说明该策略检查证书是否链到受信根、是否具备代码签名用途，并区分无签名、显式不信任、主题不受信和其他链错误。示例同时警告：嵌入签名只覆盖 PE 的特定部分，其他内容可能改变而不破坏签名。[S16]

**官方事实 F-AUTH-03**：Authenticode 用数字签名、CA 和证书链验证发布者身份及签名覆盖内容完整性。`WINTRUST_DATA` 可要求整条链吊销检查、只用本地缓存、应用 MOTW 策略，或显式关闭吊销检查；这些是不同的验证状态，不能都显示成“签名有效”。[S17][S18]

**项目含义**：完整文件 SHA-256 即使没有厂商公布的预期值，也仍应用于缓存身份、批次快照和防止“检查的是 A、启动的是 B”；但这种本地计算摘要只能证明同一字节，不证明厂商身份。若要把摘要用作来源认证，预期值必须来自独立受认证的维护来源。

### 4.5 MOTW、Attachment Services 与 SmartScreen

**官方事实 F-MOTW-01**：`IAttachmentExecute` 面向安全下载和附件交换；`SetSource` 的 URL/路径是主要安全区域判定依据，不设置来源时默认 Restricted Zone。`CheckPolicy` 可能返回允许、提示或禁止。[S19][S21][S22]

**官方事实 F-MOTW-02**：把文件复制到最终本地路径后，非临时目录应调用 `IAttachmentExecute::Save`。该调用**可能**运行病毒扫描或其他信任服务，也**可能**在 NTFS ADS 中附加证据；扫描服务甚至可能删除或修改文件。[S20]

**官方事实 F-MOTW-03**：SmartScreen 对下载应用和安装器检查已知恶意列表、文件下载信誉和数字签名信誉。未签名文件每个新版本都从零积累文件信誉，可能显示“Windows protected your PC”，企业策略可以禁止用户继续；Windows 11 Smart App Control 还可能阻止没有正面信誉的未签名文件。[S23][S24]

**官方事实 F-MOTW-04**：SmartScreen 官方概览明确说它不保护内部位置或网络共享中的恶意文件。`ShellExecuteEx` 的 `SEE_MASK_NOZONECHECKS` 会绕过由 `IAttachmentExecute` 建立的区域检查。[S23][S25]

**项目含义**：不能把“保存到了缓存”“文件带 MOTW”“SmartScreen 没弹窗”或“Attachment Services 返回成功”记录成发布者已验证。尤其在缺少命名流的卷、网络共享、U 盘或企业策略变化时，行为需要真实 Windows 证据。

### 4.6 未签名发布与更新

**官方事实 F-UPD-01**：Windows 要求可部署的 MSIX 使用有效代码签名证书，且证书必须链到设备信任根；Microsoft Store 提交后由 Store 签名。自签名证书主要适合测试，用户必须先安装并信任它。[S29][S30]

**官方事实 F-UPD-02**：Windows App Installer 可以为 MSIX 配置启动时或后台检查、提示、阻止启动和降级等更新策略，但它建立在已签名、可安装的包模型上，不能替代本项目未签名传统制品的真实性设计。[S29][S31]

**官方事实 F-UPD-03**：未签名的传统 Win32 文件仍可能由用户运行，但 SmartScreen 无法继承上一版本的签名发布者信誉，企业策略可能不允许“仍要运行”。因此“未签名”不是单纯文案成本，也会限制自动化发布和更新路径。[S24]

### 4.7 控制元数据、首次信任与紧急阻止

**官方事实 F-META-01**：The Update Framework（TUF）把 root、targets、snapshot 和 timestamp 分成不同角色。客户端必须维护受信 root，校验签名阈值、版本与到期时间；snapshot 把目标元数据的版本/长度/哈希绑定在一起，timestamp 再绑定当前 snapshot。该设计分别针对任意密钥妥协、回滚、冻结和混搭攻击，而不是把“文件有一个有效签名”当作完整更新协议。[S32][S33]

**官方事实 F-META-02**：TUF 的初始 root 必须通过带外方式作为信任根取得；后续 root 更新还要同时满足旧 root 与新 root 的阈值规则。`BCryptVerifySignature` 只回答给定公钥、摘要、签名和参数是否匹配，并不回答该公钥是否属于本项目。[S32][S34]

**项目含义**：签名更新清单、公钥或 root 元数据如果只嵌入未签名应用，可以阻止已正确取得该应用后的普通篡改，却不能认证首次下载本身。要宣称项目身份，首次信任还需代码签名、受信分发平台、用户通过独立可信渠道取得并核对的项目根，或明确取消自动替换。HTTPS、GitHub 账号和自描述公钥不能共同充当独立信任根。

**项目含义**：三类在线目录、应用更新和紧急安全通知都是能改变管理员行为的控制元数据。即使它们分开版本化，也至少需要共同的签名封装、角色/目标类型、单调版本、到期、长度/摘要绑定、最高已接受版本、密钥轮换和恢复策略。普通目录“回退”是用户选择的内容行为，不能同时回退安全元数据的最低接受版本。

### 4.8 UI Automation、进程与托管来源身份

**官方事实 F-UIA-01**：Microsoft UI Automation 安全说明要求自动化客户端理解跨进程与权限边界；UI Automation 属性由 provider 提供，调用方必须处理属性不受支持的情况。`AutomationId` 只在同级元素中预期唯一，不能保证整棵树唯一；runtime ID 只在 UI 元素当前桌面生命周期内唯一，元素被移除后可以复用。[S35][S37][S38]

**官方事实 F-UIA-02**：`GetWindowThreadProcessId` 返回创建指定窗口的线程，并可返回其进程 ID。它能把 HWND 绑定到一个当时的进程，但 PID 本身不是永久身份；Windows 在进程终止后可以复用进程 ID。[S36][S39]

**官方事实 F-PROC-01**：进程句柄在打开期间保持对该进程对象的引用；`GetProcessTimes` 可取得进程创建时间，`QueryFullProcessImageNameW` 可从进程句柄取得映像路径。`TerminateProcess` 接收进程句柄、异步启动终止，并可能使进程使用的全局数据状态受损。[S39][S40][S41][S42]

**官方事实 F-PROC-02**：Job Object 可以把一组进程作为一个单元管理，并对关联进程施加限制或发送终止；进程可以创建不自动归入同一 job 的后代，嵌套、breakaway 和安装器自身行为都要实际验证，不能从最初父 PID 推定永久完整的进程树。[S43]

**官方事实 F-MANAGED-01**：`winget install` 支持用 `--id`、`--exact` 和 `--source` 限定包，并另有版本、架构、scope 等参数；缺少这些约束时，搜索结果可能有多个候选。Microsoft Store URI 可以使用 Product ID 打开精确产品页。[S44][S45]

**项目含义**：安装阶段偏好不能只按界面文本、前台窗口、第一个匹配元素或单个 `AutomationId` 操作。强制关闭/终止也不能按进程名、窗口标题或陈旧 PID 执行。托管来源不能把模糊搜索结果或从 `PATH` 解析出的 `winget.exe` 当作已确认目标。

## 5. 候选工程门禁

本节全部是**项目建议**。维护者确认前，不应把其中任何一项改写成已接受需求。

### 5.1 URL 与重定向门禁

| 编号 | 候选门禁 | 失败结果 |
| --- | --- | --- |
| `G-URL-01` | 使用结构化 URI 解析；`WinHttpCrackUrl` 只负责分段，随后完整校验并规范化。拒绝空 host、`userinfo`、片段、控制字符、反斜杠混淆、超长组件和非 HTTP(S) scheme。 | `来源地址无效`，不发网络请求 |
| `G-URL-02` | 安装器、目录包和更新文件最终取得只允许 HTTPS；绝不设置忽略主机名、日期、CA、用途或弱签名的 WinHTTP 安全标志。 | `TLS 验证失败`，不得提供自动继续 |
| `G-URL-03` | 关闭 WinHTTP 自动重定向，每一跳手动解析、规范化、重新校验 scheme、host、端口、目的地址和来源策略；建议最多 5 跳并检测循环。 | `重定向被拒绝` 或 `重定向过多` |
| `G-URL-04` | 禁止 HTTPS 降级到 HTTP；跨源跳转只允许项目内置来源能力明确允许的官方 CDN/发布域，不能仅因状态码合法就跟随。 | `跳转目标不受允许` |
| `G-URL-05` | 禁用自动 Cookie 和源站身份验证，不发送用户凭据、授权头或不必要 Referer；跨跳重新构造请求。 | 需要登录的来源标为不支持自动处理 |
| `G-URL-06` | 对目录 URL、每个重定向 host 和最终连接执行私网/回环/链路本地/特殊用途地址检查；IPv4-mapped IPv6 同样归一化检查。代理场景无法可靠确认源站连接时单独记录限制。 | `目标不是允许的公网地址` |
| `G-URL-07` | 限定 URL、标头、HTML/JSON 和响应体大小、解析时间、连接/读取超时、并发数及软件包最大尺寸；不执行页面 JavaScript。 | `响应超过安全限制` |

`G-URL-03` 的跨域允许清单不能由手动导入目录自由扩大，否则手动导入会成为管理员网络访问能力的授权入口。目录只可引用项目已经内置的来源处理策略，符合 ADR-0027 的“数据扩展不增加执行能力”。

### 5.2 下载、缓存和 TOCTOU 门禁

建议采用以下顺序，避免路径检查与实际启动对象分离：

1. 在工作台拥有、ACL 明确且卷支持持久 ACL 的本机根目录中创建唯一暂存目录；根目录和每一级父目录都不允许普通用户或外置介质替换。
2. 用随机不可预测名称和 `CREATE_NEW` 创建临时文件，句柄不可继承；下载期间拒绝共享写与删除，边下载边计算完整文件摘要并执行大小上限。
3. 完成后刷新、取得文件 ID、最终路径、卷能力和重解析点事实；拒绝最终对象逃出受保护根、可疑重解析点、路径变化或文件 ID 变化。
4. 调用 Attachment Services 保存来源证据和策略；因为 `Save` 可能修改文件，之后重新打开受保护对象并重新计算摘要及验证签名。
5. 只有下载长度、完整文件摘要、文件 ID、最终路径、来源快照、架构、安装包类型、Attachment Services 结果和签名策略全部得到可接受结果，才写入“完成缓存”元数据。
6. 缓存复用时把文件视为重新输入，再做文件 ID、完整摘要、签名/发布者和路径检查；完成标记本身不能授权执行。
7. 启动前保持一个不共享写和删除的句柄，所有验证基于同一文件对象；再次确认文件 ID与摘要后才按绝对路径创建进程，并保持句柄至少到子进程成功创建。

外置或共享缓存的建议语义是“容量层”，不是“信任层”：

- U 盘、FAT/exFAT 和网络共享可以保存已完成字节，但不得从原位置直接以管理员令牌执行。
- 使用时复制到上述受保护本机暂存区，再按完整链路重新验证。
- 缺少持久 ACL、命名流或可验证来源证据时，显示准确能力缺口，不静默把它当成 NTFS 本机缓存。
- 多实例用独占锁、唯一临时文件和最后提交的完成记录协调；任一实例都不得读取另一个实例的半成品。

微软文档没有给出一个覆盖所有文件系统、SMB、重解析点和进程创建的完整“无 TOCTOU”配方，因此上述顺序是项目建议，必须用攻击性竞态测试验证，不能只靠代码评审宣称完成。[S09][S10][S11][S14]

### 5.3 安装器身份门禁

建议把结果建模为相互独立的事实，不能压缩成一个“安全/不安全”布尔值：

- `transport_security`：HTTPS 已验证 / HTTP / TLS 失败 / 未知。
- `content_digest`：已计算；若有独立预期值，再记录匹配 / 不匹配 / 无预期值。
- `signature_status`：受信有效 / 无签名 / 签名无效 / 证书链不受信 / 已吊销 / 吊销状态不可取得 / 格式不支持验证。
- `publisher_match`：符合该软件预期发布者 / 不符合 / 尚无预期发布者政策。
- `attachment_policy`：允许 / 需要提示 / 禁止 / 平台未提供。
- `launch_identity`：启动前文件 ID、完整摘要和最终路径。

候选默认策略：

| 文件状态 | 候选自动执行结果 |
| --- | --- |
| 签名受信、符合预期发布者、完整摘要稳定 | 允许进入用户确认后的安装启动 |
| 签名受信但发布者不符合软件预期 | 阻止；不能让“任何有效证书”替代软件身份 |
| 无签名但有厂商通过独立认证通道发布的固定摘要 | 是否允许管理员自动执行由维护者逐软件决定；建议默认外部交接 |
| 无签名且没有独立预期摘要 | 不允许工作台自动管理员执行；只允许外部交接 |
| 签名无效、明确吊销、摘要不匹配或文件在验证后变化 | 无条件阻止，不提供“仍然执行” |
| 断网导致吊销状态不可取得 | 与“已吊销”和“完整受信”分开；离线资源的接受条件由维护者决定并固定测试 |
| 在线安装器 | 只证明引导程序；必须说明后续载荷由厂商机制处理，工作台未作端到端验证 |

预期发布者政策不能由手动导入目录用自由文本降低。可行的候选形式是项目内置稳定策略标识或经过发布审核的数据；证书轮换需要明确迁移和双证书过渡，而不是遇到不匹配就静默学习新发布者。

### 5.4 子进程启动门禁

- 使用绝对、规范化且已经验证的可执行文件路径作为 `lpApplicationName`，不靠命令行首 token、当前目录、`PATH` 或文件关联猜测目标。
- 参数只能来自项目内置受控安装档案的类型化字段；对 Windows 命令行转义做集中实现和测试，不拼接用户字符串。
- 默认 `bInheritHandles = FALSE`；确需继承时只用明确句柄清单。缓存、日志、令牌、锁和恢复记录句柄永不继承。
- 为子进程提供受控当前目录和环境，不把可写下载目录、工作台目录或用户提供目录放入 DLL/可执行搜索路径。
- 不设置 `SEE_MASK_NOZONECHECKS`。若格式必须通过 Shell 启动，仍保留 Attachment Services/区域策略并取得可观察的进程或结果。
- 在启动前再次显示软件、版本、架构、最终来源、签名发布者、在线/完整安装器和将获得管理员权限这一事实；“未知”不得显示成成功。
- 在线安装器和会再启动子进程的安装器需要单独的结果检测合同；仅观察第一进程退出不能证明整棵安装流程完成。

### 5.5 U 盘与厂商驱动交接

维持 ADR-0009 时，建议只允许以下外部交接：

- 工作台显示硬件概览、厂商页面、驱动助手或 U 盘位置，并保存“驱动交接中”。
- 用户或厂商程序在工作台之外选择和安装具体驱动。
- 工作台不接收 INF/EXE 路径、不验证后自动安装、不把外部文件放入软件缓存，也不记录成“工作台安装成功”。

如果维护者选择让工作台从 U 盘导入或安装具体驱动，必须新建取代 ADR-0009 的 ADR，并单独研究驱动包目录签名、PnP/INF 安装、发布者政策、回退、BitLocker/设备控制和重启恢复。本文关于普通安装器的结论不足以批准驱动安装能力。

### 5.6 自更新真实性与回退

当前 ADR-0005、ADR-0006 与 `UPD-01` 至 `UPD-04` 的组合没有独立真实性根。HTTPS 下载 GitHub 资产只证明与当次 HTTPS 端点的受保护连接；如果发行账号、发布资产或重定向链提供了另一份未签名字节，应用没有项目签名、独立摘要或源码关联可用于拒绝它。[S05][S24]

维护者需要在以下互斥方向中选择，不应由实现者自行混搭：

1. **签名应用与更新制品**：使用受信代码签名或 Store/MSIX，更新前验证签名、发布者、版本和完整摘要；这会取代 ADR-0005。
2. **应用二进制仍不签名，但建立签名更新清单**：离线保管项目更新公钥，签名清单绑定版本、架构、制品摘要、大小、最低版本和过期时间；这会取代“无项目校验信息”的部分决定，且必须另做密钥轮换与撤销设计。
3. **保持完全无签名、无独立摘要**：取消工作台一键替换，只保留浏览器中的手动下载和清楚披露；这会修改 ADR-0006 与 `UPD-01`。
4. **交给受信分发平台**：使用 Store 或已签名 MSIX/App Installer 更新；这会改变 GitHub 传统制品和未签名发行约束。[S29][S31]

无论选哪条，回退都不能替代真实性验证。候选更新顺序应是：验证清单和制品、受保护暂存、再次验证同一文件对象、停止新任务、持久化恢复点、由项目维护的固定更新助手替换、启动健康确认、失败后恢复旧版本。更新清单不得携带任意命令或目标路径；下载的新“更新器”也不能作为插件直接获得管理员权限。

第二条只能建立**首次可信取得之后**的更新连续性：首次取得的未签名应用若已被替换，攻击者可以同时替换其中公钥并为后续恶意清单签名。若没有代码签名、受信平台或独立带外根校验，产品文案不得把这种连续性描述为已经证明“这是项目官方应用”。

### 5.7 控制元数据与跨类别紧急阻止门禁

建议为三类目录、应用更新与安全阻止通知定义共同的认证封装，而不是各自只检查 JSON/TOML 模式：

| 编号 | 候选门禁 | 失败结果 |
| --- | --- | --- |
| `G-META-01` | 受信 root 与在线内容分离；root 记录角色、密钥 ID、算法和阈值。首次 root 的取得方式必须与应用首次信任模型一同说明。 | 没有已认证 root 时，不宣称在线内容来自项目 |
| `G-META-02` | 每份签名元数据绑定稳定角色、目标类型、模式版本、单调版本、发布时间、到期时间、目标长度和摘要；拒绝未知关键字段、错误角色和跨类别目标。 | `控制元数据无效`，继续使用最后可信结果 |
| `G-META-03` | 设备持久保存每个安全角色的最高已接受版本及到期状态；目录普通回退、手动导入、日志清理、应用回退和卸载保留不得降低安全版本。 | 较旧或混搭版本失败关闭 |
| `G-META-04` | timestamp/snapshot 或等价机制绑定当前目标集合，防止只替换某一类元数据；在线时拒绝过期，离线时明确显示最后可信取得时间并使用已确认的离线政策。 | 不把“签名正确但已经冻结”显示成最新安全检查 |
| `G-META-05` | 密钥轮换同时由旧信任根和新信任根授权，并保留阈值与恢复路径；单个在线发布密钥泄漏不能直接改写 root。 | 轮换材料不闭合时保留旧根并阻止新内容 |
| `G-META-06` | 构建和发布记录签名工具、规范字节编码、密钥保管角色、阈值签名、离线恢复及泄漏演练；验证器只接受一种明确编码，避免“显示相同、签名字节不同”。 | 发布门禁失败 |

紧急阻止通道建议独立于普通目录更新，并覆盖所有能发起高权限行为的稳定目标：

- 软件来源、预期发布者、精确安装包摘要与安装档案；
- 系统设置项、系统优化方案、软件优化方案和选项；
- 内置受控能力、页面解析能力、UI Automation 档案和托管来源适配器；
- 工作台版本、更新清单版本与已知不安全的状态模式组合。

通知应声明稳定目标类型/标识、受影响版本范围、阻止原因、通知版本、到期与“阻止/解除”状态。到期只表示无法证明信息仍是最新，不能把已知“阻止”自动转成“允许”；只有经过认证的更高版本“解除”才能调整已知阻止。阻止只影响尚未开始的新修改；正在运行的外部安装器不能假装已经可安全撤回，必须按明确的停止/等待/隔离合同处理。已完成结果、历史与恢复证据不删除。设备没有本地安全通知或在线检查失败时，界面只能显示“最新状态未知”，不能显示“没有风险”。

### 5.8 UI Automation、强制终止与托管来源门禁

**UI Automation 档案**至少绑定：安装器稳定标识、受支持版本/架构、启动时已经验证的映像身份、捕获的根进程句柄与创建时间、允许的进程树规则、窗口所属 PID/会话/桌面、顶层窗口类与可观察页面身份，以及每一步所需的 control type、pattern 和多属性约束。可见文本只可作为带语言版本的辅助证据；`AutomationId`、runtime ID、窗口标题或前台状态均不能单独授权操作。出现多个候选、未知窗口、进程身份变化、意外提权边界或页面结构不匹配时立即暂停并转入如实的手动应急路径，不尝试“最像的”控件。

**强制关闭与强制终止**必须区分两类对象：

- 工作台启动的安装器/受控步骤：启动时保留进程句柄、PID、创建时间、最终映像路径和允许的后代关系；终止前用句柄重新读取身份。能够证明属于同一 job 的进程可按 job 合同处理，否则逐个验证，不从父进程名推导整棵树。
- 用户此前已经运行的目标软件：由固定检测档案限定安装位置、发布者、会话、窗口所有者和预期进程集合；无法排除同名无关进程时不得提供自动强制关闭，只能让用户自行正常退出。

严禁按映像名全局杀进程、用晚到的 PID 重新打开后直接终止、把当前前台窗口视为目标，或把杀掉一个引导进程记录为整棵安装流程已终止。终止后等待句柄进入信号状态并重新检测实际系统/软件状态；退出码、进程消失和安装成功是三个不同事实。

**托管安装来源**必须由目录审核数据或项目内置策略提供稳定包标识和允许来源：`winget` 至少使用 exact package ID、明确 source、架构/scope/version 约束，并通过受控 API 或已验证的官方调用目标执行；Store 使用精确 Product ID。不得用显示名模糊搜索后选择第一项，不得从可写 `PATH` 猜 `winget.exe`，也不得把托管工具返回“命令成功”直接等同于目标软件已经安装；仍须按目录中的独立结果检测规则验证。

## 6. 与现行文档的只读冲突映射

| 现行文档 | 当前语义 | 研究发现或张力 | 需要的动作类型 |
| --- | --- | --- | --- |
| [ADR-0001](../adr/0001-elevated-session.md) / `SEC-01` | 整个会话常驻管理员 | 网络、HTML、文件和证书解析缺陷均发生在高完整性进程；子安装器继承高权限 | 维护者确认是否接受；若保留，威胁模型和安全实机测试必须成为门禁 |
| [ADR-0005](../adr/0005-unsigned-releases.md) / `SEC-04..06` | 应用长期不签名，也不提供 SHA-256 或源码关联 | 无法建立项目发行者和更新制品的独立真实性；SmartScreen 每个未签名版本重新积累信誉，企业可阻止 | P0 产品决定；不能只补文案 |
| [ADR-0006](../adr/0006-opt-in-self-update.md) / `UPD-01..04` | 一键下载未签名、不可由项目信息校验的新版并自动替换 | 与可信更新链直接冲突；回退只能处理失败，不能识别恶意成功启动 | 选择第 5.6 节的一条方向并用 ADR 取代旧决定 |
| [ADR-0011](../adr/0011-opt-in-catalog-package-updates.md) / `CAT-01..28` | 三类目录主动更新、独立版本、回退和手动导入 | 只定义生命周期与模式校验，没有真实性、到期、防回滚、防冻结、防混搭或密钥轮换；目录会决定管理员行为 | 先决定统一控制元数据合同；普通内容回退不得回退安全版本 |
| [ADR-0009](../adr/0009-driver-acquisition-boundary.md) / `DRV-01..15` | 工作台不匹配、下载或安装具体驱动 | “从 U 盘导入厂商网卡驱动”若包含文件选择、验证或安装，会越过现有边界 | 明确是外部交接还是新受控能力；后者另立 ADR |
| [ADR-0010](../adr/0010-per-software-install-source-priority.md) / `SRC-07` | 不执行来源审查、签名验证或哈希校验 | 与自动管理员执行的真实性门禁直接冲突；用户选来源不能授权任意字节提权 | P0 产品决定；建议把来源选择与最低安全门禁分开 |
| [ADR-0013](../adr/0013-software-package-cache-retention.md) / `CACHE-08..23` | 缓存可在本机、网络共享和可移动位置直接复用 | 不同卷可能没有 ACL、命名流或可靠区域证据，网络共享不受 SmartScreen 保护 | 明确外置位置只作容量层，执行前回到受保护本机暂存 |
| [ADR-0027](../adr/0027-modular-monolith-and-controlled-extension-seams.md) / `SEC-07` | 普通目录是数据，新增高权能力必须内置 | 支持全局安全底线；但目录 URL、重定向域和发布者字段不能变成绕过内置能力的自由输入 | 把策略放入核心与受控能力，适配器只返回事实 |
| [ADR-0025](../adr/0025-emergency-notices-separate-from-catalog-updates.md) / `OPT-40..46` | 只有软件优化拥有独立紧急撤回通知 | 软件来源、安装档案、系统设置、其他受控能力和工作台版本出现严重风险时没有同等级止损通道 | 决定是否建立跨类别安全阻止元数据；不能等待普通目录更新 |
| `SRC-27` / `SRC-32` | 普通来源接受 HTTP 或 HTTPS | HTTP 在没有独立制品认证时不能保护下载字节免受网络替换 | 决定是否把可执行取得链收紧为 HTTPS-only |
| `SRC-28` / `SRC-35` | 自动跟随受控跳转，内置页面解析能力 | 尚未定义每跳校验、私网阻止、大小/时间限制和跨源允许规则 | 补安全合同与负面测试，不允许目录脚本 |
| `CAT-44` | 维护者不填写最终地址、架构、包类型、静默参数等技术事实 | 当前也没有预期发布者或独立摘要的权威位置；自动推断不能可靠决定发布者政策 | 决定发布者/摘要属于目录审核数据、内置策略还是明确不支持自动执行 |
| `SW-21..33` | 工作台下载并启动交互式或静默安装器 | 启动路径、句柄、当前目录、签名状态、在线载荷边界未定义 | 补下载到启动的单一类型化用例和失败关闭状态 |
| `TECH-20` / `SW-34..38` | 受支持安装器可以由范围受限 UI Automation 自动处理 | 尚未绑定目标进程、窗口、版本、语言和页面身份；控件文本、`AutomationId` 或前台窗口不足以授权管理员自动操作 | 为每个受支持版本冻结 UIA 身份档案并做负向实机测试 |
| `SW-19` / `OPT-27` / `OPT-54` | 可强制终止安装器、强制关闭目标软件或强制终止优化 | 尚未规定进程对象、创建时间、映像身份与允许进程树，存在按名称/PID 误杀无关高权限进程的风险 | 用捕获句柄与固定检测档案定义三种不同终止合同 |
| `SRC-24..25` / `SW-33` | Store、`winget` 等托管来源调用官方机制 | 没有精确包 ID、来源 ID、调用目标身份和结果复核合同，模糊搜索或 `PATH` 解析会改变实际安装对象 | 精确标识、精确来源、受控适配器、独立安装结果检测 |
| `REL-08` / 首版完成定义 | 可在无真实 Windows 证据时发布并标记未实机验证 | SmartScreen、吊销、MOTW、FAT/SMB、UAC 和竞态不能完全由无界面测试证明 | 维护者决定哪些安全场景是首个公开 Release 的阻断证据 |

主规格引用：[Windows 初装工作台首版规格](../../.scratch/windows-initial-setup-workbench/spec.md)。架构引用：[架构与代码质量规范](../engineering/architecture-and-code-quality.md)。

## 7. 建议的架构归属

本节只把候选门禁放回现有模块边界，不决定是否采纳门禁。

- **可移植领域模块**：拥有 URL/来源策略值类型、重定向判定、安装器身份状态、发布者匹配、摘要匹配、签名控制元数据版本/角色/到期状态、允许/阻止结论和用户确认前提；不访问 WinHTTP、证书库或文件系统。
- **应用核心**：拥有“解析来源 -> 下载 -> 完成暂存 -> 身份验证 -> 用户确认 -> 启动 -> 结果检测”的唯一用例与状态转换，以及“验证控制元数据 -> 防回滚/到期判断 -> 计算安全阻止结果”的唯一控制面；任一步失败都不能由界面或适配器改写为成功。
- **基础设施适配器**：执行 WinHTTP 请求、逐跳返回状态和连接事实，按句柄写文件、计算摘要、读取卷能力、提供文件 ID/最终路径；不自行决定某域或签名是否可接受。
- **Windows 适配器**：执行 Authenticode、Attachment Services、签名原语、进程创建、句柄/Job 管理、窗口到进程归属、范围受限 UI Automation、托管来源调用和安装结果观测；返回类型化原始事实，不把“WinVerifyTrust 返回零”“找到控件”“命令退出零”直接等同于业务身份或成功。
- **WinUI 3 适配器**：展示来源链、发布者、验证缺口和确认，不解析 URL、不构造命令行、不提供绕过按钮。
- **唯一装配入口**：注入正式网络、文件、证书、Attachment Services 和进程适配器；普通目录及调试编辑器不能登记新的下载协议、命令或验证跳过策略。

建议新增一个深的“外部制品取得与启动”能力，而不是让下载器、缓存、证书检查和进程启动成为页面可以自由拼装的四个公共工具。其接口应接收受约束的来源快照和安装档案，返回可持久化的逐阶段结果；这样才能保证同一业务规则只有一个所有者。

## 8. 维护者必须决定的问题

以下问题均未由本文回答：

本节使用研究内稳定编号；晨间总决策树的映射如下，不另起一套产品问答：

| 研究编号 | 晨间总树 | 主题 |
| --- | --- | --- |
| `SEC-D3` | `Q4a` | 一键自更新首次信任根 |
| `SEC-D1` | `Q4b` | 第三方安装器最低真实性 |
| `SEC-D2` | `Q4c` | HTTP 最终取得边界 |
| `SEC-D4` | `Q4d` | 离线吊销未知政策 |
| `SEC-D5` | `Q4e` | 外置缓存容量层语义 |
| `SEC-D7` | `Q4f` | 常驻管理员或窄特权代理 |
| `SEC-D9` | `Q4g` | 控制元数据真实性 |
| `SEC-D10` | `Q4h` | 跨类别紧急安全阻止 |
| `SEC-D6` | `Q3` | U 盘驱动交接/安装边界 |
| `SEC-D8` | `Q8` 的后续证据细目 | 公开 Release 的真实 Windows 安全矩阵 |

1. **SEC-D1：第三方安装器最低真实性门禁**。是否要求“受信签名且符合预期发布者”，还是允许逐软件声明无签名例外？无签名例外需要什么独立摘要和交接方式？
2. **SEC-D2：HTTP 来源**。HTTP 是否只允许打开说明页，还是仍允许成为安装文件最终取得链？建议后者不允许。
3. **SEC-D3：一键自更新信任根**。采用代码签名、签名清单加独立首次信任根、受信平台，还是取消一键替换？仅把公钥嵌入未签名应用不能证明首次项目身份。
4. **SEC-D4：吊销不可用的离线策略**。缓存或离线资源在证书链可验证但吊销状态不可取得时，是阻止、允许特定已审核快照，还是提示后外部交接？
5. **SEC-D5：外置缓存语义**。是否确认 U 盘和网络共享只作存储层，所有管理员执行都先复制到受保护本机暂存？
6. **SEC-D6：U 盘驱动语义**。是打开位置的外部交接，还是工作台实际导入/安装驱动？该题映射晨间全局 Q3，不另起一套产品问题。
7. **SEC-D7：常驻管理员架构**。是否接受所有网络和文件解析位于高完整性进程；若不接受，需重新评审 ADR-0001/0027，设计非提权前端与窄特权代理。
8. **SEC-D8：公开发布证据**。哪些 URL、证书、MOTW、缓存文件系统、竞态和更新失败场景必须在真实 Windows 10/11 x64/ARM64 上通过后才能公开 Release？该题映射晨间全局 Q8。
9. **SEC-D9：控制元数据真实性**。三类目录、应用更新和紧急安全通知是否采用统一的签名角色、防回滚/冻结/混搭、到期和密钥轮换合同？建议采用；若不采用，在线目录与安全通知不能授权新的管理员行为。
10. **SEC-D10：紧急安全阻止范围**。是否把现有软件优化撤回扩展为跨类别安全阻止通道，覆盖软件来源/发布者/安装档案、系统设置、受控能力和工作台版本？建议扩展，并使安全阻止版本独立于普通目录回退。

建议最短决策顺序是 `SEC-D3 -> SEC-D9 -> SEC-D10 -> SEC-D1 -> SEC-D2/SEC-D4 -> SEC-D5/SEC-D6 -> SEC-D7 -> SEC-D8`。`SEC-D3` 与 `SEC-D9` 决定控制面是否存在可信链，`SEC-D10` 决定发现严重风险后能否及时失败关闭；其余门禁才能据此收敛。UI Automation 目标身份、进程句柄/创建时间和托管来源精确标识应作为实现硬门禁；若要放宽，必须另做显式风险决定，不能由安装档案或页面静默降低。

## 9. 建议验证矩阵

### 9.1 无界面自动检查

- URL：空 host、嵌入凭据、大小写、默认端口、相对 `Location`、百分号编码、IPv6 方括号、IPv4-mapped IPv6、控制字符、片段、超长 URL 和格式错误 URL。
- 重定向：HTTPS -> HTTP、跨域、环、超过上限、跳到 loopback/private/link-local/special-use、标头与 Cookie 不跨源泄漏。
- 下载：缺少/伪造 `Content-Length`、响应过大、HTML 冒充 EXE、中断、续传资源变化、磁盘耗尽、存储拔出和并发实例。
- 文件：父目录和文件重解析点、路径在检查后替换、文件 ID 改变、完成标记先于数据、缓存摘要不匹配、另一个实例替换文件。
- Authenticode：有效且预期、有效但错误发布者、无签名、损坏签名、过期、吊销、断网吊销未知、多签名、目录签名及不支持格式。
- 启动：带空格路径、恶意 `Program.exe`、参数引号、继承句柄、恶意当前目录 DLL、文件在验证到启动之间替换。
- 更新：错误版本/架构、降级、损坏资产、签名或摘要不匹配、替换失败、新版健康检查失败、回退失败和断电中断。
- 控制元数据：错误角色/目标类型、签名不足阈值、未知根、旧合法签名重放、过期冻结、snapshot/targets 混搭、目标长度/摘要不符、旧/新 root 轮换阈值不足和普通目录回退企图降低安全版本。
- 安全阻止：每类稳定目标、版本范围边界、阻止/解除乱序、离线无本地通知、活动批次尚未开始步骤、目录回退/日志清理/应用回退后仍保持阻止。
- UI Automation：同名窗口、错误 PID、错误版本/语言、重复或缺失 `AutomationId`、runtime ID 复用、多个候选元素、意外对话框、页面跳转、目标进程退出后窗口句柄复用，全部失败关闭。
- 进程终止：PID 复用、同名无关进程、映像路径变化、不同创建时间、引导进程退出而后代继续、job breakaway、跨用户/会话目标和终止后结果未知。
- 托管来源：同名多个包、错误 source、package ID 前缀碰撞、错误架构/scope/version、伪造 `PATH` 调用目标、工具退出零但目标检测失败。

### 9.2 真实 Windows 阻断候选

- Windows 10 22H2 与 Windows 11 上未签名工作台和未签名测试安装器的 UAC、SmartScreen 与企业“禁止绕过”策略。
- NTFS、FAT32/exFAT U 盘和 SMB 共享上的 ACL、命名流、Attachment Services、直接启动与复制后启动差异。
- 在线与完全断网时的 Authenticode 链、时间戳和吊销状态。
- x64 与 ARM64 上的完整下载、验证、交互式/静默启动、停止、异常退出和结果检测。
- 可重复竞态工具持续替换路径、重解析点和文件时，工作台始终失败关闭且从不启动错误对象。
- 自更新在进程占用、存储空间不足、杀进程、断电模拟和旧版回退下保持恢复记录不变。
- 签名 root 初始化、阈值轮换、设备长期离线后元数据过期、旧合法镜像重放与紧急安全阻止在目录/应用回退后的保留。
- 每个首版必达 UI Automation 档案在声明支持的安装器版本、Windows 语言、缩放、x64/ARM64 和意外窗口注入下只操作目标进程的目标控件。
- 强制关闭/终止在安装器引导进程、后代进程、同名无关进程、PID 复用和不同 Windows 用户会话下不误杀，并如实进入结果待确认状态。
- Store/`winget` 在多个相似包、源缺失、源协议确认、架构回退和托管工具更新后仍选择精确目标。

测试记录必须区分“API 合同模拟通过”“真实平台行为通过”和“未验证”。SmartScreen 没有显示提示不能作为文件安全或发布者匹配的成功断言。

## 10. 官方资料不能直接证明的部分

本次未找到或官方资料没有承诺以下结论，后续不能把它们写成事实：

- `CreateProcessW` 直接启动、`ShellExecuteEx` 启动以及不同 Windows 策略下，SmartScreen UI 一定以同样方式出现。
- `IAttachmentExecute::Save` 在每个杀毒产品、每种卷、每个网络共享或可移动介质上都会扫描或写入 MOTW；官方措辞是“may”。[S20]
- `WinVerifyTrust` 对项目将遇到的每种 EXE、MSI、MSIX、CAB、ZIP、自解压包和厂商自定义格式都能给出同等发布者保证。
- 只调用某一个 Win32 API 就能跨 NTFS、ReFS、FAT/exFAT 和 SMB 消除全部重解析点、硬链接和 TOCTOU 风险。
- GitHub Release 的 HTTPS、文件名、MIME 或 Release 页面本身能替代项目签名、独立摘要或源码构建关联。
- 嵌入未签名应用的更新公钥能证明首次取得的应用、公钥或项目网站属于本项目；它只能在首次信任另有依据时证明后续连续性。
- 单个签名字段能同时抵御合法旧版本重放、冻结、目标混搭、根密钥泄漏和不安全密钥轮换；这些需要带状态的元数据协议。
- 任一具体第三方软件当前使用的签名发布者、证书轮换方式、官方下载域和 CDN 允许清单；这些需要逐软件维护证据。
- 在线安装器后续下载内容的真实性、静默参数和退出码语义；必须由厂商文档和真实 Windows 测试分别证明。
- `AutomationId`、runtime ID、窗口标题、前台窗口、进程名或 PID 中任一单项能跨版本、语言、进程退出和恶意同名对象唯一识别管理员操作目标。
- `winget`/Store 的显示名称、模糊搜索第一项或从 `PATH` 找到的命令能证明精确包、来源与官方调用目标身份。

## 11. 一手来源

所有来源访问日期均为 **2026-08-10**。

- `[S01]` Microsoft Learn，[How User Account Control works](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/user-account-control/how-it-works)。
- `[S02]` Microsoft Learn，[WinHttpCrackUrl function (winhttp.h)](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpcrackurl)。
- `[S03]` Microsoft Learn，[Option flags (Winhttp.h)](https://learn.microsoft.com/en-us/windows/win32/winhttp/option-flags)。
- `[S04]` RFC Editor，[RFC 9110: HTTP Semantics](https://www.rfc-editor.org/rfc/rfc9110.html)。
- `[S05]` RFC Editor，[RFC 8446: The Transport Layer Security (TLS) Protocol Version 1.3](https://www.rfc-editor.org/rfc/rfc8446.html)。
- `[S06]` IANA，[IANA IPv4 Special-Purpose Address Registry](https://www.iana.org/assignments/iana-ipv4-special-registry/iana-ipv4-special-registry.xhtml)。
- `[S07]` IANA，[IANA IPv6 Special-Purpose Address Registry](https://www.iana.org/assignments/iana-ipv6-special-registry/iana-ipv6-special-registry.xhtml)。
- `[S08]` Microsoft Learn，[WINHTTP_CONNECTION_INFO structure (winhttp.h)](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/ns-winhttp-winhttp_connection_info)。
- `[S09]` Microsoft Learn，[CreateFileW function (fileapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)。
- `[S10]` Microsoft Learn，[Reparse Points and File Operations](https://learn.microsoft.com/en-us/windows/win32/fileio/reparse-points-and-file-operations)。
- `[S11]` Microsoft Learn，[GetFinalPathNameByHandleW function (fileapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlew)。
- `[S12]` Microsoft Learn，[GetVolumeInformationW function (fileapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getvolumeinformationw)。
- `[S13]` Microsoft Learn，[File Security and Access Rights](https://learn.microsoft.com/en-us/windows/win32/fileio/file-security-and-access-rights)。
- `[S14]` Microsoft Learn，[ReplaceFileW function (winbase.h)](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)。
- `[S15]` Microsoft Learn，[WinVerifyTrust function (wintrust.h)](https://learn.microsoft.com/en-us/windows/win32/api/wintrust/nf-wintrust-winverifytrust)。
- `[S16]` Microsoft Learn，[Example C Program: Verifying the Signature of a PE File](https://learn.microsoft.com/en-us/windows/win32/seccrypto/example-c-program--verifying-the-signature-of-a-pe-file)。
- `[S17]` Microsoft Learn，[WINTRUST_DATA structure (wintrust.h)](https://learn.microsoft.com/en-us/windows/win32/api/wintrust/ns-wintrust-wintrust_data)。
- `[S18]` Microsoft Learn，[Authenticode digital signatures](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/authenticode)。
- `[S19]` Microsoft Learn，[IAttachmentExecute interface (shobjidl_core.h)](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iattachmentexecute)。
- `[S20]` Microsoft Learn，[IAttachmentExecute::Save method (shobjidl_core.h)](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iattachmentexecute-save)。
- `[S21]` Microsoft Learn，[IAttachmentExecute::SetSource method (shobjidl_core.h)](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iattachmentexecute-setsource)。
- `[S22]` Microsoft Learn，[IAttachmentExecute::CheckPolicy method (shobjidl_core.h)](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iattachmentexecute-checkpolicy)。
- `[S23]` Microsoft Learn，[Microsoft Defender SmartScreen overview](https://learn.microsoft.com/en-us/windows/security/operating-system-security/virus-and-threat-protection/microsoft-defender-smartscreen/)。
- `[S24]` Microsoft Learn，[SmartScreen reputation for Windows app developers](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation)。
- `[S25]` Microsoft Learn，[SHELLEXECUTEINFOW structure (shellapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-shellexecuteinfow)。
- `[S26]` Microsoft Learn，[CreateProcessW function (processthreadsapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)。
- `[S27]` Microsoft Learn，[Dynamic-Link Library Security](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-security)。
- `[S28]` Microsoft Learn，[UpdateProcThreadAttribute function (processthreadsapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute)。
- `[S29]` Microsoft Learn，[Sign an MSIX package](https://learn.microsoft.com/en-us/windows/msix/package/signing-package-overview)。
- `[S30]` Microsoft Learn，[Create a certificate for package signing](https://learn.microsoft.com/en-us/windows/msix/package/create-certificate-package-signing)。
- `[S31]` Microsoft Learn，[App Installer file update settings](https://learn.microsoft.com/en-us/windows/msix/app-installer/update-settings)。
- `[S32]` The Update Framework，[TUF specification](https://theupdateframework.github.io/specification/latest/)。
- `[S33]` The Update Framework，[Security](https://theupdateframework.io/security/)。
- `[S34]` Microsoft Learn，[BCryptVerifySignature function (bcrypt.h)](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptverifysignature)。
- `[S35]` Microsoft Learn，[UI Automation Security Overview](https://learn.microsoft.com/en-us/dotnet/framework/ui-automation/ui-automation-security-overview)。
- `[S36]` Microsoft Learn，[GetWindowThreadProcessId function (winuser.h)](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowthreadprocessid)。
- `[S37]` Microsoft Learn，[Automation Element Property IDs](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-automation-element-propids)。
- `[S38]` Microsoft Learn，[IUIAutomationElement::GetRuntimeId method (uiautomationclient.h)](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomationelement-getruntimeid)。
- `[S39]` Microsoft Learn，[Process Handles and Identifiers](https://learn.microsoft.com/en-us/windows/win32/procthread/process-handles-and-identifiers)。
- `[S40]` Microsoft Learn，[GetProcessTimes function (processthreadsapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes)。
- `[S41]` Microsoft Learn，[QueryFullProcessImageNameW function (winbase.h)](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-queryfullprocessimagenamew)。
- `[S42]` Microsoft Learn，[TerminateProcess function (processthreadsapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminateprocess)。
- `[S43]` Microsoft Learn，[Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)。
- `[S44]` Microsoft Learn，[winget install command](https://learn.microsoft.com/en-us/windows/package-manager/winget/install)。
- `[S45]` Microsoft Learn，[Launch the Microsoft Store app](https://learn.microsoft.com/en-us/windows/apps/develop/launch/launch-store-app)。

## 12. 研究状态

本文已完成官方事实、候选门禁、现行冲突、决策问题和验证矩阵整理。它不修改任何现行决定。只有维护者回答第 8 节问题，并用相应规格或 ADR 接受结果后，事项才能把这些门禁作为实现完成条件。
