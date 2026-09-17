param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*')
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
# Fixed-point photons and ordered bins isolate optical scheduling from unrelated
# floating-point scatter order. These validation settings are NOT benchmarks.
$scenes=@(
  @('room','--fluid-room --fluid-deterministic-bins --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('prism','--fixture'),
  @('room-orbit','--fluid-room --fluid-deterministic-bins --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater --orbit-test')
)
foreach($scene in $scenes){foreach($mode in @(@('cached',''),@('retraced','--camera-retrace-primary'),@('observer','--optical-validate'))){
  $name="optical-parity-$($scene[0])-$($mode[0])";if($name -notmatch $CaseFilter){continue}
  if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before optical parity validation'}
  $started=[DateTime]::UtcNow
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--atomics=fixed --ser=off --frame-gen=off --width=960 --height=540 --quality=balanced --frames=48 --capture --name=$name $($scene[1]) $($mode[1])" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned parity test timed out: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Parity failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale parity report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json
    if($r.frames -ne 48 -or $r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid parity capture'}
    Write-Host "CAPTURE $name"
  }finally{$p.Dispose()}
}}
