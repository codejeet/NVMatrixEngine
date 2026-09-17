param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*')
$ErrorActionPreference='Stop'
$out="$BuildDir/bin/Release"
$water='--fluid-room --fluid-deterministic-bins --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'
$scenes=@(
  @('diffuse','--no-water'),
  @('water',$water),
  @('underwater',"$water --boat --underwater-view"),
  @('fresh-pt',"$water --restir-pt --pt-history=1 --pt-spatial=0"),
  @('reused-pt',"$water --restir-pt"),
  @('temporal-pt','--fluid-pit --fluid-validate --fluid-surface-fixture=2 --no-whitewater --restir-pt')
)
foreach($scene in $scenes) {
  $modes=@(@('reduced',''),@('reference','--lambertian-reference --water-visibility-reference'))
  if($scene[0] -in @('water','underwater')){$modes+=,@('roots','--lambertian-reference')}
  foreach($mode in $modes) {
    $name="lambertian-$($scene[0])-$($mode[0])"
    if($name -notmatch $CaseFilter){continue}
    if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before optical validation'}
    $started=[DateTime]::UtcNow
    $arguments="--optical-estimator-test --atomics=fixed --ser=off --normal-lens --frame-gen=off --width=960 --height=540 --quality=balanced --frames=256 --capture --name=$name $($scene[1]) $($mode[1])"
    $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList $arguments -WorkingDirectory $out -PassThru
    try {
      if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned optical test timed out: $name"}
      $p.Refresh()
      if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Optical test failed: $name"}
      foreach($frame in 128,144,160,176,192,208,224,240,256) {
        $path="$out/$name-$frame.json"
        if((Get-Item $path).LastWriteTimeUtc -lt $started){throw "Stale capture: $path"}
        $r=Get-Content $path -Raw|ConvertFrom-Json
        if($r.frames -ne $frame -or $r.restirPT.invalidLastFrame -or $r.photonCounters[5] -or $r.photonCounters[6]){throw "Invalid optical capture: $path"}
      }
      Write-Output "CAPTURE $name | nine raw production frames"
    } finally {$p.Dispose()}
  }
}
