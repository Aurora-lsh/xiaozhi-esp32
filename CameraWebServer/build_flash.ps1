# build_flash.ps1 -- ESP32-CAM (AI Thinker) 编译 + 烧录一体脚本
#
# 用法示例：
#   powershell -ExecutionPolicy Bypass -File .\build_flash.ps1 -Port COM9
#   powershell -ExecutionPolicy Bypass -File .\build_flash.ps1 -Port COM9 -CompileOnly
#   powershell -ExecutionPolicy Bypass -File .\build_flash.ps1 -Port COM9 -SkipCompile
#   powershell -ExecutionPolicy Bypass -File .\build_flash.ps1 -Port COM9 -NoErase
#   powershell -ExecutionPolicy Bypass -File .\build_flash.ps1 -Port COM9 -Monitor
#
# 接线（AI Thinker ESP32-CAM + USB-TTL）：
#   USB-TTL 5V  -> 板子 5V
#   USB-TTL GND -> 板子 GND
#   USB-TTL TX  -> 板子 U0R (RX)
#   USB-TTL RX  -> 板子 U0T (TX)
#   烧录前把 IO0 接 GND 进入下载模式；烧录完成后断开 IO0-GND，按一下 RST。

[CmdletBinding()]
param(
    # 串口号；可先用 "Get-ChildItem | ? { $_.Name -match 'COM' }" 或用设备管理器确认
    [string]$Port = "COM9",

    # 烧录波特率
    [ValidateSet(115200, 230400, 460800, 921600)]
    [int]$Baud = 460800,

    # Flash 工作频率，需与编译时一致（默认 80MHz）
    [ValidateSet(40, 80)]
    [int]$FlashFreq = 80,

    # 保留整片擦除开关：默认首次烧录先擦除整片
    [switch]$NoErase,

    # 只编译不烧录
    [switch]$CompileOnly,

    # 跳过编译，直接烧录已有产物
    [switch]$SkipCompile,

    # 烧录完成后打开串口监视器
    [switch]$Monitor,

    [int]$MonitorBaud = 115200,

    # 手动指定工具路径（一般不用填，脚本会自动搜索）
    [string]$ArduinoCli,
    [string]$EspTool
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Root       = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir   = Join-Path $Root "temporary/build_out"
$Fqbn       = "esp32:esp32:esp32cam"
$ProjectName= "CameraWebServer"
$PartitionCsv = Join-Path $Root "partitions.csv"

$Esp32Core  = Join-Path $env:LOCALAPPDATA "Arduino15\packages\esp32\hardware\esp32\2.0.14"
$BootApp0   = Join-Path $Esp32Core "tools\partitions\boot_app0.bin"
$GenPart    = Join-Path $Esp32Core "tools\gen_esp32part.exe"

# ---------------------------------------------------------------------------
# 输出辅助
# ---------------------------------------------------------------------------

function Write-Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Write-Ok {
    param([string]$Message)
    Write-Host "    $Message" -ForegroundColor Green
}

function Write-Warn {
    param([string]$Message)
    Write-Host "    $Message" -ForegroundColor Yellow
}

function Test-ToolFile {
    param([string]$Path)
    return ($Path -and (Test-Path -LiteralPath $Path -PathType Leaf))
}

# 在若干候选路径中挑第一个存在的文件
function Resolve-ToolPath {
    param(
        [string[]]$Candidates,
        [string]$ToolName,
        [string]$Hint
    )

    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-ToolFile $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw "找不到 $ToolName。$Hint"
}

# 从 PATH 里找命令
function Resolve-FromPath {
    param([string]$CommandName)

    $cmd = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { return $cmd.Source }
    return $null
}

# 执行外部命令并在失败时终止
function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    Write-Host "    > $Executable $($Arguments -join ' ')" -ForegroundColor DarkGray

    & $Executable @Arguments

    if ($LASTEXITCODE -ne 0) {
        throw "命令执行失败（退出码 $LASTEXITCODE）：$Executable"
    }
}

function Require-File {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$Description = "文件"
    )

    if (-not (Test-ToolFile $Path)) {
        throw "缺少$Description：$Path"
    }
}

# ---------------------------------------------------------------------------
# 工具定位
# ---------------------------------------------------------------------------

$ArduinoCli = Resolve-ToolPath -ToolName "arduino-cli" -Hint "请用 -ArduinoCli 指定路径。" -Candidates @(
    $ArduinoCli,
    $env:ARDUINO_CLI,
    "D:\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
    (Join-Path $env:LOCALAPPDATA "Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"),
    (Join-Path $env:LOCALAPPDATA "Arduino15\arduino-cli.exe"),
    (Resolve-FromPath "arduino-cli")
)

$EspToolCandidates = @(
    $EspTool,
    "D:\Python\python_3_9_amd64\Scripts\esptool.exe",
    (Join-Path $env:LOCALAPPDATA "Programs\Python\Python39\Scripts\esptool.exe"),
    (Resolve-FromPath "esptool")
)

# 额外搜索 Arduino 核心自带的 esptool
$esptoolInCore = Get-ChildItem -Path (Join-Path $env:LOCALAPPDATA "Arduino15\packages\esp32\tools") -Recurse -Filter "esptool.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
if ($esptoolInCore) { $EspToolCandidates += $esptoolInCore }

$EspTool = Resolve-ToolPath -ToolName "esptool" -Hint "请用 -EspTool 指定路径，或执行 pip install esptool。" -Candidates $EspToolCandidates

