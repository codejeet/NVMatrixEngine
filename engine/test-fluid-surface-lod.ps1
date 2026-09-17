param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*',
      [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Prefix='surface-lod')
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
$cases=@(
  @('calm',32,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('room',32,'--fluid-room --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('deep',32,'--fluid-room --fluid-depth=1.5 --fluid-particles=250000 --fluid-capacity=250000 --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('calm-xz',32,'--fluid-pit --fluid-surface-lod-axes=xz --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('room-xz',32,'--fluid-room --fluid-surface-lod-axes=xz --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('room-xz-fine',32,'--fluid-room --fluid-surface-lod-axes=xz --fluid-surface-lod-fine --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('room-xz-view',32,'--fluid-room --fluid-surface-lod-axes=xz --fluid-surface-lod-view --fluid-view --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('deep-xz',32,'--fluid-room --fluid-surface-lod-axes=xz --fluid-depth=1.5 --fluid-particles=250000 --fluid-capacity=250000 --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('room-xz-orbit',120,'--fluid-room --fluid-surface-lod-axes=xz --orbit-test --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('fall-xz',120,'--fluid-pit --fluid-surface-lod-axes=xz --no-whitewater'),
  @('wake-xz',180,'--fluid-pit --fluid-surface-lod-axes=xz --fluid-wake-test --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('emission-xz',90,'--fluid-room --fluid-surface-lod-axes=xz --fluid-emitter'),
  @('axis-x',32,'--fluid-pit --fluid-surface-fixture=3 --fluid-surface-lod-axes=x --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('axis-y',32,'--fluid-pit --fluid-surface-fixture=3 --fluid-surface-lod-axes=y --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('axis-z',32,'--fluid-pit --fluid-surface-fixture=3 --fluid-surface-lod-axes=z --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('empty',4,'--fluid-pit --fluid-particles=0 --no-whitewater'),
  @('flat-box',32,'--fluid-pit --fluid-surface-fixture=3 --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('flat-box-fine',32,'--fluid-pit --fluid-surface-fixture=3 --fluid-surface-lod-fine --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('curved-box',32,'--fluid-pit --fluid-surface-fixture=4 --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('curved-box-fine',32,'--fluid-pit --fluid-surface-fixture=4 --fluid-surface-lod-fine --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('view',32,'--fluid-pit --fluid-surface-fixture=4 --fluid-surface-lod-view --fluid-view --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('curved-orbit',120,'--fluid-pit --fluid-surface-fixture=4 --orbit-test --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('fall',120,'--fluid-pit --no-whitewater'),
  @('wake',180,'--fluid-pit --fluid-wake-test --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('emission',90,'--fluid-room --fluid-emitter'),
  @('sheet',4,'--fluid-pit --fluid-surface-fixture=2 --no-whitewater'),
  @('sphere',4,'--fluid-pit --fluid-surface-fixture=1 --no-whitewater'),
  @('mac',120,'--fluid-pit --fluid-mac --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('fg',60,'--fluid-room --frame-gen=2'),
  @('pt',60,'--fluid-room --restir-pt')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before surface LOD validation'}
  $name="$Prefix-$($case[0])";$started=[DateTime]::UtcNow
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-surface-lod-validate --fluid-validate --fluid-deterministic-bins --frame-gen=off --width=1280 --height=720 --quality=balanced --frames=$($case[1]) --capture --name=$name $($case[2])" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned surface LOD test timed out: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Surface LOD failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale surface LOD report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$s=$r.fluidSurface;$a=$s.adaptiveSurface
    if($r.frames -ne $case[1] -or !$r.fluid.validated -or !$a.audits){throw 'Missing surface LOD validation'}
    if($r.fluidProbes.badRoots -or $r.fluidProbes.truncated -or $r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid fluid optical geometry'}
    if($case[0] -eq 'fg' -and !$r.frameGeneration.enabled){throw 'FG disabled'}
    if($case[0] -eq 'pt' -and !$r.restirPT){throw 'PT disabled'}
    if($case[0] -in 'flat-box','curved-box','curved-orbit','calm-xz','room-xz','room-xz-view' -and (!$a.coarseSurfaceBricks -or !$a.coarseGatherBricks)){throw 'No actual coarse surface work'}
    if($case[0] -like 'axis-*' -and (!$a.coarseSurfaceBricks -or !$a.coarseGatherBricks)){throw 'No single-axis coarse work'}
    if($case[0] -like '*-fine' -and ($a.coarseSurfaceBricks -or $a.coarseGatherBricks)){throw 'Force-fine reference coarsened'}
    Write-Host "PASS $name | coarse gathers $($a.coarseGatherBricks)/$($s.activeBricks); coarse surface $($a.coarseSurfaceBricks)/$($s.surfaceBricks); phi $($a.maxPhiErrorCells), normal $($a.maxNormalError)"
  }finally{$p.Dispose()}
}
