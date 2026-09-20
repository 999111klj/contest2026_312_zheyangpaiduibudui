# APK 构建与部署完整流程

本文档总结从源码到可用 APK 的完整流程，所有步骤均在本机实际验证通过。

**源码工程**：`C:\Users\MI\Desktop\selfie_scorer\ai_vlog_app`

**本文件夹内也有一份完整工程**：`ai_vlog_app\`（含 `android/` 构建配置、`pubspec.yaml` 依赖、`assets/` 资源、`lib/` 源码），已验证可独立构建出 48MB 的 release APK。拿到任何一台装好环境的机器上都能直接构建。

> ⚠️ **路径必须全英文**：Android Gradle 插件拒绝在含中文的路径下构建（报错 `Your project path contains non-ASCII characters`）。本文件夹曾叫 `AI_Vlog_App_交接文档` 导致构建失败，改名为 `AI_Vlog_App` 后正常。不要把工程移回中文路径。

---

## 一、构建环境（关键，缺一不可）

| 项目 | 路径 / 版本 | 说明 |
|------|------------|------|
| Flutter | `D:\qch\flutter\flutter\bin\flutter.bat` | 版本 3.35.7 / Dart 3.9.2，**不在系统 PATH 里，必须用完整路径调用** |
| JDK 17 | `C:\Program Files\Microsoft\jdk-17.0.20.101-hotspot` | **必须手动设置 JAVA_HOME 指向它**。系统默认 Java 25 与 AGP 8.7.2 不兼容，会构建失败 |
| ADB | `C:\Users\MI\AppData\Local\Android\Sdk\platform-tools\adb.exe` | 手机通过 USB 连接后使用 |
| 测试设备 | `5XY5TGFIIVJFMRIV` | 已验证的安卓真机 |

**Gradle 构建配置**（工程 `android/` 目录，已配置好无需改动）：
- AGP 8.7.2 / Kotlin 1.9.24 / Gradle 8.12
- compileSdk 36 / minSdk 24
- Kotlin 1.9.24 有弃用警告（Flutter 未来版本要求 2.1.0+），当前不影响构建

---

## 二、构建 Release APK

在 PowerShell 中执行（三条命令一次跑完）：

```powershell
cd "C:\Users\MI\Desktop\selfie_scorer\ai_vlog_app"
$env:JAVA_HOME = "C:\Program Files\Microsoft\jdk-17.0.20.101-hotspot"
& "D:\qch\flutter\flutter\bin\flutter.bat" build apk --release
```

**成功输出标志**：

```
Running Gradle task 'assembleRelease'...
√ Built build\app\outputs\flutter-apk\app-release.apk (48.0MB)
```

**产物路径**：`build\app\outputs\flutter-apk\app-release.apk`（约 48MB）

### 注意事项

1. **JAVA_HOME 必须在构建前设置**。新开的 PowerShell 窗口环境变量不保留，每次都要重新执行 `$env:JAVA_HOME = ...`
2. **flutter.bat 用完整路径**。直接敲 `flutter` 会报"不是内部或外部命令"
3. 构建过程约 1 到 3 分钟（增量构建更快）
4. 构建前可先跑代码检查：
   ```powershell
   & "D:\qch\flutter\flutter\bin\flutter.bat" analyze
   ```
   输出 `No issues found!` 即可放心构建

---

## 三、复制 APK 到桌面

```powershell
Copy-Item "C:\Users\MI\Desktop\selfie_scorer\ai_vlog_app\build\app\outputs\flutter-apk\app-release.apk" "C:\Users\MI\Desktop\AI-Vlog.apk" -Force
```

桌面的 `AI-Vlog.apk` 就是最终交付的安装包，可直接发给任何人安装。

---

## 四、安装到手机（两种方式）

### 方式 A：USB + ADB 直装（开发调试推荐）

手机插 USB，弹出的"允许 USB 调试"对话框点允许（勾选"始终允许"），然后：

```powershell
& "C:\Users\MI\AppData\Local\Android\Sdk\platform-tools\adb.exe" install -r "C:\Users\MI\Desktop\AI-Vlog.apk"
```

- `-r` 表示覆盖安装（保留应用数据）
- **成功输出**：`Performing Streamed Install` + `Success`
- 若提示 `no devices/emulators found`：检查手机 USB 是否插好、是否弹权限框
- 首次连接 ADB 守护进程会自启（提示 `daemon not running; starting now` 属正常）

### 方式 B：直接传 APK 文件

把 `AI-Vlog.apk` 通过微信/QQ/USB 传到手机，手机上点击安装（需允许"安装未知来源应用"权限）。演示或交付时用这种方式。

---

## 五、构建后联调验证流程

APK 装好后，按以下步骤验证功能（需手机与后端电脑在同一局域网）：

### 5.1 真实后端联调（当前生产路径）

1. PC 后端启动（Linux 服务器，`https://192.168.1.100`，HTTPS 443 端口）
2. 手机打开 App → 设备页 → 输入 `192.168.1.100`（不用带端口，App 自动补 `https://`）
3. 点"连接" → 应显示视频列表（带中文标题）
4. 点任意视频的下载按钮 → 进度条走完 → 提示"已下载「标题」→ 去 Vlog 库播放"
5. 切到 Vlog 库 → 刷新 → 点视频播放 → 点"保存到相册"

