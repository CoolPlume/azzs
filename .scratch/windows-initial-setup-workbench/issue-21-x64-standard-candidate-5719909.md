# 事项 21：standard x64 portable 候选 `5719909`

生成时间：2026-08-18  
源码提交：`5719909f3ac14997a569f4e14476a85cc5fc9a64`  
候选目录：`D:\azzs-codex\artifacts\issue21\standard-x64-portable-5719909`

## 候选内容

- ZIP：`Azzs-standard-x64-portable.zip`
- ZIP SHA-256：`E3B136DCA46091CA58FFB21CD69693DF424AB3432F505B3A3FC80F2751A78A3A`
- ZIP 大小：46,167,226 bytes；ZIP entries：304。
- staging：302 个文件，116,724,305 bytes。
- package manifest：`kind=portable`、`architecture=x64`、`artifactId=standard-x64-portable`、`sourceCommit=5719909f3ac14997a569f4e14476a85cc5fc9a64`，且 `package.path=out/packages/Azzs-standard-x64-portable.zip` 保持仓库相对。
- Release build manifest：`result=succeeded`、`source.commit=5719909f3ac14997a569f4e14476a85cc5fc9a64`、`source.dirty=false`、305 个 build artifacts。
- `rescue-tools/generic-network-driver/` 与 `rescue-tools/offline-network-diagnostics/` 在 staging 和 ZIP 中均存在且为空；ZIP 中没有 rescue 文件条目，未扫描、识别或启动任何 rescue EXE。

## 已执行验证

```powershell
python tests\ui-design-contract\check_design_system.py .
.\eng\build.ps1 -Architecture x64 -SkipCoreSmoke
& 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir out\build\windows-x64 -C Release --output-on-failure -R '^(startup-assembly\.contract|build\.toolchain\.contract|ui\.design\.contract|product\.identity\.contract|portable\.package\.contract|windows-driver-rescue-folder\.contract)$'
.\eng\package-portable.ps1 -ArtifactId standard-x64-portable -SkipBuild
.\eng\verify-portable-package.ps1 -ArtifactId standard-x64-portable -RepositoryRoot (Get-Location).Path -StagingDirectory .\out\staging\portable\standard-x64-portable -PackagePath .\out\packages\Azzs-standard-x64-portable.zip -ManifestPath .\out\manifests\package-standard-x64-portable.json
```

- `ui.design.contract` 通过。
- x64 Release 构建成功，0 errors；保留 5 个既有 C4244/C4457 warnings。
- `startup-assembly.contract`、`build.toolchain.contract`、`ui.design.contract`、`product.identity.contract` 和 `windows-driver-rescue-folder.contract` 通过。
- `portable.package.contract` 的普通 package/manifest/runtime-output 场景输出通过，但其 reparse 反例创建符号链接时因宿主返回 `Administrator privilege required` 停止；这不是 portable runtime 或候选装配失败，且未写成合同通过。
- 打包内置 verifier 与工作树内的独立 verifier 均通过。独立 verifier 明确要求路径处于源码仓库内，因此仓库外 artifact 副本不作为其直接输入；已核对源 ZIP 与 artifact ZIP 的 SHA-256 一致，staging 文件数与总字节数一致。

## 源码复核与边界

当前工程保持 `WindowsAppSDKSelfContained=true`，Release build manifest 对完整运行时输出建档，portable verifier 将 build artifacts 与 staging/ZIP 的路径、字节数和 SHA-256 绑定。`App` 构造和组合根已将可处理的装配异常投影到失败窗口或最后一级平台提示；本轮未发现可由当前源码或打包路径直接坐实的 x64 portable runtime 根因，也没有修改产品源码。

本候选仅覆盖 current standard x64 portable。ARM64、其余七制品、WiX/MSI、安装生命周期、UAC/真实窗口验收、完整 host guardrails 和 GitHub CI 均未执行。事项 21 的 `Resolution: open`、`Blocked by` 和八制品标准不变。
