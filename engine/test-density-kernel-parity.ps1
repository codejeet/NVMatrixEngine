param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*')
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
$cases=@(
  @('calm',120,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0'),
  @('room',120,'--fluid-room --fluid-emitter'),
  @('wake',180,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --fluid-wake-test'),
  @('adaptive',64,'--fluid-room --fluid-emitter --fluid-resample --fluid-sparse-work --adaptive-rays --restir-pt')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  foreach($mode in @('cached','full')){
    if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before kernel parity tests'}
    $extra=if($mode -eq 'full'){'--fluid-cut-kernel-full'}else{''}
    $name="density-kernel-parity-$mode-$($case[0])";$started=[DateTime]::UtcNow
    # Operator/cut-cell snapshots are already covered by test-cut-pressure.
    # Keep independent particle/surface checks here, without repeating the slow
    # full-matrix CPU assembly in every image-parity reference frame.
    $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-cut-pressure --fluid-validate --fluid-deterministic-bins --atomics=fixed --no-whitewater --frame-gen=off --width=960 --height=540 --quality=balanced --frames=$($case[1]) --capture --name=$name $extra $($case[2])" -WorkingDirectory $out -PassThru
    try{
      if(!$p.WaitForExit(180000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned kernel parity test timed out: $name"}
      $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Kernel parity test failed: $name"}
      if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale kernel parity report'}
      $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json
      if(!$r.fluid.validated -or $r.floatAtomics -or $r.fluid.adaptiveMac.multigrid.exhaustedSolves -or !$r.fluid.cutCells.solidKernelEnabled){throw 'Invalid kernel parity workload'}
      Write-Host "PASS $name | density $($r.fluid.postStepMaxRelativeDensity) | rebuilt $($r.fluid.cutCells.lastEndpointKernelRebuilt)"
    }finally{$p.Dispose()}
  }
}