### 5.2 Mock 服务器联调（无真实后端时用）

```bash
python tools/mock_device_server.py          # 默认 8080 端口
```

App 设备页填 `<电脑局域网IP>:8080`。mock 脚本位于工程 `tools/mock_device_server.py`，提供 `/list` 和 `/download` 接口，已验证可用。

---

## 六、App 与后端的接口契约（构建的代码依赖这些）

APK 里的网络逻辑（`lib/services/device_client.dart`）依赖以下后端约定，**后端接口变了就需要改代码重新构建**：

| 接口 | 方法 | 鉴权 | 响应格式 |
|------|------|------|---------|
| `/list` | GET | 不需要 | `{"videos":[{"name":"x.mp4","title":"标题","url":"完整下载URL"},...]}` |
| `/download`（或 /list 返回的 url） | GET | 需要 `Authorization: Bearer <token>` | mp4 文件流 |

**代码内置关键参数**（`device_client.dart`）：
- 默认协议 `https://`（输入裸 IP 自动补全）
- 跳过自签名证书校验（`badCertificateCallback` 返回 true）
- Bearer Token 硬编码在 `_apiToken` 常量中，token 变了需改代码重新构建

---

## 七、本对话中踩过的坑（遇到构建/连接问题先查这里）

| 现象 | 原因 | 解决 |
|------|------|------|
| `flutter` 不是内部或外部命令 | flutter.bat 不在 PATH | 用完整路径 `& "D:\qch\flutter\flutter\bin\flutter.bat"` |
| 构建报 `path contains non-ASCII characters` | 项目路径含中文 | 把整个工程移到纯英文路径（如 `C:\Users\MI\Desktop\AI_Vlog_App\ai_vlog_app`） |
| Gradle 构建报 Java 版本错误 | 系统默认 Java 25，与 AGP 不兼容 | 构建前 `$env:JAVA_HOME = "C:\Program Files\Microsoft\jdk-17.0.20.101-hotspot"` |
| App 输入 IP 报"连不上" | App 默认 http 后端是 https | 已修复：代码默认 https，无需处理 |
| 能连上但列表为空 | 后端返回 `{"videos":[...]}`，App 只解析顶层数组 | 已修复：fetchList 兼容两种格式，无需处理 |
| 下载报 HTTP 401 | 后端下载接口需要 Bearer Token | 已修复：代码带 Token，token 过期需改 `_apiToken` 重新构建 |
| 下载成功但 Vlog 库看不到 | 库页面不自动刷新 | 点库页面右上角刷新按钮（已知遗留项） |

---

## 八、一键构建脚本（可选）

把以下内容存为 `build_apk.ps1` 放桌面，以后右键"使用 PowerShell 运行"即可完成构建+复制+安装：

```powershell
$ErrorActionPreference = "Stop"

$flutter = "D:\qch\flutter\flutter\bin\flutter.bat"
$adb = "C:\Users\MI\AppData\Local\Android\Sdk\platform-tools\adb.exe"
$project = "C:\Users\MI\Desktop\selfie_scorer\ai_vlog_app"
$apk = "$project\build\app\outputs\flutter-apk\app-release.apk"
$dest = "C:\Users\MI\Desktop\AI-Vlog.apk"

$env:JAVA_HOME = "C:\Program Files\Microsoft\jdk-17.0.20.101-hotspot"

Set-Location $project
Write-Host "==> 构建 Release APK..." -ForegroundColor Cyan
& $flutter build apk --release
if ($LASTEXITCODE -ne 0) { throw "构建失败" }

Write-Host "==> 复制到桌面..." -ForegroundColor Cyan
Copy-Item $apk $dest -Force
Write-Host "已生成: $dest ($([math]::Round((Get-Item $dest).Length/1MB,2)) MB)"

if (& $adb devices | Select-String "device`$") {
    Write-Host "==> 检测到手机，安装中..." -ForegroundColor Cyan
    & $adb install -r $dest
} else {
    Write-Host "未检测到手机，跳过安装（APK 已在桌面）" -ForegroundColor Yellow
}
```

---

## 九、完整流程速查（TL;DR）

```
改代码 → analyze 检查 → build apk --release → 复制到桌面 → adb install → 手机联调
         (1秒)          (1~3分钟)              (1秒)         (30秒)        (2分钟)
```

本对话最终产出的 APK 版本已包含全部三项修复（HTTPS + 列表解析 + Bearer Token），桌面的 `AI-Vlog.apk` 即为最新版，可直接使用。
