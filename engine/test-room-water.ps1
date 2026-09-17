param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*',
      [ValidateSet('dx12','cuda')][string]$Backend='dx12',[switch]$CudaGraphs,
      [ValidateSet('uniform','fine','mixed')][string]$CudaPressure='uniform',
      [ValidateSet('conditional','unrolled')][string]$CudaPressureLoop='conditional',
      [ValidateSet('primary','cig')][string]$CudaContext='primary')
$ErrorActionPreference='Stop'
. "$PSScriptRoot/cuda-test-mode.ps1"
$cudaLaunch=Get-CudaTestArguments $Backend $CudaGraphs $CudaPressure $CudaPressureLoop $CudaContext
$out="$BuildDir/bin/Release"
if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close game/lab before room-water GPU validation.'}
$cases=@(
  @('still',240,'--no-whitewater'),
  @('flow',240,'--fluid-emitter'),
  @('flow-no-whitewater',240,'--fluid-emitter --no-whitewater'),
  @('inlet-view',240,'--fluid-emitter --fluid-view'),
  @('controls',180,'--fluid-room-test'),
  @('capacity',60,'--fluid-emitter --fluid-capacity=100064'),
  @('already-full',60,'--fluid-emitter --fluid-capacity=100000'),
  @('fixed',90,'--fluid-emitter --atomics=fixed --ser=off'),
  @('nvapi',90,'--fluid-emitter --ser=nvapi'),
  @('frame-gen',90,'--fluid-emitter --frame-gen=2'),
  @('long-flow',900,'--fluid-emitter')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  $name="room-water-$($case[0])";$started=[DateTime]::UtcNow
  if($Backend -eq 'cuda'){$name="cuda-$name"}
  if($CudaGraphs){$name="cuda-graphs-"+$name.Substring(5)}
  if($CudaPressure -ne 'uniform'){$name="cuda-$CudaPressure-"+$name.Substring(5)}
  if($CudaPressureLoop -eq 'unrolled'){$name="unrolled-$name"}
  if($CudaContext -eq 'cig'){$name="cig-$name"}
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-room --fluid-validate --fluid-backend=$Backend $cudaLaunch --frame-gen=off --width=1280 --height=720 --quality=balanced --frames=$($case[1]) --capture --name=$name $($case[2])" -WorkingDirectory $out -PassThru
  $null=$p.Handle
  try{
    if(!$p.WaitForExit(120000)){$p.Kill();throw "Room water timeout: $name"}
    $p.Refresh();if($null -eq $p.ExitCode -or $p.ExitCode -ne 0){Get-Content "$out/lab-error.txt";throw "Room water failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale room water report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json
    Assert-CudaTestMode $r $Backend $CudaGraphs $CudaPressure $CudaPressureLoop $CudaContext
    if($r.frames -ne $case[1] -or $r.dlssEvaluations -ne $case[1] -or !$r.fluid.validated -or !$r.fluid.roomPool){throw 'Incomplete room water validation'}
    if($r.fluidProbes.badRoots -or $r.fluidProbes.truncated -or $r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid fluid intersection/transport'}
    if(!$r.fluidProbes.hits -or !$r.fluidSurface.surfaceBricks -or !$r.waterPhotonEntries){throw 'Missing room geometry/optical paths'}
    if($r.fluid.active -ne 100000+$r.fluid.emittedParticles){throw 'Inlet particle accounting mismatch'}
    if(@($r.fluid.roomQuadrants|Where-Object {$_ -lt 15000}).Count){throw 'Initial water did not cover all room quadrants'}
    if($case[0] -in @('still','already-full') -and $r.fluid.emittedParticles){throw 'Closed inlet added water'}
    if($case[0] -eq 'capacity' -and (!$r.fluid.emitterFull -or $r.fluid.active -ne 100064)){throw 'Capacity did not safely close inlet'}
    if($case[0] -eq 'already-full' -and !$r.fluid.emitterFull){throw 'Initially full inlet did not close'}
    if($case[2] -notmatch 'no-whitewater'){
      if(!$r.whitewater.validated -or $r.whitewater.invalid -or $r.whitewater.foam+$r.whitewater.bubbles+$r.whitewater.spray -gt $r.whitewater.capacity){throw 'Whitewater state invalid'}
      if($case[0] -notin @('capacity','already-full') -and (!$r.whitewater.foam -or !$r.whitewater.bubbles)){throw 'Aerated moving water did not generate both foam and bubbles'}
      if($r.whitewaterProbes.bad -or ($r.whitewater.foam -and !$r.whitewaterProbes.foam) -or ($r.whitewater.bubbles -and !$r.whitewaterProbes.bubbles)){throw 'Whitewater did not pass actual DXR entry/exit/IOR probes'}
      if($r.whitewater.foamRendering -ne 'advected-grain-layer' -or !$r.whitewater.foamFieldBytes -or $r.whitewater.foamMaxDensity -gt 4){throw 'Persistent foam layer contract failed'}
      if($case[0] -notin @('capacity','already-full') -and (!$r.whitewater.foamActiveNodes -or $r.whitewater.foamMaxDensity -le .1)){throw 'Entrainment did not grow a visible foam layer'}
    }elseif($r.whitewater){throw 'Disabled whitewater still allocated'}
    Write-Host "PASS $name | $($r.fluid.active) liquid | $($r.whitewater.foam) foam / $($r.whitewater.bubbles) bubbles / $($r.whitewater.spray) spray | frame $($r.medianMs[5]) ms | whitewater $($r.medianMs[9]+$r.medianMs[10]) ms"
  }finally{$p.Dispose()}
}
