param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*')
$ErrorActionPreference='Stop'
$out="$BuildDir/bin/Release"
if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close the game/lab before PT GPU validation.'}
$cases=@(
  @('reference',180,'--fixture --no-restir-pt'),
  @('fresh',180,'--fixture --restir-pt --pt-history=1 --pt-spatial=0'),
  @('spatial',180,'--fixture --restir-pt --pt-history=1'),
  @('temporal',180,'--fixture --restir-pt --pt-spatial=0'),
  @('combined',180,'--fixture --restir-pt'),
  @('moving',180,'--fixture --restir-pt --animate'),
  @('room-reference',240,'--fluid-room --fluid-emitter --fluid-validate --no-restir-pt'),
  @('room',240,'--fluid-room --fluid-emitter --fluid-validate --restir-pt'),
  @('orbit',180,'--restir-pt --orbit-test'),
  @('nvapi',90,'--fixture --restir-pt --ser=nvapi'),
  @('fixed',90,'--fixture --restir-pt --ser=off --atomics=fixed'),
  @('frame-gen',90,'--fluid-room --restir-pt --frame-gen=2')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  $name="restir-$($case[0])";$started=[DateTime]::UtcNow
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--frame-gen=off --quality=balanced --width=1280 --height=720 --frames=$($case[1]) --capture --name=$name $($case[2])" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(120000)){$p.Kill();throw "PT timeout: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "PT failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale PT report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json
    if($r.frames -ne $case[1] -or $r.dlssEvaluations -ne $case[1]){throw 'Incomplete render validation'}
    if($r.restirPT.invalidLastFrame -or $r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid PT/transport'}
    if($r.restirPT.enabled -and !$r.restirPT.pixelsLastFrame){throw 'No real PT pixels'}
    if($case[0] -in @('temporal','combined') -and !$r.restirPT.temporalFrames){throw 'No temporal path reuse'}
    if($case[0] -in @('spatial','temporal','combined') -and !$r.restirPT.reusedLastFrame){throw 'No resampled paths'}
    if($case[0] -in @('fresh','spatial','moving','room') -and $r.restirPT.temporalFrames){throw 'Unexpected/stale temporal reuse'}
    if($case[0] -like 'room*' -and ($r.fluidProbes.badRoots -or $r.fluidProbes.truncated -or !$r.fluidProbes.hits)){throw 'Fluid traversal regression'}
    if($case[0] -eq 'frame-gen' -and (!$r.frameGeneration.enabled -or !$r.frameGeneration.extraPresents -or $r.frameGeneration.status)){throw 'FG handoff failed'}
    Write-Host "PASS $name | PT $($r.restirPT.pixelsLastFrame) px / $($r.restirPT.reusedLastFrame) reused / $($r.restirPT.temporalFrames) temporal frames | camera $($r.medianMs[2]) / raw frame $($r.medianMs[5]) ms"
  }finally{$p.Dispose()}
}
