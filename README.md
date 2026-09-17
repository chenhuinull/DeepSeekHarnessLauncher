# DeepSeek Harness 启动器

Windows 桌面启动器，提供启动/停止、检查更新、窗口始终置顶及可折叠运行日志。标题栏上的“日志”按钮控制下方日志文本框，日志默认折叠；旁边的“状态”指示当前状态。窗口可拖动、最小化和关闭。

窗口默认置顶，可通过标题栏的“关闭置顶”按钮切换。日志区采用白底深色字。

启动器打开时位于主屏幕工作区中央。

日志默认收起；启动失败时自动展开，服务启动成功时自动收起。也可以随时点击标题栏的“日志”手动切换。

鲸鱼图标旁的状态点：绿色表示已启动，黄色表示未启动，红色表示未安装 DeepSeek Harness，紫色表示未启动且有更新。悬停状态点可查看说明。

DeepSeek 鲸鱼图标下载自 [DeepSeek 官网](https://www.deepseek.com/favicon.ico)。

## 使用

发布原生 Win32 单文件版本需要 Windows 和 Visual Studio 的“使用 C++ 的桌面开发”组件。双击项目根目录的 `publish.cmd` 即可编译发布；命令行也可运行：

```powershell
.\publish.cmd --no-pause
```

首次点击“启动”会把官方 `@deepseek-ai/dsh` 安装到 `%LOCALAPPDATA%\DeepSeekHarnessLauncher\runtime`，随后运行 `dsh web`。默认浏览器会打开带访问令牌的本地地址；也可从运行日志复制完整地址。关闭启动器窗口不会停止 Web 服务；再次打开启动器会检测并接管仍在运行的服务，可通过“停止”按钮结束它。服务日志保存在 `%LOCALAPPDATA%\DeepSeekHarnessLauncher\server.log`。

“检查更新”查询 npm 最新版本；发现新版后，按钮变为“安装更新”，再次点击即可更新。更新前需停止服务。

发布结果仅有 `dist\DeepSeekHarnessLauncher.exe`（约 0.54 MiB）。目标 Windows 电脑无需安装 .NET、MFC 或 Visual C++ 运行库；启动 DeepSeek Harness 仍需 Node.js（含 npm）。
