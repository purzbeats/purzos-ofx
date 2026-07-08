# Build the purzOS OFX plugins and point Resolve at them via OFX_PLUGIN_PATH.
#
#   .\build.ps1            # configure (first run) + build Release
#   .\build.ps1 -Install   # also set the user OFX_PLUGIN_PATH (relaunch Resolve)
#
# Using OFX_PLUGIN_PATH avoids needing admin rights on
# C:\Program Files\Common Files\OFX\Plugins. Resolve reads it at launch.
param([switch]$Install)

$ErrorActionPreference = "Stop"
$root  = $PSScriptRoot
$build = Join-Path $root "build"

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
  $cands = @(
    "C:\Program Files\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2026\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  )
  $cmake = $cands | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $cmake) { throw "cmake not found on PATH or known locations" }
Write-Host "cmake: $cmake"

if (-not (Test-Path (Join-Path $build "CMakeCache.txt"))) {
  & $cmake -S $root -B $build -A x64
}
& $cmake --build $build --config Release

$bundles = Join-Path $build "bundles"
Write-Host "`nBundles:"
Get-ChildItem $bundles -Directory -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $($_.Name)" }

if ($Install) {
  # Append to (not overwrite) any existing user OFX_PLUGIN_PATH entries.
  $existing = [Environment]::GetEnvironmentVariable("OFX_PLUGIN_PATH", "User")
  $parts = @()
  if ($existing) { $parts = $existing -split ';' | Where-Object { $_ } }
  if ($parts -notcontains $bundles) { $parts += $bundles }
  $value = $parts -join ';'
  [Environment]::SetEnvironmentVariable("OFX_PLUGIN_PATH", $value, "User")
  Write-Host "`nOFX_PLUGIN_PATH (User) -> $value"
  Write-Host "Relaunch DaVinci Resolve to pick up the plugins."
}
