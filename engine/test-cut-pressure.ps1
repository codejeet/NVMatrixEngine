param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*',
  [ValidateSet('cut','legacy')][string]$Mode='cut',
  [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$NamePrefix='cut-pressure',[switch]$FullKernel,[switch]$FixedAtomics,[switch]$TimeCentered)
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
$cases=@(
  @('empty',4,'--fluid-pit --fluid-particles=0'),
  @('room-short',8,'--fluid-room'),
  @('calm',120,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0'),
  @('interior',120,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --fluid-interior-validate'),
  @('cycle',120,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --fluid-mac-cycle-test'),
  @('fall',120,'--fluid-pit'),
  @('room',120,'--fluid-room --fluid-emitter'),
  @('wake',180,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --fluid-wake-test'),
  @('controls',12,'--fluid-pit --fluid-particles=257 --fluid-surface-controls-test'),
  @('relaxation',12,'--fluid-room --fluid-mac-solver=relaxation'),
  @('adaptive',64,'--fluid-room --fluid-emitter --fluid-resample --fluid-sparse-work --adaptive-rays --restir-pt')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before cut-pressure tests'}
  $name="$NamePrefix-$(if($Mode -eq 'legacy'){'legacy-'})$($case[0])";$started=[DateTime]::UtcNow
  $solver=if($Mode -eq 'cut'){'--fluid-cut-pressure'}else{'--fluid-mac-solver=multigrid'}
  if($TimeCentered){if($Mode -ne 'cut'){throw 'Time-centered tests require cut mode'};$solver+=' --fluid-cut-time-centered'}
  if($FullKernel){$solver+=' --fluid-cut-kernel-full'}
  if($FixedAtomics){$solver+=' --atomics=fixed'}
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "$solver --fluid-mac-validate --fluid-cut-validate --fluid-validate --fluid-deterministic-bins --no-whitewater --frame-gen=off --width=960 --height=540 --quality=balanced --frames=$($case[1]) --capture --name=$name $($case[2])" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(180000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned cut-pressure test timed out: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Cut-pressure test failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale cut-pressure report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$m=$r.fluid.adaptiveMac;$c=$r.fluid.cutCells
    if($m.cutPressure -ne ($Mode -eq 'cut') -or !$m.validated -or $m.invalid -or !$c.validated -or $c.invalid -or !$r.fluid.validated){throw 'Missing cut-pressure invariant audits'}
    if($c.timeCenteredPressure -ne [bool]$TimeCentered -or $c.maxTimeAreaError -gt .0001){throw 'Temporal cut geometry audit failed'}
    if($case[0] -ne 'relaxation' -and (!$m.multigrid.validated -or $m.multigrid.exhaustedSolves -or $m.multigrid.peakFinalDivergence -gt .000101)){throw 'Cut-cell MGPCG convergence failure'}
    if($r.photonCounters[5] -or $r.photonCounters[6] -or $r.fluidProbes.badRoots -or $r.fluidProbes.truncated){throw 'Invalid cut-pressure optical surface'}
    if($case[0] -eq 'calm' -and !$m.coarseLeaves){throw 'No actual coarse MAC pressure leaves'}
    if($case[0] -in @('calm','interior','cycle','wake') -and $r.fluid.postStepMaxRelativeDensity -gt 1.05){throw 'Cut-pressure calm/wake compression regression'}
    if($case[0] -eq 'cycle' -and (!$m.finePromotions -or !$m.coarseDemotions)){throw 'Cut-pressure LOD transitions were not exercised'}
    Write-Host "PASS $name | $($m.coarseLeaves) coarse leaves | matrix $($m.matrixError) | flux $($m.fluxError) | peak divergence $($m.multigrid.peakFinalDivergence)"
  }finally{$p.Dispose()}
}