Require-File $PartitionCsv "分区表 partitions.csv"
Require-File $BootApp0     "boot_app0.bin（ESP32 核心随附）"

$FlashMode = "dio"
$FlashSize = "4MB"

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

Set-Location -LiteralPath $Root

Write-Host "========================================"
Write-Host " ESP32-CAM 编译 + 烧录"
Write-Host "========================================"
Write-Host " 工程目录   : $Root"
Write-Host " 目标板卡   : $Fqbn"
Write-Host " 串口       : $Port"
Write-Host " 烧录波特率 : $Baud"
Write-Host " Flash 频率 : ${FlashFreq}m"
Write-Host " 整片擦除   : $(-not $NoErase)"
Write-Host " 跳过编译   : $SkipCompile"
Write-Host " arduino-cli: $ArduinoCli"
Write-Host " esptool    : $EspTool"

# --- 1. 校验分区表 ---------------------------------------------------------

if (Test-ToolFile $GenPart) {
    Write-Step "校验分区表 partitions.csv"
    # gen_esp32part 在只给输入文件时会把二进制表格输出到 stdout，
    # 这里输出到临时文件并加 --quiet，避免污染控制台与误判退出码。
    $partProbe = Join-Path $env:TEMP "cam_partition_probe.bin"
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $GenPart "--quiet" "--flash-size" $FlashSize $PartitionCsv $partProbe 2>&1 | Out-Null
        $genExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedEap
        Remove-Item -LiteralPath $partProbe -Force -ErrorAction SilentlyContinue
    }
    if ($genExit -ne 0) {
        throw "分区表校验失败（退出码 $genExit）：$PartitionCsv"
    }
    Write-Ok "分区表格式正确"
} else {
    Write-Warn "未找到 gen_esp32part.exe，跳过分区表校验"
}

# --- 2. 编译 ---------------------------------------------------------------

if (-not $SkipCompile) {
    Write-Step "编译固件"

    if (-not (Test-Path -LiteralPath $BuildDir)) {
        New-Item -ItemType Directory -Path $BuildDir | Out-Null
    }

    Invoke-Checked -Executable $ArduinoCli -Arguments @(
        "compile",
        "--fqbn", $Fqbn,
        "--output-dir", $BuildDir,
        $Root
    )
    Write-Ok "编译完成"
} else {
    Write-Step "跳过编译，使用 temporary/build_out 中已有产物"
}

# --- 3. 检查产物 -----------------------------------------------------------

$BootloaderBin = Join-Path $BuildDir "$ProjectName.ino.bootloader.bin"
$PartitionsBin = Join-Path $BuildDir "$ProjectName.ino.partitions.bin"
$FirmwareBin   = Join-Path $BuildDir "$ProjectName.ino.bin"

Require-File $BootloaderBin "bootloader 固件"
Require-File $PartitionsBin "分区表固件"
Require-File $FirmwareBin   "应用固件"

Write-Step "编译产物"
Get-Item -LiteralPath $BootloaderBin, $PartitionsBin, $FirmwareBin |
    Select-Object Name, Length, LastWriteTime |
    Format-Table -AutoSize | Out-Host

if ($CompileOnly) {
    Write-Host ""
    Write-Ok "仅编译模式，已跳过烧录。"
    exit 0
}

# --- 4. 检查串口 -----------------------------------------------------------

$ports = @([System.IO.Ports.SerialPort]::GetPortNames())
if ($ports -notcontains $Port) {
    throw "串口 $Port 不存在。当前可用串口：$($ports -join ', ')。请确认 USB-TTL 已插好，或用 -Port 指定正确串口。"
}

# --- 5. 烧录 ---------------------------------------------------------------

if (-not $NoErase) {
    Write-Step "擦除整片 Flash"
    Write-Warn "如未把 IO0 接到 GND，这一步会失败。"
    Invoke-Checked -Executable $EspTool -Arguments @(
        "--chip", "esp32",
        "--port", $Port,
        "--baud", "$Baud",
        "--before", "default_reset",
        "--after", "hard_reset",
        "erase_flash"
    )
    Write-Ok "Flash 已擦除"
}

Write-Step "写入固件"
Invoke-Checked -Executable $EspTool -Arguments @(
    "--chip", "esp32",
    "--port", $Port,
    "--baud", "$Baud",
    "--before", "default_reset",
    "--after", "hard_reset",
    "write_flash",
    "-z",
    "--flash_mode", $FlashMode,
    "--flash_freq", "${FlashFreq}m",
    "--flash_size", $FlashSize,
    "0x1000",  $BootloaderBin,
    "0x8000",  $PartitionsBin,
    "0xe000",  $BootApp0,
    "0x10000", $FirmwareBin
)

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host " 烧录完成" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host "1. 断开 IO0-GND"
Write-Host "2. 按一下 RST"
Write-Host "3. 串口 $Port 波特率 115200 查看日志"
Write-Host "4. 浏览器打开 http://<板子IP>/  （视觉识别页面 /vision）"

# --- 6. 串口监视器（可选） -------------------------------------------------

if ($Monitor) {
    Write-Step "打开串口监视器（Ctrl+C 退出）"
    Invoke-Checked -Executable $ArduinoCli -Arguments @(
        "monitor",
        "--port", $Port,
        "--fqbn", $Fqbn,
        "--config", "baudrate=$MonitorBaud"
    )
}
