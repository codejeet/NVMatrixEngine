param([string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngine/build",[switch]$RestirPt)
$ErrorActionPreference = 'Stop'
$out = "$BuildDir/bin/Release"
if (Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue) { throw 'Close the game/lab before temporal validation.' }
$started = [DateTime]::UtcNow
$name=if($RestirPt){'pt-temporal'}else{'temporal'}
$extra=if($RestirPt){'--restir-pt'}else{'--no-restir-pt'}
$process = Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fixture --frames=256 --frame-gen=off --width=960 --height=540 --temporal-test --name=$name $extra" -WorkingDirectory $out -PassThru
try {
  if (!$process.WaitForExit(60000)) { $process.Kill(); throw 'Temporal validation timed out.' }
  $process.Refresh()
  if ($process.ExitCode -ne 0) { Get-Content "$out/lab-error.txt"; throw 'Temporal validation failed.' }
  foreach ($frame in 64,96,160,192,253,254,255,256) {
    $path = "$out/$name-$frame.json"
    if ((Get-Item $path).LastWriteTimeUtc -lt $started) { throw 'Stale temporal report.' }
    $r = Get-Content $path -Raw | ConvertFrom-Json
    if ($r.frames -ne $frame -or $r.dlssEvaluations -ne $frame -or $r.rrHistoryResets -ne 1) { throw "RR history failed at $frame" }
    $atlasResets = if ($frame -eq 64) { 1 } elseif ($frame -eq 96) { 33 } else { 34 }
    if ($r.historyResets -ne $atlasResets) { throw "Wrong atlas invalidation at $frame : $($r.historyResets)" }
    if ($RestirPt -and (!$r.restirPT.enabled -or $r.restirPT.invalidLastFrame)) {throw 'Invalid PT temporal run'}
    Write-Host "PASS: frame $frame | 1 RR reset | $($r.historyResets) atlas resets"
  }
} finally { $process.Dispose() }
Write-Host 'Run node engine/validate-temporal.mjs with the Windows output directory (WSL path when using WSL Node).'
