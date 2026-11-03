<#
.SYNOPSIS
  Footprint gate (client/desktop-host Footprint Budgets): install size of the core package and the idle working set
  of the core process with the window closed. Exits non-zero when a budget is exceeded.
#>
param(
    [Parameter(Mandatory)] [string] $BuildDir,
    [double] $MaxInstallMB = 25,
    [double] $MaxIdleWorkingSetMB = 60,
    [int] $IdleSeconds = 20
)
$ErrorActionPreference = 'Stop'
$bin = Join-Path $BuildDir 'bin'
$files = 'Workbench.exe', 'workbench-worker.exe', 'pdfium.dll' | ForEach-Object { Get-Item (Join-Path $bin $_) }
$installMB = ($files | Measure-Object Length -Sum).Sum / 1MB
$failures = @()
Write-Host ("Install size: {0:N2} MB (budget {1} MB)" -f $installMB, $MaxInstallMB)
if ($installMB -gt $MaxInstallMB) { $failures += "install size $([math]::Round($installMB, 2)) MB > $MaxInstallMB MB" }

# Idle working set: an isolated instance (own data root and pipe, no control plane), window closed.
$root = Join-Path ([IO.Path]::GetTempPath()) ("workbench-footprint-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $root | Out-Null
$exe = Join-Path $bin 'Workbench.exe'
$p = Start-Process $exe -ArgumentList '--background', '--no-register', '--data-root', "`"$root\data`"", '--pipe-name', 'workbench-footprint', '--config', "`"$root\none.json`"" -PassThru
try {
    $samples = @()
    for ($i = 0; $i -lt $IdleSeconds; $i++) {
        Start-Sleep 1
        $p.Refresh()
        if ($p.HasExited) { throw "Workbench.exe exited with code $($p.ExitCode)" }
        $samples += $p.WorkingSet64 / 1MB
    }
    $cpu = $p.TotalProcessorTime.TotalSeconds
    $ws = ($samples | Select-Object -Last 5 | Measure-Object -Maximum).Maximum
    Write-Host ("Idle working set: {0:N1} MB (budget {1} MB); CPU {2:N2} s over {3} s" -f $ws, $MaxIdleWorkingSetMB, $cpu, $IdleSeconds)
    if ($ws -gt $MaxIdleWorkingSetMB) { $failures += "idle working set $([math]::Round($ws, 1)) MB > $MaxIdleWorkingSetMB MB" }
} finally {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep 1
    Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
}
# Release builds only (debug builds carry symbols and are not representative).
if ($failures) { Write-Error ("Footprint budget exceeded: " + ($failures -join '; ')); exit 1 }
Write-Host 'Footprint within budget.'
