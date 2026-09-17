param(
  [string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngineCUDA/build",
  [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Tag='baseline',
  [ValidateRange(1,10)][int]$Repeats=3,
  [string]$CaseFilter='.*',
  [string]$ModeFilter='.*',
  [ValidatePattern('^[A-Za-z0-9_-]+\.exe$')][string]$Executable='NVMatrixFluidLab.exe',
  [ValidateSet('conditional','unrolled')][string]$PressureLoop='conditional',
  [ValidateSet('primary','cig')][string]$Context='primary',
  [ValidateSet('backend','pressure','all')][string]$Comparison='backend',
  [ValidateSet('room','deep')][string]$Scene='room',
  [switch]$Calm
)
$ErrorActionPreference='Stop'
. "$PSScriptRoot/cuda-test-mode.ps1"
. "$PSScriptRoot/cpu-submission-profile.ps1"
$runtime=(Resolve-Path "$BuildDir/bin/Release").Path
$exe=Join-Path $runtime $Executable
function CheckIdle {
  if(Get-Process NVMatrixFluidLab,NVMatrixEngine,([IO.Path]::GetFileNameWithoutExtension($Executable)) -ErrorAction SilentlyContinue){throw 'Close the game/lab before profiling.'}
}
function Summary($values) {
  $sorted=@($values|Sort-Object)
  if(!$sorted.Count){throw 'Missing timing samples'}
  [ordered]@{median=$sorted[[Math]::Floor(.5*($sorted.Count-1))];
    p95=$sorted[[Math]::Floor(.95*($sorted.Count-1))];
    p99=$sorted[[Math]::Floor(.99*($sorted.Count-1))];max=$sorted[-1];
    missed60Hz=@($sorted|Where-Object {$_ -gt (1000/60)}).Count;count=$sorted.Count}
}
CheckIdle
$hashes=[ordered]@{executable=(Get-FileHash $exe).Hash;shaders=[ordered]@{};
  profileScript=(Get-FileHash $PSCommandPath).Hash;validator=(Get-FileHash "$PSScriptRoot/cuda-test-mode.ps1").Hash;
  cpuValidator=(Get-FileHash "$PSScriptRoot/cpu-submission-profile.ps1").Hash}
foreach($file in Get-ChildItem "$runtime/shaders" -Filter '*.dxil'){$hashes.shaders[$file.Name]=(Get-FileHash $file.FullName).Hash}
$manifest=[ordered]@{version=6;tag=$Tag;comparison=$Comparison;scene=$Scene;calm=[bool]$Calm;cudaContext=$Context;executable=$Executable;hashes=$hashes;runs=@();
  pressureNote='Pressure comparison includes the old fixed-Jacobi baseline for cost context only. Fine/mixed MGPCG share convergence targets; timing alone does not establish equal physical or optical error.';
  note='Same executable/shaders/settings. FG/probes off. All frames retained, first 32 separated. Whole-frame CPU wall duration includes simulation, UI and present wait. Memory is frame-sampled DXGI process local usage, not an exact subframe peak. Sparse topology transactions and per-kernel profiling remain separate gates.'}
foreach($repeat in 1..$Repeats){foreach($case in @(@('play',''),@('orbit','--orbit-test'),@('rolling','--rolling-test'),@('bursts','--profile-fluid-bursts'))){
  if($case[0] -notmatch $CaseFilter){continue}
  $order=if($Comparison -eq 'all'){@('dx12','cuda-graphs','cuda-fine','cuda-mixed')}elseif($Comparison -eq 'pressure'){@('cuda-graphs','cuda-fine','cuda-mixed')}else{@('dx12','cuda','cuda-graphs')}
  if(!($repeat%2)){[array]::Reverse($order)}
  foreach($mode in $order){
    if($mode -notmatch $ModeFilter){continue}
    $backend=if($mode -eq 'dx12'){'dx12'}else{'cuda'}
    $graphs=if($mode -in @('cuda-graphs','cuda-fine','cuda-mixed')){'on'}else{'off'}
    $pressure=if($mode -eq 'cuda-fine'){'fine'}elseif($mode -eq 'cuda-mixed'){'mixed'}else{'uniform'}
    $loop=if($pressure -eq 'uniform'){'conditional'}else{$PressureLoop}
    $cudaContext=if($backend -eq 'cuda'){$Context}else{'primary'}
    CheckIdle
    $name="cuda-perf-$Tag-$($case[0])-$repeat-$mode"
    $sceneArgs=if($Scene -eq 'deep'){'--fluid-deep-pool'}else{'--fluid-room'}
    $inletArgs=if($Calm){''}else{'--fluid-emitter'}
    $args="--profile-latency $sceneArgs $inletArgs --fluid-backend=$backend --fluid-cuda-graphs=$graphs --fluid-cuda-pressure=$pressure --fluid-cuda-pressure-loop=$loop --fluid-cuda-context=$cudaContext --frame-gen=off --width=1920 --height=1080 --quality=balanced --frames=300 --name=$name $($case[1])"
    $started=[DateTime]::UtcNow
    $p=Start-Process $exe -ArgumentList $args -WorkingDirectory $runtime -PassThru
    $null=$p.Handle
    try{
      if(!$p.WaitForExit(300000)){Stop-Process -Id $p.Id -Force;throw "Profile timed out: $name"}
      $p.Refresh()
      if($null -eq $p.ExitCode -or $p.ExitCode -ne 0){throw "Profile failed: $name"}
      $path="$runtime/$name.json"
      if((Get-Item $path).LastWriteTimeUtc -lt $started){throw 'Stale CUDA profile'}
      $r=Get-Content $path -Raw|ConvertFrom-Json
      Assert-CudaTestMode $r $backend ($graphs -eq 'on') $pressure $loop $cudaContext
      if([bool]$r.deepPool -ne ($Scene -eq 'deep')){throw 'Wrong pool scenario'}
      if($Scene -eq 'deep' -and ($r.fluid.initialParticles -ne 900000 -or
          [Math]::Abs($r.fluid.cellSize-.64) -gt .000001 -or $r.fluid.initialDepth -ne 8 -or
          $r.fluid.gridCells -ne 125400)){throw 'Deep-pool size/resolution changed'}
      if($Calm -and $case[0] -ne 'bursts' -and $r.fluid.emittedParticles){throw 'Calm pool emitted particles'}
      if($pressure -ne 'uniform' -and $graphs -eq 'on' -and $PressureLoop -eq 'conditional' -and
         ($r.fluid.cuda.pressureLoopSolves -ne 600 -or
          $r.fluid.cuda.pressureLoopIterations -ne $r.fluid.cuda.pressureIterations)){
        throw 'Raw profile did not execute exactly the accepted conditional pressure iterations'
      }
      if($r.frames -ne 300 -or $r.sampleCount -ne 268 -or $r.dlssEvaluations -ne 300 -or
         $r.fluid.backend -ne $backend -or $r.fluid.steps -ne 600 -or $r.fluid.droppedSeconds -ne 0 -or
         $r.frameGeneration.enabled -or $r.fluid.validated -or $r.photonCounters[5] -or $r.photonCounters[6]){
        throw 'Unequal/incomplete raw-frame profile'
      }
      if($backend -eq 'cuda' -and (($graphs -eq 'on' -and ($r.fluid.cuda.graphReplays -ne 600 -or $r.fluid.cuda.graphBuilds -ne 4)) -or
         ($graphs -eq 'off' -and ($r.fluid.cuda.graphReplays -ne 0 -or $r.fluid.cuda.directSteps -ne 600)))){
        throw 'Wrong CUDA launch mode or unbounded graph rebuilding'
      }
      $timings=[ordered]@{}
      for($i=0;$i -lt $r.timingColumns.Count;++$i){
        $values=@(foreach($row in $r.samplesMs){[double]$row[$i]})
        $timings[$r.timingColumns[$i]]=Summary $values
      }
      if($r.latency.completeFrames -ne 300 -or $r.latency.rows.Count -ne 300 -or
         $r.latency.columns.Count -ne 23 -or !$r.latency.sampledPeakLocalUsageBytes){throw 'Incomplete latency/memory samples'}
      for($i=0;$i -lt 300;++$i){
        $row=$r.latency.rows[$i]
        if($row.Count -ne 23 -or $row[0] -ne $i -or $row[1] -lt $row[7] -or
           [Math]::Abs($row[18]-($i+1)/60) -gt .00001 -or $row[19] -ne 0 -or $row[20] -le 0 -or $row[21] -le 0){
          throw 'Invalid whole-frame, simulated-time or memory sample'
        }
        foreach($value in $row){if([double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -lt 0){throw 'Nonfinite or negative profile data'}}
      }
      $phases=[ordered]@{cold=@(0,0);warmup=@(0,31);steady=@(32,299)}
      if($case[0] -eq 'bursts'){
        $phases.preInlet=@(32,89);$phases.inletOnset=@(90,119);$phases.flow=@(120,179);$phases.colliderDrop=@(180,239);$phases.settle=@(240,299)
      }
      $cpuSubmission=Get-CpuSubmissionProfile $r $phases
      $phaseStats=[ordered]@{}
      foreach($phase in $phases.Keys){
        $bounds=$phases[$phase];$stats=[ordered]@{}
        foreach($column in (@(1..17)+@(22))){
          $values=@(foreach($i in $bounds[0]..$bounds[1]){[double]$r.latency.rows[$i][$column]})
          $stats[$r.latency.columns[$column]]=Summary $values
        }
        $phaseStats[$phase]=@{firstFrame=$bounds[0];lastFrame=$bounds[1];timings=$stats}
      }
      $manifest.runs+=@{name=$name;backend=$backend;mode=$mode;pressure=$pressure;context=$cudaContext;arguments=$args;startedUTC=$started.ToString('o');
        reportSHA256=(Get-FileHash $path).Hash;simulatedSeconds=($r.fluid.steps/120);timings=$timings;cudaLastFrame=$r.fluid.cuda;
        fluidConfiguration=$r.fluid;
        phases=$phaseStats;startupMs=$r.latency.startupMs;firstFrameCompletedMs=$r.latency.firstFrameCompletedMs;
        sampledPeakLocalUsageBytes=$r.latency.sampledPeakLocalUsageBytes;cpuSubmission=$cpuSubmission}
      $whole=$phaseStats.steady.timings.wholeFrame
      Write-Host "$name | whole frame $($whole.median) / p95 $($whole.p95) / p99 $($whole.p99) ms | fluid $($timings.fluidSimulation.median) ms | sampled VRAM $($r.latency.sampledPeakLocalUsageBytes) bytes"
    }finally{$p.Dispose()}
  }
}}
if((Get-FileHash $exe).Hash -ne $hashes.executable){throw 'Executable changed during profile'}
if((Get-FileHash $PSCommandPath).Hash -ne $hashes.profileScript -or
   (Get-FileHash "$PSScriptRoot/cuda-test-mode.ps1").Hash -ne $hashes.validator -or
   (Get-FileHash "$PSScriptRoot/cpu-submission-profile.ps1").Hash -ne $hashes.cpuValidator){throw 'Profile/validator changed during profile'}
foreach($name in $hashes.shaders.Keys){if((Get-FileHash "$runtime/shaders/$name").Hash -ne $hashes.shaders[$name]){throw 'Shader changed during profile'}}
if(!$manifest.runs.Count){throw 'No profile cases selected'}
$manifest|ConvertTo-Json -Depth 12|Set-Content "$runtime/cuda-perf-$Tag-manifest.json" -Encoding UTF8
