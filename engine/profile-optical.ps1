param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",
      [ValidateRange(1,5)][int]$Repeats=3,[string]$CaseFilter='.*')
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
# Same binary/shaders, interleaved order, genuine frames, live room/inlet/foam.
# Adaptive versus uniform four samples is an equal-cap comparison, not a
# claim to be faster than the original cheap one-sample lighting estimator.
for($repeat=1;$repeat -le $Repeats;$repeat++){
  $modes=@(@('cached',''),@('retraced','--camera-retrace-primary'),
           @('adaptive','--adaptive-rays'),@('uniform','--optical-reference'))
  if(!($repeat%2)){[array]::Reverse($modes)}
  foreach($mode in $modes){
    if($mode[0] -notmatch $CaseFilter){continue}
    if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before raw-frame profiling'}
    $name="optical-perf-$($mode[0])-$repeat";$started=[DateTime]::UtcNow
    $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-room --fluid-emitter --orbit-test --frame-gen=off --width=1920 --height=1080 --quality=balanced --frames=300 --name=$name $($mode[1])" -WorkingDirectory $out -PassThru
    try{
      if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned profile timed out: $name"}
      $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Profile failed: $name"}
      if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale profile'}
      $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json
      if($r.frames -ne 300 -or $r.sampleCount -ne 268 -or $r.fluid.steps -ne 600 -or $r.frameGeneration.enabled -or
         $r.fluid.validated -or $r.opticalImportance.audits -or $r.fluid.deterministicBins -or
         $r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid raw-frame profile'}
      Write-Host "$name | camera $($r.medianMs[2]) / raw $($r.medianMs[5]) / optics $($r.opticalImportance.classificationMedianMs) ms"
    }finally{$p.Dispose()}
  }
}
