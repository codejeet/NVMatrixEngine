param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*')
$ErrorActionPreference='Stop'
$out="$BuildDir/bin/Release"
if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close the game/lab before photon sampling tests.'}
$cases=@(
  @('flow',240,'--fluid-emitter'),
  @('still',240,'--no-whitewater'),
  @('fixed',90,'--fluid-emitter --atomics=fixed --ser=off'),
  @('dxr',90,'--fluid-emitter --ser=dxr'),
  @('nvapi',90,'--fluid-emitter --ser=nvapi'),
  @('frame-gen',90,'--fluid-emitter --frame-gen=2'),
  @('restir',180,'--fluid-emitter --restir-pt')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  $name="photon-sampling-$($case[0])";$started=[DateTime]::UtcNow
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-room --fluid-validate --frame-gen=off --width=1280 --height=720 --quality=balanced --frames=$($case[1]) --capture --name=$name $($case[2])" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(120000)){$p.Kill();throw "Photon sampling timeout: $name"}
    $p.Refresh();if($p.ExitCode){throw "Photon sampling process failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale photon sampling report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json
    if($r.frames -ne $case[1] -or $r.dlssEvaluations -ne $case[1] -or !$r.fluid.validated){throw 'Incomplete GPU validation'}
    if($r.fluidProbes.badRoots -or $r.fluidProbes.truncated -or !$r.fluidProbes.hits){throw 'Invalid/missing water intersections'}
    if($r.photonCounters[5] -or $r.photonCounters[6] -or !$r.waterPhotonEntries){throw 'Invalid/missing water photons'}
    Write-Host "PASS $name | photons $($r.medianMs[0]) ms | raw frame $($r.medianMs[5]) ms"
  }finally{$p.Dispose()}
}
