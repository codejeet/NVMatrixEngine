param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build")
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
# Validate scheduler changes against identical physical/numerical settings.
# Odd iteration counts exercise the paired-density scalar tail explicitly.
foreach($case in @(@('room',90,'--fluid-room --fluid-emitter'),
                   @('odd',60,'--fluid-room --fluid-emitter --fluid-density-iterations=121'),
                   @('fall',120,'--fluid-pit --no-whitewater'))){
  $reference=$null
  foreach($mode in @(@('paired',''),@('scalar','--fluid-density-scalar'),@('split','--fluid-mac-split-coarse'))){
    if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before schedule validation'}
    $name="mac-schedule-$($case[0])-$($mode[0])";$started=[DateTime]::UtcNow
    $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-mac-solver=multigrid --fluid-deterministic-bins --fluid-validate --fluid-complexity-validate --frame-gen=off --width=1280 --height=720 --quality=balanced --frames=$($case[1]) --capture --name=$name $($case[2]) $($mode[1])" -WorkingDirectory $out -PassThru
    try{
      if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned schedule test timed out: $name"}
      $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Schedule test failed: $name"}
      $path="$out/$name.json"
      if((Get-Item $path).LastWriteTimeUtc -lt $started){throw 'Stale schedule report'}
      $r=Get-Content $path -Raw|ConvertFrom-Json;$f=$r.fluid;$mg=$f.adaptiveMac.multigrid
      if(!$f.validated -or !$f.adaptiveMac.validated -or !$mg.validated -or $mg.exhaustedSolves -or $mg.peakFinalDivergence -gt .000101){throw 'Invalid/unconverged schedule result'}
      if($reference){
        $volumeError=[Math]::Abs($r.fluidSurface.tetrahedralVolumeEstimate/$reference.fluidSurface.tetrahedralVolumeEstimate-1)
        $densityError=[Math]::Abs($f.postStepMaxRelativeDensity-$reference.fluid.postStepMaxRelativeDensity)
        if($volumeError -gt .000001 -or $densityError -gt .00001 -or $f.particleMassUnits -ne $reference.fluid.particleMassUnits){throw "Schedule changed physics: volume $volumeError / density $densityError"}
      }else{$reference=$r}
      Write-Host "PASS $name | density $($f.postStepMaxRelativeDensity) | $($f.densitySchedule) / $($mg.coarseSchedule)"
    }finally{$p.Dispose()}
  }
}
