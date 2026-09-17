param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[ValidateRange(1,5)][int]$Repeats=3)
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
for($repeat=1;$repeat -le $Repeats;$repeat++){
  $modes=if($repeat%2){@('legacy','cut')}else{@('cut','legacy')}
  foreach($mode in $modes){
    if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before cut-pressure profiling'}
    $extra=if($mode -eq 'cut'){'--fluid-cut-pressure'}else{'--fluid-mac-solver=multigrid --fluid-cut-cells'}
    $name="cut-pressure-cost-$mode-$repeat";$started=[DateTime]::UtcNow
    $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-room --fluid-emitter --orbit-test --frame-gen=off --width=1920 --height=1080 --quality=balanced --frames=300 --capture --name=$name $extra" -WorkingDirectory $out -PassThru
    try{
      if(!$p.WaitForExit(180000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned cut-pressure profile timed out: $name"}
      $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Cut-pressure profile failed: $name"}
      if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale pressure profile'}
      $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$m=$r.fluid.adaptiveMac
      if($r.frames -ne 300 -or $r.sampleCount -ne 268 -or $r.fluid.steps -ne 600 -or $r.fluid.droppedSeconds -or $r.frameGeneration.enabled -or $r.fluid.validated -or $m.auditedFrames -or $r.fluid.cutCells.auditedFrames){throw 'Invalid pressure profile workload'}
      if($m.cutPressure -ne ($mode -eq 'cut') -or $m.multigrid.exhaustedSolves){throw 'Incorrect/unconverged pressure profile mode'}
      Write-Host "$name | raw $($r.medianMs[5]) ms | fluid $($r.medianMs[6]) ms | pressure mean $($m.meanMs) ms"
    }finally{$p.Dispose()}
  }
}
