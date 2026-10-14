# flash.ps1 -- 一键烧录 ESP32-CAM (AI Thinker)
#
# 用法：
#   1) 用 USB-TTL 连接 ESP32-CAM：5V->5V, GND->GND, TX->U0R(RX), RX->U0T(TX)
#   2) 烧录时把 IO0 接 GND（进入下载模式），烧完断开并复位
#   3) powershell -ExecutionPolicy Bypass -File .\flash.ps1 -Port COM9

param(
    [Parameter(Mandatory=$true)][string]$Port,
    [switch]$NoErase,
    [int]$Baud = 460800
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$cli  = 'D:\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$fqbn = 'esp32:esp32:esp32cam'

if (-not (Test-Path -LiteralPath $cli)) { throw "找不到 arduino-cli: $cli" }
if (-not (Test-Path -LiteralPath (Join-Path $root 'temporary/build_out\CameraWebServer.ino.bin'))) {
    throw "未找到编译产物，请先执行：arduino-cli compile --fqbn $fqbn --output-dir temporary/build_out ."
}

Write-Host "目标端口 : $Port"
Write-Host "擦除整片 : $(-not $NoErase)"
Write-Host "上传波特率: $Baud"
Write-Host ""

# 分区表已改为 huge_app 布局，首次烧录前擦除整片，避免残留 nvs/ota 数据
if (-not $NoErase) {
    Write-Host '==> 擦除 flash ...'
    & $cli upload -p $Port -b $fqbn --input-dir (Join-Path $root 'temporary/build_out') `
         --upload-field EraseFlash=all -v
} else {
    Write-Host '==> 直接烧录 ...'
    & $cli upload -p $Port -b $fqbn --input-dir (Join-Path $root 'temporary/build_out') -v
}

if ($LASTEXITCODE -ne 0) { throw "烧录失败 (exit=$LASTEXITCODE)" }
Write-Host ''
Write-Host '烧录完成。断开 IO0-GND，按 RST 复位，串口 115200 查看日志。'
Write-Host '网页入口：http://<板子IP>/        视觉识别：http://<板子IP>/vision'
