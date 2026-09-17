param([string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngine/build")
$ErrorActionPreference = 'Stop'
$out = "$BuildDir/bin/Release"
if (Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue) { throw 'Close the game/lab before water validation.' }
$cases = @(@('water-green','--laser-nm=532'),@('water-blue','--laser-nm=450'),@('water-red','--laser-nm=638'),@('water-flat','--flat-water --no-lasers'),@('water-no-laser','--no-lasers'),@('water-off','--no-water'),@('water-fixed','--atomics=fixed'),@('water-no-haze','--no-haze'))
foreach($case in $cases) {
  $name=$case[0]
  $started=[DateTime]::UtcNow
  $arguments="--frames=192 --width=1280 --height=720 --capture --name=$name $($case[1])"
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList $arguments -WorkingDirectory $out -PassThru
  try {
    if(!$p.WaitForExit(60000)){$p.Kill();throw "Water validation timed out: $name"}
    $p.Refresh()
    if($p.ExitCode -ne 0){Get-Content "$out/lab-error.txt";throw "Water validation failed: $name"}
    $path="$out/$name.json"
    if((Get-Item $path).LastWriteTimeUtc -lt $started){throw 'Stale water report.'}
    $r=Get-Content $path -Raw|ConvertFrom-Json
    if($r.frames -ne 192 -or $r.dlssEvaluations -ne 192 -or $r.rrHistoryResets -ne 1){throw 'Water disrupted reconstruction history.'}
    if($r.waterEnabled -and ($r.waterPhotonEntries -lt 1000 -or $r.waterFloorWatts -lt 10)){throw 'Water photons did not reach the submerged receiver.'}
    if(!$r.waterEnabled -and ($r.waterPhotonEntries -or $r.waterFloorWatts)){throw 'Disabled water still contributes.'}
    if($r.lasersEnabled -and ($r.beamSegments -lt 2 -or $r.laserDeposits -lt 1)){throw 'Laser paths or surface deposits missing.'}
    if(!$r.lasersEnabled -and ($r.beamSegments -or $r.laserDeposits)){throw 'Disabled lasers still contribute.'}
    if($r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid photon accumulation.'}
    $sum=0;foreach($x in $r.photonEnergyWatts[1..6]){$sum+=$x}
    if([Math]::Abs($sum-$r.photonEnergyWatts[0]) -gt .15){throw 'Photon radiant-power ledger does not balance.'}
    Write-Host "PASS: $name | $($r.waterFloorWatts) W submerged | $($r.beamSegments) beam segments | 1 RR reset"
  }finally{$p.Dispose()}
}
