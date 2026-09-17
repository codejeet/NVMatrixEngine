param([string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngine/build")
$ErrorActionPreference = 'Stop'
$out = [IO.Path]::GetFullPath("$BuildDir/bin/Release")
$exe = Join-Path $out 'NVMatrixFluidLab.exe'
if (Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue) { throw 'Close the game/lab before launch tests.' }
function Check-Launch([string]$Name, [string]$Command, [string]$WorkingDirectory) {
  $started = [DateTime]::UtcNow
  $info = [Diagnostics.ProcessStartInfo]::new()
  $info.FileName = $env:ComSpec
  $info.Arguments = "/d /s /c `"$Command --fixture --width=960 --height=540 --frames=48 --name=$Name`""
  $info.WorkingDirectory = $WorkingDirectory
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $child = [Diagnostics.Process]::Start($info)
  try {
    if (!$child.WaitForExit(60000)) { $child.Kill(); throw "Launch timed out: $Name" }
    if ($child.ExitCode -ne 0) { throw "Launch failed: $Name (exit $($child.ExitCode)); see $out/lab-error.txt" }
    $path = Join-Path $out "$Name.json"
    if ((Get-Item $path).LastWriteTimeUtc -lt $started) { throw "Stale launch report: $Name" }
    $report = Get-Content $path -Raw | ConvertFrom-Json
    if ($report.frames -ne 48 -or $report.dlssEvaluations -ne 48 -or $report.photonCounters[3] -le 0) { throw "Incomplete rendering: $Name" }
    Write-Host "PASS: $Name (48 frames + DLSS, module-relative assets)"
  } finally { $child.Dispose() }
}
Check-Launch 'launch-basename' 'NVMatrixFluidLab.exe' $out
Check-Launch 'launch-relative' 'Release\NVMatrixFluidLab.exe' ([IO.Directory]::GetParent($out).FullName)
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("NVMatrixEngine launch " + [char]0x03A9 + ' ' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($scratch) | Out-Null
try {
  Check-Launch 'launch-foreign-cwd' "`"$exe`"" $scratch
  if ((Get-ChildItem -LiteralPath $scratch -Force).Count) { throw 'Lab wrote files into the launch working directory.' }
} finally {
  # Only the exact, newly created empty directory; never recurse or delete test evidence.
  if ([IO.Directory]::Exists($scratch) -and !(Get-ChildItem -LiteralPath $scratch -Force)) { [IO.Directory]::Delete($scratch, $false) }
}
# Exercise the exact Explorer launcher, without --frames, then close only our lab.
if ($out -eq [IO.Path]::GetFullPath("$env:LOCALAPPDATA/NVMatrixEngine/build/bin/Release")) {
  if (Get-Process NVMatrixFluidLab -ErrorAction SilentlyContinue) { throw 'Unexpected lab process before launcher test.' }
  $started = [DateTime]::UtcNow
  $info = [Diagnostics.ProcessStartInfo]::new()
  $info.FileName = $env:ComSpec
  $info.Arguments = "/d /s /c `"`"$PSScriptRoot\Play Lab.cmd`"`""
  $info.WorkingDirectory = $out
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $launcher = [Diagnostics.Process]::Start($info)
  $interactive = $null
  try {
    if (!$launcher.WaitForExit(10000)) { $launcher.Kill(); throw 'Launcher did not return.' }
    if ($launcher.ExitCode -ne 0) { throw "Launcher failed: $($launcher.ExitCode)" }
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
      if (!$interactive) {
        $candidates = @(Get-Process NVMatrixFluidLab -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe -and $_.StartTime.ToUniversalTime() -ge $started })
        if ($candidates.Count -gt 1) { throw 'More than one lab was launched; refusing ambiguous process ownership.' }
        if ($candidates.Count -eq 1) {
          $interactive = $candidates[0]
          # Retain a handle before it exits: Get-Process alone cannot recover an exit code afterward.
          $interactive.EnableRaisingEvents = $true
          $null = $interactive.Handle
        }
      }
      if ($interactive) {
        $interactive.Refresh()
        if ($interactive.HasExited) { throw 'Interactive lab exited before rendering.' }
        if ($interactive.MainWindowTitle -like 'NVMatrixEngine Lab | Raygen / Spectral Caustics |*') { break }
      }
      Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    if (!$interactive -or $interactive.MainWindowTitle -notlike 'NVMatrixEngine Lab | Raygen / Spectral Caustics |*') { throw 'No rendered lab window after launching.' }
    if ($interactive.WaitForExit(3000)) { throw 'Interactive lab closed unexpectedly.' }
    # Exercise waitable-swapchain resize/occlusion and message wakeups on our own window.
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LabWindowTest {
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int height, uint flags);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int command);
}
'@
    $handle = $interactive.MainWindowHandle
    if (![LabWindowTest]::SetWindowPos($handle, [IntPtr]::Zero, 0, 0, 1100, 700, 0x16)) { throw 'Test resize failed.' }
    if ($interactive.WaitForExit(1500)) { throw 'Interactive lab exited during resize.' }
    [LabWindowTest]::ShowWindow($handle, 6) | Out-Null
    if ($interactive.WaitForExit(500)) { throw 'Interactive lab exited while minimized.' }
    [LabWindowTest]::ShowWindow($handle, 9) | Out-Null
    if (![LabWindowTest]::SetWindowPos($handle, [IntPtr]::Zero, 0, 0, 1280, 720, 0x16)) { throw 'Test restore resize failed.' }
    if ($interactive.WaitForExit(1500)) { throw 'Interactive lab exited on restore.' }
    if (!$interactive.CloseMainWindow()) { throw 'Cannot close the test lab gracefully.' }
    if (!$interactive.WaitForExit(15000)) { throw 'Interactive shutdown timed out.' }
    if ($interactive.ExitCode -ne 0) { throw "Interactive lab failed: $($interactive.ExitCode)" }
    $path = Join-Path $out 'lab.json'
    if ((Get-Item $path).LastWriteTimeUtc -lt $started) { throw 'Missing fresh interactive report.' }
    $report = Get-Content $path -Raw | ConvertFrom-Json
    if ($report.frames -lt 15 -or $report.dlssEvaluations -ne $report.frames) { throw 'Interactive rendering did not complete.' }
    if (!$report.pacedPresentation -or $report.rrHistoryResets -lt 3) { throw 'Pacing/resize reset path was not exercised.' }
    Write-Host "PASS: Play Lab.cmd stayed open, rendered $($report.frames) interactive frames, and shut down cleanly."
  } finally {
    if ($interactive) {
      if (!$interactive.HasExited) {
        $interactive.CloseMainWindow() | Out-Null
        if (!$interactive.WaitForExit(10000)) { $interactive.Kill() }
      }
      $interactive.Dispose()
    }
    $launcher.Dispose()
  }
}
