param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*')
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
$scenes=@(
  @('diffuse','--fixture'),
  @('fresh-pt','--fixture --restir-pt --pt-history=1 --pt-spatial=0'),
  @('reused-pt','--fixture --restir-pt'),
  @('water','--fluid-room --fluid-deterministic-bins --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater')
)
foreach($scene in $scenes){foreach($mode in @(@('adaptive','--adaptive-rays'),@('uniform','--optical-reference'),@('reference','--optical-reference --optical-samples=8'))){
  $name="optical-estimator-$($scene[0])-$($mode[0])";if($name -notmatch $CaseFilter){continue}
  if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before estimator validation'}
  $started=[DateTime]::UtcNow
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--optical-estimator-test --atomics=fixed --frame-gen=off --width=960 --height=540 --quality=balanced --frames=256 --capture --name=$name $($scene[1]) $($mode[1])" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned estimator test timed out: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Estimator failed: $name"}
    foreach($frame in 128,144,160,176,192,208,224,240,256){
      if((Get-Item "$out/$name-$frame.json").LastWriteTimeUtc -lt $started){throw 'Stale estimator report'}
      $r=Get-Content "$out/$name-$frame.json" -Raw|ConvertFrom-Json
      if($r.frames -ne $frame -or $r.restirPT.invalidLastFrame -or $r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid estimator sample'}
      if($scene[0] -eq 'reused-pt' -and !$r.restirPT.temporalFrames){throw 'Missing temporal reuse'}
    }
    Write-Host "CAPTURE $name | 9 independently seeded production frames"
  }finally{$p.Dispose()}
}}
