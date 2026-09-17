param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",
      [ValidateRange(1,5)][int]$Repeats=3,[ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Tag='pressure-tiled')
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
for($repeat=1;$repeat -le $Repeats;$repeat++){
  # Rotate ordering to avoid always measuring one solver on a colder GPU.
  $modes=@('uniform','active','multigrid')
  for($k=0;$k -lt 3;$k++){
    $mode=$modes[($k+$repeat-1)%3]
    if(Get-Process NVMatrixFluidLab,NVMatrixEngine,NVMatrixFluidLab-pressure-baseline -ErrorAction SilentlyContinue){throw 'Close games before pressure profiling'}
    $name="$Tag-$mode-$repeat";$started=[DateTime]::UtcNow
    $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-room --fluid-emitter --fluid-adaptive --fluid-pressure=$mode --orbit-test --frame-gen=off --width=1920 --height=1080 --quality=balanced --frames=300 --capture --name=$name" -WorkingDirectory $out -PassThru
    try{
      if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned profile timed out: $name"}
      $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Profile failed: $name"}
      if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale profile'}
      $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json
      if($r.frames -ne 300 -or $r.sampleCount -ne 268 -or $r.fluid.steps -ne 600 -or $r.fluid.droppedSeconds -or $r.frameGeneration.enabled -or $r.fluid.validated -or $r.fluid.pressureSolver.validated){throw 'Invalid raw-frame profile'}
      if($r.photonCounters[5] -or $r.photonCounters[6] -or $r.fluidProbes.badRoots -or $r.fluidProbes.truncated -or $r.whitewaterProbes.bad){throw 'Invalid optical state during profiling'}
      Write-Host "$name | frame $($r.medianMs[5]) ms | fluid $($r.medianMs[6]) ms | pressure $($r.fluid.pressureSolver.meanFrameMs) ms | coarse $($r.fluid.pressureSolver.activeCoarseCells)"
    }finally{$p.Dispose()}
  }
}
