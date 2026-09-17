param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*')
$ErrorActionPreference='Stop'
$out="$BuildDir/bin/Release"
if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close game/lab before frame generation validation.'}
$cases=@(
  @('off',60,'off',''),
  @('2x',180,'2',''),
  @('3x',180,'3',''),
  @('4x',180,'4',''),
  @('controls',180,'2','--frame-gen-test'),
  @('dred',60,'2','--dred'),
  @('fluid',90,'2','--fluid-validate --fluid-view')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  $name="frame-gen-$($case[0])";$started=[DateTime]::UtcNow
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--width=1280 --height=720 --frames=$($case[1]) --frame-gen=$($case[2]) --capture --name=$name $($case[3])" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(60000)){$p.Kill();throw "Frame generation timeout: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Frame generation failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale frame generation report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$g=$r.frameGeneration
    Copy-Item "$out/NVMatrixEngine.log" "$out/$name.log"
    # SDK emits these two startup notices even on the correctly synchronized
    # single present thread. The adjacent SR plugin is intentionally not loaded:
    # RR already performs upscaling. Reject all other warnings/errors.
    $issues=Select-String -Path "$out/$name.log" -Pattern 'SL ERROR:|SL WARN:|LAB ERROR:' |
      Where-Object {$_.Line -notmatch "Ignoring plugin 'sl.dlss' since it is was not requested|slDLSSGGetState must be synchronized with the present thread"}
    # Explicit swapchain recreation and synchronous pause captures are loading
    # stalls, not steady rendering. The SDK correctly resets its pacing timer.
    if($case[0] -eq 'controls'){$issues=$issues|Where-Object{$_.Line -notmatch 'Frame rate over 100.00ms, reseting frame timer'}}
    if($issues){$issues|ForEach-Object{Write-Host $_.Line};throw "Streamline error/warning: $name.log"}
    if($r.frames -ne $case[1] -or $r.dlssEvaluations -ne $case[1]){throw 'Incomplete RR rendering'}
    foreach($counter in @('frameTokens','simulationMarkers','submissionMarkers','presentMarkers')){
      if($g.$counter -ne $r.frames){throw "Unpaired simulation/RR/present frame: $counter"}
    }
    if($g.status -or !$g.reflex){throw 'Invalid FG/Reflex state'}
    if($case[0] -eq 'off'){
      if($g.loaded -or $g.enabled -or $g.extraPresents -or $g.presentedFrames -ne $r.frames){throw 'FG Off retained its presentation overhead'}
    }else{
      if(!$g.supported -or !$g.loaded -or !$g.enabled -or $g.extraPresents -lt 20 -or $g.presentedFrames -le $r.frames+20){throw 'No actual generated presentation frames'}
    }
    if($case[0] -eq 'fluid' -and (!$r.fluid.validated -or $r.fluidProbes.glassShellHits -ne 10 -or $r.fluidProbes.badRoots)){throw 'Fluid/glass optical regression'}
    Write-Host "PASS $name | $($r.frames) rendered / $($g.presentedFrames) presented | Reflex $($g.reflex) | status $($g.status)"
  }finally{$p.Dispose()}
}
