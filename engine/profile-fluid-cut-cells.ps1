param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[ValidateRange(1,5)][int]$Repeats=3)
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
for($repeat=1;$repeat -le $Repeats;$repeat++){
  $modes=if($repeat%2){@('off','on')}else{@('on','off')}
  foreach($mode in $modes){
    if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before cut-cell profiling'}
    $extra=if($mode -eq 'on'){'--fluid-cut-cells'}else{''};$name="cut-cost-$mode-$repeat";$started=[DateTime]::UtcNow
    $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-room --fluid-emitter --fluid-adaptive --orbit-test --frame-gen=off --width=1920 --height=1080 --quality=balanced --frames=300 --capture --name=$name $extra" -WorkingDirectory $out -PassThru
    try{
      if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned profile timed out: $name"}
      $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Cut-cell profile failed: $name"}
      if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale cut-cell profile'}
      $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$c=$r.fluid.cutCells
      if($r.frames -ne 300 -or $r.sampleCount -ne 268 -or $r.fluid.steps -ne 600 -or $r.fluid.droppedSeconds -or $r.frameGeneration.enabled -or $r.fluid.validated -or $c.auditedFrames){throw 'Invalid raw-frame profile'}
      if(($mode -eq 'off' -and $c) -or ($mode -eq 'on' -and (!$c -or $c.invalid))){throw 'Wrong geometry profile mode'}
      Write-Host "$name | raw $($r.medianMs[5]) ms | fluid $($r.medianMs[6]) ms | capacity mean $($c.meanFrameMs) ms"
    }finally{$p.Dispose()}
  }
}
