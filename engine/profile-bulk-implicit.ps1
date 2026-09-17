param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",
      [ValidateRange(1,5)][int]$Repeats=3)
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
for($repeat=1;$repeat -le $Repeats;$repeat++){
  $modes=if($repeat%2){@('reference','implicit')}else{@('implicit','reference')}
  foreach($mode in $modes){
    if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before implicit transport profiling'}
    $solver=if($mode -eq 'implicit'){'--fluid-bulk-implicit'}else{'--fluid-bulk-pressure'}
    $name="bulk-implicit-cost-$mode-$repeat";$started=[DateTime]::UtcNow
    $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "$solver --fluid-room --fluid-emitter --orbit-test --frame-gen=off --width=1920 --height=1080 --quality=balanced --frames=300 --capture --name=$name" -WorkingDirectory $out -PassThru
    try{
      if(!$p.WaitForExit(180000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned implicit profile timed out: $name"}
      $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Implicit profile failed: $name"}
      if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale implicit profile'}
      $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$f=$r.fluid;$b=$f.bulk;$i=$b.implicitTransport;$s=$f.bulkPressure;$m=$f.adaptiveMac;$c=$f.cutCells
      if($r.frames -ne 300 -or $r.sampleCount -ne 268 -or $f.steps -ne 600 -or $f.droppedSeconds -or $r.frameGeneration.enabled -or $f.validated -or $m.auditedFrames -or $c.auditedFrames -or $b.validated){throw 'Invalid implicit profile workload'}
      if(!$c.timeCenteredPressure -or !$m.cutPressure -or $m.multigrid.exhaustedSolves -or $m.multigrid.peakFinalDivergence -gt .000101 -or $b.invalid -or $r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid implicit profile physics'}
      if(($mode -eq 'implicit') -ne [bool]$i -or !$s -or $s.auditedFrames -or $s.auditedIdleFrames -or $s.invalid -or $s.projectionCalls -ne 600){throw 'Incorrect implicit profile mode'}
      if($i){
        if($i.steps -ne 600 -or $i.invalid -or $i.exhaustedSteps -or $i.auditedFrames -or $i.auditedIdleFrames -or $i.peakResidual -gt 2.01e-10 -or $c.bulkInClosedCellsM3){throw 'Implicit profile failed transport checks'}
      }elseif(!$b.phaseLimiter -or $b.phaseLimiter.exhaustedSteps){throw 'Reference phase limiter failed'}
      Write-Host "$name | raw $($r.medianMs[5]) ms | fluid $($r.medianMs[6]) ms | implicit transport mean $($i.meanFrameMs) ms"
    }finally{$p.Dispose()}
  }
}
