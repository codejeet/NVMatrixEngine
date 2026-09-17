param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[switch]$NoProbes)
$ErrorActionPreference='Stop'
$out="$BuildDir/bin/Release"
if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close game/lab before traversal A/B tests.'}
foreach($repeat in 0,1,2){foreach($mode in 'full','tight'){
  $prefix=if($NoProbes){'traversal-raw'}else{'traversal'}
  $validation=if($NoProbes){''}else{'--fluid-validate'}
  $name="$prefix-$mode-$repeat";$started=[DateTime]::UtcNow
  $extra=if($mode -eq 'full'){'--fluid-full-brick-traversal'}else{''}
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-room --fluid-emitter $validation --no-restir-pt --frame-gen=off --quality=balanced --width=1280 --height=720 --frames=240 --capture --name=$name $extra" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(120000)){$p.Kill();throw "Traversal timeout: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Traversal failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale traversal report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json
    if($r.frames -ne 240 -or $r.fluidProbes.badRoots -or $r.fluidProbes.truncated){throw 'Invalid fluid traversal'}
    if(!$NoProbes -and (!$r.fluid.validated -or !$r.fluidProbes.hits)){throw 'Missing fluid probes'}
    if($r.fluidTightTraversal -ne ($mode -eq 'tight')){throw 'Traversal mode was not applied'}
    Write-Host "PASS $name | $($r.fluidProbes.cellSteps) cell visits / $($r.fluidProbes.intersections) brick intersections | photons $($r.medianMs[0]) / camera $($r.medianMs[2]) / frame $($r.medianMs[5]) ms"
  }finally{$p.Dispose()}
}}
