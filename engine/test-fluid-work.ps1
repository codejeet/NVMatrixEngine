param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*')
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
$cases=@(
  @('empty',4,'--fluid-pit --fluid-particles=0 --no-whitewater'),
  @('constant',1,'--fluid-transfer-test=1 --fluid-solver-only'),
  @('affine',1,'--fluid-transfer-test=2 --fluid-solver-only'),
  @('compression',1,'--fluid-transfer-test=3 --fluid-solver-only'),
  @('roundtrip',1,'--fluid-transfer-test=4 --fluid-solver-only'),
  @('controls',12,'--fluid-controls-test --fluid-particles=257'),
  @('calm',120,'--fluid-pit --fluid-adaptive --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('fall',120,'--fluid-pit --no-whitewater'),
  @('wake',180,'--fluid-pit --fluid-adaptive --fluid-wake-test --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('interior',120,'--fluid-pit --fluid-interior-validate --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('interior-wake',180,'--fluid-pit --fluid-interior-validate --fluid-interior-wake-test --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('resample-fall',120,'--fluid-pit --fluid-resample-validate --no-whitewater'),
  @('long',1200,'--fluid-pit --no-whitewater'),
  @('room',90,'--fluid-room --fluid-emitter'),
  @('odd',60,'--fluid-room --fluid-emitter --fluid-density-iterations=61'),
  @('mac',120,'--fluid-pit --fluid-mac --fluid-mac-cycle-test --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('mgpcg',120,'--fluid-pit --fluid-mac-solver=multigrid --no-whitewater'),
  @('fg',60,'--fluid-room --fluid-emitter --frame-gen=2'),
  @('pt',60,'--fluid-room --fluid-emitter --restir-pt')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue};$reference=$null
  foreach($mode in 'dense','sparse'){
    if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before fluid work validation'}
    $extra=if($mode -eq 'sparse'){'--fluid-work-validate'}else{''}
    if($mode -eq 'sparse' -and $case[0] -eq 'long'){$extra='--fluid-sparse-work'}
    $name="work-$($case[0])-$mode";$started=[DateTime]::UtcNow
    $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-validate --fluid-deterministic-bins --frame-gen=off --width=1280 --height=720 --quality=balanced --frames=$($case[1]) --capture --name=$name $($case[2]) $extra" -WorkingDirectory $out -PassThru
    try{
      if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned fluid work test timed out: $name"}
      $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Fluid work failed: $name"}
      $path="$out/$name.json";if((Get-Item $path).LastWriteTimeUtc -lt $started){throw 'Stale fluid work report'}
      $r=Get-Content $path -Raw|ConvertFrom-Json;$f=$r.fluid
      if(!$f.validated -or $r.frames -ne $case[1]){throw 'Missing fluid validation'}
      if($mode -eq 'dense'){$reference=$r;Write-Host "PASS $name reference";continue}
      if(!$f.work.validated){throw 'Missing independent work coverage audit'}
      if($null -eq $f.work.sameStateFaceMaxDifference -or $null -eq $f.work.sameStateDensityMaxDifference -or $f.work.sameStateFaceMaxDifference -ne 0 -or $f.work.sameStateDensityMaxDifference -ne 0){throw 'Sparse work changed a same-state dense operator result'}
      if(!$f.work.sameStateFaceComparisons -or ($case[0] -notin 'constant','affine','compression','roundtrip' -and !$f.work.sameStateDensityComparisons)){throw 'Missing within-frame operator comparisons'}
      if($f.active -ne $reference.fluid.active -or $f.particleMassUnits -ne $reference.fluid.particleMassUnits){throw 'Sparse work changed particle mass/count'}
      foreach($field in 'postStepMaxRelativeDensity','postStepOccupiedVolume','meanHeight','maxSpeed'){
        if($null -ne $f.$field -and [Math]::Abs($f.$field-$reference.fluid.$field) -gt .00002*(1+[Math]::Abs($reference.fluid.$field))){throw "Sparse work changed $field"}
      }
      if($r.fluidSurface){
        $surfaceDifference=[Math]::Abs($r.fluidSurface.tetrahedralVolumeEstimate-$reference.fluidSurface.tetrahedralVolumeEstimate)
        if($surfaceDifference -gt .00002*(1+$reference.fluidSurface.tetrahedralVolumeEstimate)){throw 'Sparse work changed reconstructed volume'}
        if($r.fluidProbes.badRoots -or $r.fluidProbes.truncated -or $r.photonCounters[5] -or $r.photonCounters[6]){throw 'Sparse work damaged fluid optics'}
      }
      if($f.adaptiveMac.multigrid.exhaustedSolves){throw 'Sparse work caused a capped pressure solve'}
      if($case[0] -eq 'empty' -and ($f.work.lastFaceTiles -or $f.work.lastDensityTiles)){throw 'Empty fluid scheduled gather work'}
      if($case[0] -in 'calm','room' -and $f.work.lastFaceTiles -gt .5*$f.work.faceTileCapacity){throw 'Work failed to exclude empty volume'}
      if($case[0] -eq 'fg' -and !$r.frameGeneration.enabled){throw 'FG disabled'}
      if($case[0] -eq 'pt' -and !$r.restirPT){throw 'PT disabled'}
      Write-Host "PASS $name | face tiles $($f.work.lastFaceTiles)/$($f.work.faceTileCapacity) | density $($f.work.lastDensityTiles)/$($f.work.densityTileCapacity)"
    }finally{$p.Dispose()}
  }
}
if('view' -match $CaseFilter){
  if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before fluid work view validation'}
  $started=[DateTime]::UtcNow
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList '--fluid-room --fluid-work-view --fluid-work-validate --fluid-validate --frame-gen=off --width=1280 --height=720 --frames=12 --capture --name=work-view' -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw 'Owned work view test timed out'}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw 'Work view failed'}
    if((Get-Item "$out/work-view.json").LastWriteTimeUtc -lt $started){throw 'Stale work view report'}
    $r=Get-Content "$out/work-view.json" -Raw|ConvertFrom-Json
    if($r.frames -ne 12 -or !$r.fluid.work.validated -or !$r.fluid.debugVisible -or $r.fluid.debugMode -ne 7){throw 'Missing work view validation or overlay'}
    Write-Host 'PASS work-view'
  }finally{$p.Dispose()}
}
