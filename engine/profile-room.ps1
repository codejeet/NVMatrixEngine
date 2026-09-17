param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",
      [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Tag='current',
      [ValidateRange(1,10)][int]$Repeats=3,[string]$CaseFilter='.*',
      [ValidateRange(1,100)][int]$FirstRepeat=1,[switch]$Mac,[switch]$MacMultigrid,[switch]$MacSplitCoarse,
      [switch]$DensityScalar,[switch]$SparseWork,[switch]$SurfaceLod,
      [ValidateSet('x','y','z','xy','xz','yz','xyz')][string]$SurfaceLodAxes='xyz',[switch]$Calm)
$ErrorActionPreference='Stop'
$out="$BuildDir/bin/Release"
$adaptive=if($Mac){'--fluid-mac'}else{''}
if($MacMultigrid){$adaptive='--fluid-mac-solver=multigrid'}
if($MacSplitCoarse){$adaptive='--fluid-mac-split-coarse'}
if($DensityScalar){$adaptive+=' --fluid-density-scalar'}
if($SparseWork){$adaptive+=' --fluid-sparse-work'}
if($SurfaceLod){$adaptive+=" --fluid-surface-lod-axes=$SurfaceLodAxes"}
$scene=if($Calm){'--fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'}else{'--fluid-emitter'}
if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close the game/lab before room profiling.'}
# Full live room, wall inlet and whitewater; real frames only, no per-cell probes.
# All views advance the same 120 Hz solver twice per 60 Hz simulated frame.
foreach($repeat in $FirstRepeat..($FirstRepeat+$Repeats-1)){foreach($case in @(@('play',''),@('orbit','--orbit-test'),@('rolling','--rolling-test'))){
  if($case[0] -notmatch $CaseFilter){continue}
  if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Another game/lab started during profiling.'}
  $name="room-perf-$Tag-$($case[0])-$repeat";$started=[DateTime]::UtcNow
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "$adaptive --fluid-room $scene --frame-gen=off --width=1920 --height=1080 --quality=balanced --frames=300 --capture --name=$name $($case[1])" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Profile timeout: $name"}
    $p.Refresh();if($p.ExitCode){throw "Profile failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale profile'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json
    if($r.frames -ne 300 -or $r.sampleCount -ne 268 -or $r.dlssEvaluations -ne 300 -or
       $r.fluid.steps -ne 600 -or $r.fluid.droppedSeconds -ne 0 -or $r.frameGeneration.enabled -or
       $r.photonCounters[5] -or $r.photonCounters[6] -or $r.fluid.validated){throw 'Invalid raw-frame profile'}
    $times=@($r.samplesMs|ForEach-Object {$_[5]}|Sort-Object)
    Write-Host "$name | median [$($r.timingColumns -join ', ')]: $($r.medianMs -join ', ') | p95 $($times[[Math]::Floor(.95*($times.Count-1))]) ms"
  }finally{$p.Dispose()}
}}
