# 事项 21 当前 integration standard x64 portable 候选

日期：2026-08-16  
事项：21 - 构建 GitHub 发行制品  
候选范围：仅当前 integration 的 standard x64 portable；这是开发者证据，不是用户发行资产或 GitHub Release 文案。

## 输入与基线

- 源提交：`43ed00f9c33e8977af2fe92330f93cef2585028f`；分支为 `codex/issue-21-current-x64-rc`，从该精确 `origin/codex/v1-integration` SHA 创建。
- 在验证前执行 `git diff --name-status 4552f5429deb7547fd30c640e3e9dc70da5051d2..HEAD`，唯一变更为本事项的 Markdown 证据；产品代码未变，因此只运行本候选所需的一次 x64 构建、两个 focused 合同、standard portable 打包和两个 verifier。
- `out/manifests/windows-x64-release.json`：`result=succeeded`、`source.commit=43ed00f9c33e8977af2fe92330f93cef2585028f`、`source.dirty=false`、目标为 x64/Release。
- 本机工具链：Visual Studio `18.8.3`、MSVC `14.51.36231`、compiler `19.51.36252.0`、MSBuild `18.8.2.30814`、CMake `4.3.1-msvc1`、Windows SDK release `10.0.28000.2526` / target `10.0.28000.0`、`/std:c++latest`、`/MT`。

## 已执行的最小验证

```text
eng/build.ps1 -Architecture x64 -SkipCoreSmoke
<Visual Studio bundled ctest.exe> --test-dir out/build/windows-x64 -C Release --output-on-failure -R "^(portable\.package\.contract|windows-driver-rescue-folder\.contract)$"
eng/package-portable.ps1 -ArtifactId standard-x64-portable -SkipBuild
eng/verify-portable-package.ps1 -ArtifactId standard-x64-portable -RepositoryRoot <repo-root> -StagingDirectory out/staging/portable/standard-x64-portable -PackagePath out/packages/Azzs-standard-x64-portable.zip -ManifestPath out/manifests/package-standard-x64-portable.json
```

- x64 Release 构建成功。`-SkipCoreSmoke` 按命令跳过 core smoke；未运行完整 host CTest。
- `portable.package.contract` 与 `windows-driver-rescue-folder.contract` 为 2/2 通过。当前 shell 的 `PATH` 未提供 `ctest`，该命令在测试启动前失败；随后使用同一 Visual Studio CMake 安装目录中的 `ctest.exe` 执行上述唯一一次实际合同运行。
- standard portable 打包成功，打包脚本的内置 verifier 输出 `Portable package contract passed: x64`；随后独立 verifier 再次输出相同通过结果。

## 候选制品与内容边界

- 保留副本目录：`D:\azzs-codex\artifacts\issue21\standard-x64-portable-43ed00f`。
- staging：`staging`，300 个文件、`116665715` bytes。
- ZIP：`Azzs-standard-x64-portable.zip`，300 个文件、`46147634` bytes，SHA-256 `DA1B97D5A97CA21E0B7AF033FDBBB82BE85AB35CDB07E99F6F9B264731387DDD`。
- `package-standard-x64-portable.json`：`artifactId=standard-x64-portable`、`kind=portable`、`architecture=x64`、`sourceCommit=43ed00f9c33e8977af2fe92330f93cef2585028f`、`inputs=[]`。`package.path=out/packages/Azzs-standard-x64-portable.zip` 是 canonical 仓库相对路径，不是绝对工作区路径或 G 盘路径。
- `Azzs.WinUI.exe` 所在 staging 根目录含 `rescue-tools/generic-network-driver/` 与 `rescue-tools/offline-network-diagnostics/` 两个固定槽位；二者各有 0 个子项。ZIP 中各有对应目录项且没有子项，staging/ZIP 均无 rescue EXE 或其他救援随包内容。
- `release/artifact-content-manifest.v1.json` 中 `rescue-x64-portable.inputs=[]` 与 `large-offline-x64-portable.inputs=[]`；通过的 `portable.package.contract` 保留“rescue package without locked inputs”和 large-offline rescue-superset 等 fail-closed 反例。本轮没有生成 rescue 或 large-offline 候选，空槽位不等于完成其制品。

## 未完成边界

- 这是当前唯一的 standard x64 portable 候选，不是八制品、完整 rescue/large-offline 制品或外部发布成功。
- ARM64、其余七制品、WiX/MSI、安装、Repair、卸载、迁移、安装生命周期、真实 UAC 与实机验证均未执行；未创建 tag、GitHub Release 或上传发行资产。
- 事项 21 的 `Resolution: open` 保持不变。
