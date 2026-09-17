param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",
      [ValidateRange(1,5)][int]$Repeats=3,
      [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Tag='bulk-cost',[switch]$Projected,[switch]$Capacity,[switch]$Bounded)
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
for($repeat=1;$repeat -le $Repeats;$repeat++){
  $modes=if($repeat%2){@('off','on')}else{@('on','off')}
  foreach($mode in $modes){
    if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before bulk profiling'}
    $extra=if($mode -eq 'on'){if($Bounded){'--fluid-bulk-bounded'}elseif($Capacity){'--fluid-bulk-capacity'}elseif($Projected){'--fluid-bulk-projected'}else{'--fluid-bulk'}}else{''}
    if($Bounded){$extra+=' --fluid-bulk-capacity'}
    if($Capacity){$extra+=' --fluid-bulk-projected'}
    if($Projected){$extra+=' --fluid-cut-pressure'}
    $name="$Tag-$mode-$repeat";$started=[DateTime]::UtcNow
    $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-room --fluid-emitter --fluid-adaptive --orbit-test --frame-gen=off --width=1920 --height=1080 --quality=balanced --frames=300 --capture --name=$name $extra" -WorkingDirectory $out -PassThru
    try{
      if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned profile timed out: $name"}
      $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Bulk profile failed: $name"}
      if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale bulk profile'}
      $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$b=$r.fluid.bulk
      if($r.frames -ne 300 -or $r.sampleCount -ne 268 -or $r.fluid.steps -ne 600 -or $r.fluid.droppedSeconds -or $r.frameGeneration.enabled -or $r.fluid.validated -or $b.validated){throw 'Invalid raw-frame profile'}
      if((!$Capacity -and !$Bounded -and $mode -eq 'off' -and $b) -or ($mode -eq 'on' -and (!$b -or $b.invalid -or $b.steps -ne 600))){throw 'Incorrect bulk schedule'}
      if($Bounded -and (!$b.sourceAllocation -or !$b.projectedFlux -or ($mode -eq 'off' -and $b.phaseLimiter) -or ($mode -eq 'on' -and (!$b.phaseLimiter -or $b.phaseLimiter.validated -or $b.phaseLimiter.invalid -or $b.phaseLimiter.exhaustedSteps -or $b.phaseLimiter.steps -ne 600)))){throw 'Incorrect phase limiter schedule'}
      if($Capacity -and (!$b.projectedFlux -or ($mode -eq 'off' -and $b.sourceAllocation) -or ($mode -eq 'on' -and (!$b.sourceAllocation -or $b.sourceAllocation.invalid -or $b.sourceAllocation.validated)))){throw 'Incorrect admission schedule'}
      Write-Host "$name | raw frame $($r.medianMs[5]) ms | fluid $($r.medianMs[6]) ms | bulk mean $($b.meanFrameMs) ms"
    }finally{$p.Dispose()}
  }
}
