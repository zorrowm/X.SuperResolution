# X.SuperResolution Windows 安装包

> 更新时间：2026-09-14

NSIS 负责文件安装、快捷方式、注册和卸载，原生 C++ 插件负责完整的无边框界面。安装器采用与 XXray 相同的石墨／雾白配色、圆角、按钮尺寸与步骤布局；语义颜色从本项目的 `WorkbenchColors.axaml` 生成。

## 构建

在仓库根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-installer.ps1
```

需要 Windows x64、.NET 10 SDK、NSIS 3 Unicode、Visual Studio / Build Tools 的“使用 C++ 的桌面开发”工作负载及 Windows SDK。脚本自动定位 MSVC、SDK、NSIS 和可再分发的 x64 `vcomp140.dll`。单独构建安装包不需要 7-Zip；生成 Full/Thin 压缩包时需要 `7z`。

默认发布自包含单文件客户端，包含 .NET 运行时、托管程序集、NCNN 和 UI 原生依赖；原生依赖由 .NET 在运行时解出。模型目录与 `vcomp140.dll` 保留在程序旁。安装界面静态链接 C++ 运行库，不依赖 WebView2。目标电脑仍需支持 Vulkan 的显卡驱动。

| 产物 | 路径 |
| --- | --- |
| Full 发布目录 | `artifacts/publish/win-x64-full/` |
| Thin 发布目录 | `artifacts/publish/win-x64-thin/` |
| 安装包 | `artifacts/installer/X.SuperResolution-Setup-<版本>-win-x64.exe` |
| 校验文件 | 安装包旁的 `.exe.sha256` |
| 自动生成内容 | `installer/generated/`，已加入 Git 忽略 |

复用已有的 Full 单文件发布目录：

```powershell
.\scripts\build-installer.ps1 -SkipPublish
.\scripts\build-installer.ps1 -SkipFullPublish
.\scripts\build-installer.ps1 -SkipPublish -PublishDir 'D:\Release\SuperResolution'
.\scripts\build-installer.ps1 -MakeNsisPath 'D:\Tools\NSIS\makensis.exe'
```

`-SkipFullPublish` 是 `-SkipPublish` 的兼容别名。指定自定义发布目录时需要同时使用 `-SkipPublish`。直接编译同样会重新生成元数据、清单和原生 DLL：

```powershell
makensis .\installer\X.SuperResolution.nsi
makensis /DPUBLISH_DIR=D:\Release\SuperResolution .\installer\X.SuperResolution.nsi
```

## 自动生成内容

| 文件 | 用途 |
| --- | --- |
| `Generated.AppInfo.nsh` | 读取求值后的 MSBuild 属性和 EXE 实际四段版本、公司、版权、图标及输出路径 |
| `Generated.Payload.nsh` | 枚举发布文件，生成安装、按字节更新进度、精确卸载及空目录清理宏 |
| `payload-manifest.txt` | 记录安装器拥有的文件，供后续升级清理过期文件 |
| `Generated.Theme.h` | 从 `WorkbenchColors.axaml` 生成两套主题的 23 个语义颜色 |
| `build-info.json` | 记录确切安装包路径、版本、单文件／自包含状态、文件数量与大小 |

版本使用发布 EXE 的真实版本，支持 `AssemblyVersion=1.0.*`。打包读取 .NET bundle 清单及内嵌运行配置，检查 x64、自包含、单文件、NCNN 原生库、OpenMP 运行库与模型，不依赖外置 `runtimeconfig.json`。失败会中止打包，避免误用旧 `.nsh` 或 Thin 产物。

## 界面与安装行为

- 无系统标题栏，可拖动、最小化、关闭和切换主题。安装、进度、完成、错误及卸载均使用相同的界面。
- 窗口与按钮使用内存缓冲绘制；悬停只在进入和离开时更新，静态内容缓存，进度由独立线程每 40 ms 合并读取，避免阻塞文件解压。
- 目录输入框的正常、只读与禁用状态均采用当前主题背景。支持高 DPI、键盘操作和减少动画设置，最小化时暂停装饰动画。
- 默认安装到 `%LOCALAPPDATA%\Programs\X.SuperResolution`，仅注册当前用户，无需管理员权限，可选择桌面快捷方式。
- 使用与应用相同的互斥量 `X.Lucifer.SuperResolution`，运行中的客户端或正在写入的另一个安装器会阻止安装／卸载。
- 原位升级保留原安装位置，依据旧清单清理过期文件。旧安装器没有清单时，只认可已注册且包含主程序与卸载程序的目录，覆盖应用文件后建立新清单。
- 卸载只删除清单内的程序、模型和运行库，清理快捷方式及注册信息；保留原图、`output/`、`settings.json` 和其他用户文件，根目录仅在为空时删除。

## Full / Thin 发布

```powershell
.\scripts\publish-win-x64-full.ps1
.\scripts\publish-win-x64-thin.ps1
# 仅发布，不创建 7z
.\scripts\publish-win-x64-full.ps1 -SkipArchive
```

两种模式均为单文件客户端，使用独立输出目录；Full 包含运行时与模型，Thin 需要 .NET 10 x64 Runtime 和所需模型。发布先写入新目录并验证 bundle，成功后将旧目录改名为 `.previous-<ID>` 备份。程序集不裁剪，EXE 内部不重复压缩。

## 静默安装与卸载

```powershell
& '.\X.SuperResolution-Setup-<版本>-win-x64.exe' /S /D=D:\Apps\X.SuperResolution
& 'D:\Apps\X.SuperResolution\Uninstall.exe' /S
```

`/S` 区分大小写；`/D` 放在最后，目录参数不加引号。静默安装不启动应用。退出码：`0` 成功，`1602` 取消，`1603` 路径、文件或占用错误，`1633` 系统不支持。

## 界面预览

生成元数据后可编译同源预览程序：

```powershell
.\installer\Build-NsisSkin.ps1 -Preview
.\installer\generated\InstallerSkinPreview.exe
.\installer\generated\InstallerSkinPreview.exe --render .\artifacts\installer-preview
```

预览模式不安装、注册或卸载应用，使用演示数据展示交互；`--render` 输出两套主题的各步骤与 100% / 150% 缩放图像。安装包未配置数字签名，正式发布可使用发布方证书签名。
