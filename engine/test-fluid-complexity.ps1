param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*')
$ErrorActionPreference='Stop'
$out="$BuildDir/bin/Release"
$cases=@(
  @('disabled',96,'--fluid-emitter --orbit-test',0),
  @('observe',96,'--fluid-emitter --orbit-test --fluid-complexity-validate',1),
  @('rolling',300,'--fluid-emitter --rolling-test --fluid-complexity-validate',1),
  @('empty',4,'--fluid-particles=0 --no-whitewater --fluid-complexity-validate',1),
  @('freeze',16,'--fluid-complexity-freeze --fluid-complexity-validate',1),
  @('controls',16,'--fluid-complexity-controls-test',1),
  @('overlay',32,'--fluid-emitter --fluid-complexity-view=6 --fluid-complexity-validate',1),
  @('no-material',16,'--fluid-surface-tension=0 --fluid-complexity-validate',1),
  @('sphere',4,'--fluid-pit --fluid-surface-fixture=1 --fluid-validate --fluid-complexity-validate',1),
  @('sheet',4,'--fluid-pit --fluid-surface-fixture=2 --fluid-validate --fluid-complexity-validate',1),
  @('coupled-validation',240,'--fluid-emitter --fluid-validate --fluid-complexity-validate',1),
  @('fixed',32,'--fluid-emitter --atomics=fixed --ser=off --fluid-complexity-validate',1),
  @('frame-gen',90,'--fluid-emitter --frame-gen=2 --fluid-complexity-validate',1),
  @('restir-pt',90,'--fluid-emitter --restir-pt --fluid-complexity-validate',1)
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  if(Get-Process NVMatrixFluidLab,NVMatrixEngine,NVMatrixFluidLab-adaptive-baseline -ErrorAction SilentlyContinue){throw 'Close game/lab before complexity GPU validation.'}
  $name="adaptive-$($case[0])";$started=[DateTime]::UtcNow
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-room --frame-gen=off --width=1280 --height=720 --quality=balanced --frames=$($case[1]) --capture --name=$name $($case[2])" -WorkingDirectory $out -PassThru
  try {
    if(!$p.WaitForExit(90000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned test timed out: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Complexity test failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale complexity report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json
    if($r.frames -ne $case[1] -or $r.dlssEvaluations -ne $case[1] -or $r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid rendering run'}
    if(!$case[3]){if($r.complexity){throw 'Disabled classifier allocated/executed'};Write-Host "PASS $name";continue}
    $c=$r.complexity
    if(!$c.validated -or $c.invalidMetrics -or $c.activeBricks -gt $c.brickCapacity){throw 'Invalid GPU complexity partition'}
    if(($c.physicsLodBricks|Measure-Object -Sum).Sum -ne $c.activeBricks -or
       ($c.surfaceLodBricks|Measure-Object -Sum).Sum -ne $c.activeBricks){throw 'Incomplete LOD partitions'}
    if($case[0] -ne 'freeze' -and ($c.particlesByRequestedLod|Measure-Object -Sum).Sum -ne $r.fluid.particles){throw 'Brick mapping lost particle accounting'}
    if($case[0] -ne 'freeze' -and $c.surfaceBricks -ne $r.fluidSurface.surfaceBricks){throw 'Canonical surface mask mismatch'}
    if($case[0] -eq 'empty' -and ($c.activeBricks -or ($c.particlesByRequestedLod|Measure-Object -Sum).Sum -ne 0)){throw 'Empty liquid invented adaptive work'}
    if($case[0] -eq 'freeze' -and ($c.classifications -ne 1 -or $c.frozenFrames -ne 15)){throw 'Freeze did not preserve decisions'}
    if($case[0] -eq 'controls' -and ($c.classifications -ne 12 -or $c.frozenFrames -ne 4 -or $c.frozen -or $c.view -ne 'off')){throw 'Input freeze/reset/view cycle failed'}
    if($case[0] -eq 'rolling' -and !$c.movingSolidBricks){throw 'Moving ball failed to request swept refinement'}
    if($case[0] -eq 'frame-gen' -and !$r.frameGeneration.enabled){throw 'FG observer interoperability failed'}
    if($case[0] -in @('sphere','sheet') -and (!$r.fluid.validated -or $r.fluidProbes.badRoots -or $r.fluidProbes.truncated -or !$r.fluidProbes.hits)){throw 'Invalid analytic fluid geometry'}
    if($case[0] -eq 'coupled-validation' -and (!$r.fluid.validated -or !$r.whitewater.validated -or $r.whitewater.invalid -or $r.fluidProbes.badRoots -or $r.fluidProbes.truncated -or $r.whitewaterProbes.bad)){throw 'Fluid/whitewater/DXR validation failed'}
    Write-Host "PASS $name | $($c.activeBricks) active / physics $($c.physicsLodBricks -join ',') / $($c.classificationMeanMs+$c.schedulingMeanMs) ms importance"
  } finally {$p.Dispose()}
}
