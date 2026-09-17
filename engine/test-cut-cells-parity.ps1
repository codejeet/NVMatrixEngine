param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build")
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
$scenes=@(@('calm',48,'--fluid-room --fluid-gravity=0 --fluid-surface-tension=0'),
  @('room-orbit',48,'--fluid-room --fluid-emitter --orbit-test'),
  @('wake',180,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --fluid-wake-test'))
foreach($scene in $scenes){foreach($mode in @('off','on')){
  if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before cut-cell parity tests'}
  $name="cut-parity-$($scene[0])-$mode";$started=[DateTime]::UtcNow;$cut=if($mode -eq 'on'){'--fluid-cut-validate'}else{''}
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "$cut --fluid-deterministic-bins --atomics=fixed --ser=off --no-whitewater --frame-gen=off --width=960 --height=540 --quality=balanced --frames=$($scene[1]) --capture --name=$name $($scene[2])" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(180000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned parity test timed out: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Cut-cell parity failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale parity report'}
    Write-Host "CAPTURE $name"
  }finally{$p.Dispose()}
}}
