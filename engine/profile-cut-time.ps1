param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",
      [ValidateRange(1,5)][int]$Repeats=3)
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
for($repeat=1;$repeat -le $Repeats;$repeat++){
  $modes=if($repeat%2){@('endpoint','time')}else{@('time','endpoint')}
  foreach($mode in $modes){
    if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before cut-time profiling'}
    $extra=if($mode -eq 'time'){'--fluid-cut-time-centered'}else{''}
    $name="cut-time-cost-$mode-$repeat";$started=[DateTime]::UtcNow
    $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-bulk-bounded --fluid-room --fluid-emitter --orbit-test --frame-gen=off --width=1920 --height=1080 --quality=balanced --frames=300 --capture --name=$name $extra" -WorkingDirectory $out -PassThru
    try{
      if(!$p.WaitForExit(180000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned cut-time profile timed out: $name"}
      $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Cut-time profile failed: $name"}
      if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale cut-time profile'}
      $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$m=$r.fluid.adaptiveMac;$c=$r.fluid.cutCells;$b=$r.fluid.bulk
      if($r.frames -ne 300 -or $r.sampleCount -ne 268 -or $r.fluid.steps -ne 600 -or $r.fluid.droppedSeconds -or $r.frameGeneration.enabled -or $r.fluid.validated -or $m.auditedFrames -or $c.auditedFrames -or $b.validated){throw 'Invalid cut-time profile workload'}
      if($c.timeCenteredPressure -ne ($mode -eq 'time') -or !$m.cutPressure -or $m.multigrid.exhaustedSolves -or $m.multigrid.peakFinalDivergence -gt .000101 -or !$b.phaseLimiter -or $b.phaseLimiter.exhaustedSteps -or $b.invalid -or $r.photonCounters[5] -or $r.photonCounters[6]){throw 'Incorrect or invalid cut-time profile mode'}
      Write-Host "$name | raw $($r.medianMs[5]) ms | fluid $($r.medianMs[6]) ms | cut geometry mean $($c.meanFrameMs) ms"
    }finally{$p.Dispose()}
  }
}
