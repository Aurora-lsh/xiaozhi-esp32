$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$venvPython = Join-Path $root ".venv\Scripts\python.exe"

if (-not (Test-Path -LiteralPath (Join-Path $root ".env"))) {
  Copy-Item -LiteralPath (Join-Path $root ".env.example") -Destination (Join-Path $root ".env")
  Write-Host "已创建 mcp_server\.env，请先填写 MCP_ENDPOINT。"
  exit 2
}

# fastmcp 要求 Python >= 3.10，而 PATH 上的 python 可能更旧。
# 优先用 uv：它能自动选中本机合适的解释器并同步依赖。
$uv = Get-Command uv -ErrorAction SilentlyContinue
$created = $false

if (-not (Test-Path -LiteralPath $venvPython)) {
  $created = $true
  if ($uv) {
    $pyArg = if ($env:PYTHON_EXE) { $env:PYTHON_EXE } else { ">=3.10" }
    Write-Host ("使用 uv 创建虚拟环境（Python {0}）..." -f $pyArg)
    & $uv.Source venv --python $pyArg (Join-Path $root ".venv")
    if ($LASTEXITCODE -ne 0) { throw "uv venv 创建失败，请检查 Python 3.10+ 是否可用。" }
  } else {
    # 没有 uv 时手工搜索一个 >= 3.10 的解释器。
    $candidates = @()
    foreach ($name in @("python", "python3")) {
      $cmd = Get-Command $name -ErrorAction SilentlyContinue
      if ($cmd) { $candidates += $cmd.Source }
    }
    if ($env:PYTHON_EXE) { $candidates += $env:PYTHON_EXE }
    $best = $null
    foreach ($exe in ($candidates | Select-Object -Unique)) {
      $ver = & $exe -c "import sys;print('%d.%d' % sys.version_info[:2])" 2>$null
      if ($LASTEXITCODE -ne 0 -or -not $ver) { continue }
      $parts = $ver.Trim().Split(".")
      if ([int]$parts[0] -lt 3 -or ([int]$parts[0] -eq 3 -and [int]$parts[1] -lt 10)) { continue }
      $best = $exe
      break
    }
    if (-not $best) {
      throw "找不到 Python 3.10 或更高版本（fastmcp 的最低要求）。请安装 uv 或 Python 3.10+，也可用环境变量 PYTHON_EXE 指定。"
    }
    Write-Host "创建虚拟环境: $best"
    & $best -m venv (Join-Path $root ".venv")
    if ($LASTEXITCODE -ne 0) { throw "虚拟环境创建失败。" }
  }
}

if ($uv) {
  Write-Host "同步依赖（uv pip install）..."
  & $uv.Source pip install --python $venvPython -r (Join-Path $root "requirements.txt")
  if ($LASTEXITCODE -ne 0) { throw "依赖安装失败。" }
} elseif ($created) {
  Write-Host "安装依赖（pip）..."
  & $venvPython -m pip install --upgrade pip
  & $venvPython -m pip install -r (Join-Path $root "requirements.txt")
  if ($LASTEXITCODE -ne 0) { throw "依赖安装失败。" }
}

Write-Host "启动 ESP32-CAM Xiaozhi MCP bridge..."
& $venvPython (Join-Path $root "xiaozhi_mcp_bridge.py")
