param([string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngine/build")
$ErrorActionPreference = 'Stop'
$out = "$BuildDir/bin/Release"
if (Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue) { throw 'Close the game/lab before GPU validation.' }
foreach ($case in @(@('initial',180),@('solved',240),@('blocked',240),@('tuning',180),@('controls',360))) {
  $name = "play-$($case[0])"
  $arguments = @('--width=1280','--height=720',"--frames=$($case[1])", "--name=$name", '--capture')
  if ($case[0] -eq 'controls') { $arguments += '--gameplay-test' } else { $arguments += "--pose=$($case[0])" }
  $started = [DateTime]::UtcNow
  $process = Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList $arguments -WorkingDirectory $out -PassThru
  try {
    if (!$process.WaitForExit(60000)) { $process.Kill(); throw "GPU timeout: $name" }
    $process.Refresh()
    if ($process.ExitCode -ne 0) { Get-Content "$out/lab-error.txt"; throw "Lab failed: $name" }
    if ((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started -or (Get-Item "$out/$name.game.json").LastWriteTimeUtc -lt $started) { throw 'Stale test report.' }
    $r = Get-Content "$out/$name.json" -Raw | ConvertFrom-Json
    $g = Get-Content "$out/$name.game.json" -Raw | ConvertFrom-Json
    if ($r.frames -ne $case[1] -or $r.dlssEvaluations -ne $case[1]) { throw "Incomplete RR rendering: $name" }
    $resetLimit = if ($case[0] -eq 'controls') { 12 } else { 1 }
    if ($r.rrHistoryResets -lt 1 -or $r.rrHistoryResets -gt $resetLimit) { throw "Physics discarded RR history: $name" }
    if ($r.hitMode -ne 0 -or $r.serActive) { throw 'Auto unexpectedly enabled expensive continuation reordering.' }
    if ($r.photonCounters[5] -or $r.photonCounters[6]) { throw "Invalid photon accumulation: $name" }
    if (@($r.cameraPathTotals | Where-Object { $_ -le 0 }).Count) { throw "Missing glass/refraction/TIR/reflection paths: $name" }
    if ($case[0] -in 'initial','blocked') {
      if ($g.gateOpen -or $g.charge -gt 0 -or $r.receiverWatts -ge .1) { throw "Negative optical control incorrectly charged: $name" }
    } elseif ($case[0] -in 'solved','tuning') {
      if (!$g.gateOpen -or $r.receiverWatts -lt .1) { throw "Spectral receiver did not open portal: $name" }
    } elseif (!$g.won -or $g.jumps -lt 1 -or $g.throws -lt 2 -or $g.tuneMoves -lt 1 -or $g.tuneTurns -lt 1) { throw 'Real input/mechanics/completion checks incomplete.' }
    Write-Host "PASS: $name | $($r.frames) DLSS frames | $($r.rrHistoryResets) RR resets | $($r.receiverWatts) W | glass paths $($r.cameraPathTotals -join ', ')"
  } finally { $process.Dispose() }
}
