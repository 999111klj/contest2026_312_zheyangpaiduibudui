# ============================================================
#  AI Vlog App - 一键构建 APK
#  用法：双击 build_apk.bat，或右键本文件 →“使用 PowerShell 运行”
#
#  换新电脑：装好 Flutter 3.35.x + JDK 17 + Android SDK 后，
#  把整个 AI_Vlog_App 文件夹拷过去即可。本脚本不含任何写死的
#  机器路径；若 Flutter/JDK 不在 PATH 和常见位置，在下方配置区填一下。
# ============================================================

$ErrorActionPreference = "Stop"

# ---------- 配置区（换新电脑时按需修改，留空 = 自动探测） ----------
$FlutterRoot = ""    # Flutter SDK 根目录（其下有 bin\flutter.bat）
$Jdk17Home   = ""    # JDK 17 根目录（其下有 bin\java.exe）
# ---------------------------------------------------------------

# 工程与产物路径：全部基于脚本所在文件夹定位，随文件夹移动
$ProjectDir = Join-Path $PSScriptRoot "ai_vlog_app"
$ApkPath    = Join-Path $ProjectDir "build\app\outputs\flutter-apk\app-release.apk"
$DestApk    = Join-Path $PSScriptRoot "AI-Vlog.apk"

# Android Gradle 插件拒绝在含中文的路径下构建
if ($PSScriptRoot -match '[^\x00-\x7F]') {
    throw "当前文件夹路径含中文/非ASCII字符，Gradle 无法构建。请把整个文件夹移到纯英文路径。"
}
if (-not (Test-Path (Join-Path $ProjectDir "pubspec.yaml"))) {
    throw "找不到 Flutter 工程：$ProjectDir （本脚本必须放在 AI_Vlog_App 文件夹根目录）"
}

# --- 1. 定位 flutter.bat ---
if ($FlutterRoot -ne "") {
    $flutterBat = Join-Path $FlutterRoot "bin\flutter.bat"
    if (-not (Test-Path $flutterBat)) { throw "配置区 FlutterRoot 无效：找不到 $flutterBat" }
} else {
    $cmd = Get-Command flutter -ErrorAction SilentlyContinue
    if (-not $cmd) { throw "找不到 flutter 命令。请把 Flutter 的 bin 目录加入 PATH，或在脚本顶部 FlutterRoot 填入 SDK 路径" }
    $flutterBat = $cmd.Source
}

# --- 2. 定位 JDK 17 并设置 JAVA_HOME（必须 JDK 17，其他版本与 AGP 8.7.2 不兼容） ---
if ($Jdk17Home -ne "") {
    if (-not (Test-Path (Join-Path $Jdk17Home "bin\java.exe"))) { throw "配置区 Jdk17Home 无效：找不到 java.exe" }
    $env:JAVA_HOME = $Jdk17Home
} else {
    # 先扫描常见 JDK 17 安装位置（保证版本正确）
    $candidates = @(
        "C:\Program Files\Microsoft\jdk-17*"
        "C:\Program Files\Eclipse Adoptium\jdk-17*"
        "C:\Program Files\Java\jdk-17*"
        "C:\Program Files\Zulu\zulu-17*"
        "C:\Program Files\Android\Android Studio\jbr"
    )
    $found = $null
    foreach ($pattern in $candidates) {
        $hit = Get-Item $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { $found = $hit.FullName; break }
    }
    if ($found) {
        $env:JAVA_HOME = $found
    } elseif ($env:JAVA_HOME -and (Test-Path (Join-Path $env:JAVA_HOME "bin\java.exe"))) {
        # 没扫到 JDK 17，但已有可用的 JAVA_HOME，就用它（版本需自行保证是 17）
    } else {
        throw "找不到 JDK 17。请安装 JDK 17，或在脚本顶部 Jdk17Home 填入路径，或设置 JAVA_HOME 环境变量"
    }
}

Write-Host "Flutter   : $flutterBat"
Write-Host "JAVA_HOME : $env:JAVA_HOME"
Write-Host "工程目录  : $ProjectDir"
Write-Host "输出位置  : $DestApk"
Write-Host ""

# --- 3. 清理旧机器残留的 local.properties（内含写死的 SDK 路径，构建时 flutter 会按本机环境自动重新生成） ---
$localProps = Join-Path $ProjectDir "android\local.properties"
if (Test-Path $localProps) {
    Remove-Item $localProps -Force -Confirm:$false
    Write-Host "已清理 android\local.properties（旧机器路径残留），构建时自动重新生成"
}

# --- 4. 构建 Release APK ---
Set-Location $ProjectDir
Write-Host "==> 开始构建 Release APK（增量 1～3 分钟，新电脑首次约 10 分钟）..." -ForegroundColor Cyan
& $flutterBat build apk --release
if ($LASTEXITCODE -ne 0) { throw "构建失败（exit code $LASTEXITCODE）。常见原因见《APK构建与部署流程.md》第七节踩坑表" }

# --- 5. 复制 APK 到本文件夹根目录 ---
if (-not (Test-Path $ApkPath)) { throw "构建报告成功但找不到产物：$ApkPath" }
Copy-Item $ApkPath $DestApk -Force
$sizeMB = [math]::Round((Get-Item $DestApk).Length / 1MB, 2)
Write-Host ""
Write-Host "√ 完成！APK 已保存到：$DestApk （$sizeMB MB）" -ForegroundColor Green
