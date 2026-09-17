param(
  [string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",
  [string]$Tag='current',
  [int]$Repeats=2,
  [int]$Frames=360,
  [string]$CaseFilter='.*'
)
$ErrorActionPreference='Stop'
if($Tag -notmatch '^[a-zA-Z0-9_-]+$' -or $Repeats -lt 1 -or $Frames -le 32){throw 'Invalid profiling arguments'}
$out="$BuildDir/bin/Release"
if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close game/lab before GPU profiling.'}
# Fixed 1/60 simulated seconds per real frame; same evolving water state at the
# same frame index. No FG, validation-ray atomics, or presentation pacing;
# retain the normal gameplay HUD and identical scene/optical quality settings.
foreach($repeat in 1..$Repeats){
  foreach($case in @(@('pool','--fluid-view'),@('play',''))){
    if($case[0] -notmatch $CaseFilter){continue}
    $name="fluid-perf-$Tag-$($case[0])-$repeat";$started=[DateTime]::UtcNow
    $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-pit $($case[1]) --frame-gen=off --width=1920 --height=1080 --quality=balanced --frames=$Frames --capture --name=$name" -WorkingDirectory $out -PassThru
    try {
      if(!$p.WaitForExit(60000)){$p.Kill();$p.WaitForExit();throw "Profiling timeout: $name"}
      $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Profiling failed: $name"}
      if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale profiling report'}
      $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json
      if($r.frames -ne $Frames -or $r.dlssEvaluations -ne $Frames -or $r.sampleCount -ne ($Frames-32)){throw 'Incomplete profile'}
      if($r.frameGeneration.enabled -or $r.frameGeneration.presentedFrames -ne $Frames -or
         $r.fluid.steps -ne (2*$Frames) -or $r.fluid.particles -ne 100000 -or
         $r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid raw-frame profile'}
      $frameTimes=@($r.samplesMs|ForEach-Object {$_[5]}|Sort-Object)
      $p95=$frameTimes[[Math]::Floor(.95*($frameTimes.Count-1))]
      Write-Host "$name | median ms [$($r.timingColumns -join ', ')]: $($r.medianMs -join ', ') | p95 frame $p95"
    } finally {$p.Dispose()}
  }
}
