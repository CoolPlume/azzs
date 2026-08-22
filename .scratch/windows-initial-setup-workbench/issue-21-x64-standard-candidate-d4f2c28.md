# 事项 21 当前 standard x64 portable 候选

日期：2026-08-16  
事项：21 - 构建 GitHub 发行制品  
候选范围：仅当前 integration 的 standard x64 portable；这是开发者证据，不是用户发行资产或 GitHub Release 文案。

## 输入与工具链

- 源提交：`d4f2c280158da2105b246a014a644c34f637a77b`；目标分支为 `codex/issue-21-x64-standard-rc`，与 `origin/codex/v1-integration` 起点一致。
- `out/manifests/windows-x64-release.json`：`result=succeeded`、`source.commit` 为上述 SHA、`source.dirty=false`。
- 本机工具链：Visual Studio `18.8.3`，MSVC toolset `14.51.36231`，compiler `19.51.36252.0`，MSBuild `18.8.2.30814`，CMake `4.3.1-msvc1`，Windows SDK release `10.0.28000.2526` / target `10.0.28000.0`，`/std:c++latest`、`/MT`。
- `TEMP`/`TMP` 使用代理临时根下的 `issue21-x64-standard-rc` 目录；绝对临时路径不在仓库证据中展开。

## 命令与结果

```text
eng/build.ps1 -Architecture x64 -SkipCoreSmoke
ctest --test-dir out/build/windows-x64 -C Release --output-on-failure -R "^(portable\.package\.contract|windows-driver-rescue-folder\.contract)$"
eng/package-portable.ps1 -ArtifactId standard-x64-portable -SkipBuild
eng/verify-portable-package.ps1 -ArtifactId standard-x64-portable -RepositoryRoot <repo-root> -StagingDirectory out/staging/portable/standard-x64-portable -PackagePath out/packages/Azzs-standard-x64-portable.zip -ManifestPath out/manifests/package-standard-x64-portable.json
```

- x64 构建成功，0 errors、5 个既有编译警告；没有运行 core smoke 或完整 host CTest。
- focused 合同通过：`portable.package.contract` 与 `windows-driver-rescue-folder.contract` 为 2/2。
- 打包脚本内置 verifier 通过；随后独立 verifier 再次通过（输出 `Portable package contract passed: x64`）。
- 同 SHA 的 GitHub x64 jobs `31895084081`、`95037058023` 已成功，本轮未重复无关 host 全量或 CI。

## 候选统计与合同

- staging：`out/staging/portable/standard-x64-portable`，300 个文件、`116665039` bytes。
- ZIP：`out/packages/Azzs-standard-x64-portable.zip`，300 个文件、`46147415` bytes，SHA-256 `C600849B5837B344398B99A0503C2CFB0D6C42855AFAB39A80B1CF43BF7708B7`。
- package manifest：`out/manifests/package-standard-x64-portable.json`，`artifactId=standard-x64-portable`、`kind=portable`、`architecture=x64`、`sourceCommit=d4f2c280158da2105b246a014a644c34f637a77b`；`package.path` 为 `out/packages/Azzs-standard-x64-portable.zip`，非绝对路径、无反斜杠、无 `..`；`inputs=[]`。其绑定 `release/artifact-content-manifest.v1.json`，SHA-256 为 `47bd2b812868f910a360890d6c3f1aef666af50e2b5d94578318e26c3344ce5a`。
- staging 中 `rescue-tools/generic-network-driver` 与 `rescue-tools/offline-network-diagnostics` 均存在且各有 0 个子项；ZIP 中各恰有一个目录项且无子项；内置和独立 verifier 均通过这些检查。
- `release/artifact-content-manifest.v1.json` 的 `rescue-x64-portable.inputs=[]`。portable 合同中“rescue package without locked inputs”的 fail-closed 场景通过；本轮未生成 rescue ZIP，空 rescue 定义未被当作可发布内容。
- 未启动候选 `Azzs.WinUI.exe` 或包内救援 EXE；verifier 仅读取入口 PE 头完成 x64 架构检查，rescue focused 合同运行的是合同测试 harness。

## 边界

- 这是当前唯一的 x64 standard portable 候选，不是八制品或外部发布成功。ARM64、其余七种制品、WiX/MSI、安装、Repair、卸载、迁移、UAC、实机和完整安装生命周期均未执行；未创建 tag 或 GitHub Release，也未运行 main/Release 发布流程。
- 已知的两个非提升 ACL host 前置（`execution-log.contract` 的 prepared ACL root、`windows-device-data.contract` 的隔离设备/reparse 根）属于本轮未执行的完整 host 检查，未写成通过，也未为其请求 UAC 或重跑。
- 事项 21 `Resolution: open` 保持不变。
