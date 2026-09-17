param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[ValidateRange(1,5)][int]$Repeats=3)
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
for($repeat=1;$repeat -le $Repeats;$repeat++){
  $modes=if($repeat%2){@('legacy','full','cached')}else{@('cached','full','legacy')}
  foreach($mode in $modes){
    if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before kernel profiling'}
    $extra=if($mode -eq 'legacy'){'--fluid-mac-solver=multigrid --fluid-cut-cells'}else{'--fluid-cut-pressure'}
    if($mode -eq 'full'){$extra+=' --fluid-cut-kernel-full'}
    $name="density-kernel-cost-$mode-$repeat";$started=[DateTime]::UtcNow
    $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-room --fluid-emitter --orbit-test --frame-gen=off --width=1920 --height=1080 --quality=balanced --frames=300 --capture --name=$name $extra" -WorkingDirectory $out -PassThru
    try{
      if(!$p.WaitForExit(180000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned kernel profile timed out: $name"}
      $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Kernel profile failed: $name"}
      if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale kernel profile'}
      $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$m=$r.fluid.adaptiveMac;$k=$r.fluid.cutCells
      if($r.frames -ne 300 -or $r.sampleCount -ne 268 -or $r.fluid.steps -ne 600 -or $r.fluid.droppedSeconds -or $r.frameGeneration.enabled -or $r.fluid.validated -or $m.auditedFrames -or $k.auditedFrames){throw 'Invalid kernel profile workload'}
      if($m.cutPressure -ne ($mode -ne 'legacy') -or $k.solidKernelEnabled -ne ($mode -ne 'legacy') -or $m.multigrid.exhaustedSolves){throw 'Incorrect/unconverged kernel profile'}
      if($mode -ne 'legacy' -and $k.solidKernelCache -ne ($mode -eq 'cached')){throw 'Wrong kernel cache mode'}
      Write-Host "$name | raw $($r.medianMs[5]) ms | fluid $($r.medianMs[6]) ms | geometry $($k.meanFrameMs) ms | rebuilt $($k.lastEndpointKernelRebuilt) reused $($k.lastEndpointKernelReused)"
    }finally{$p.Dispose()}
  }
}
