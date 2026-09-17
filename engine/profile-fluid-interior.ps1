param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",
      [ValidateRange(1,5)][int]$Repeats=3,
      [ValidateSet('calm','room')][string]$Scene='calm')
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
for($repeat=1;$repeat -le $Repeats;$repeat++){
  $modes=if($repeat%2){@('off','on')}else{@('on','off')}
  foreach($mode in $modes){
    if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before profiling'}
    $extra=if($mode -eq 'on'){'--fluid-interior'}else{''}
    $scenario=if($Scene -eq 'calm'){'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'}else{'--fluid-room --fluid-emitter --orbit-test'}
    $name="interior-cost-$Scene-$mode-$repeat";$started=[DateTime]::UtcNow
    $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "$scenario --fluid-resample --frame-gen=off --width=1920 --height=1080 --quality=balanced --frames=300 --capture --name=$name $extra" -WorkingDirectory $out -PassThru
    try{
      if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned profile timed out: $name"}
      $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Profile failed: $name"}
      if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale profile'}
      $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$b=$r.fluid.interior
      if($r.frames -ne 300 -or $r.sampleCount -ne 268 -or $r.fluid.steps -ne 600 -or $r.fluid.droppedSeconds -or $r.frameGeneration.enabled -or $r.fluid.validated -or $b.validated){throw 'Invalid raw-frame profile'}
      if(($mode -eq 'off' -and $b) -or ($mode -eq 'on' -and !$b)){throw 'Incorrect interior mode'}
      Write-Host "$name | raw frame $($r.medianMs[5]) ms | fluid $($r.medianMs[6]) ms | coarse mass $($b.massUnits)"
    }finally{$p.Dispose()}
  }
}
