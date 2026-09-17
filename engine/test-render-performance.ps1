param([Parameter(Mandatory=$true)][string]$BaselineShaderDir,
      [string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",
      [ValidateSet('both','before','after')][string]$Version='both',
      [string]$CaseFilter='.*',[string]$BackendFilter='.*')
$ErrorActionPreference='Stop'
$out="$BuildDir/bin/Release";$shaders="$out/shaders"
function CheckIdle {
  if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close the game/lab before render parity tests.'}
}
CheckIdle
$names=@(foreach($mode in 0..2){foreach($atomic in 0..1){"Transport-$mode-$atomic.dxil"}})
foreach($name in $names){if(!(Test-Path "$BaselineShaderDir/$name")){throw "Missing original shader $name"}}
$saved=(New-Item -ItemType Directory -Path "$out/profile-shaders-parity-$([Guid]::NewGuid().ToString('N'))").FullName
foreach($name in $names){Copy-Item "$shaders/$name" "$saved/$name"}
try{
  $versions=if($Version -eq 'both'){@('before','after')}else{@($Version)}
  foreach($variant in $versions){
    CheckIdle
    $source=if($variant -eq 'before'){$BaselineShaderDir}else{$saved}
    foreach($name in $names){Copy-Item "$source/$name" "$shaders/$name" -Force}
    foreach($case in @(@('sphere','--fluid-validate --fluid-view --fluid-surface-fixture=1'),
                       @('sheet','--fluid-validate --fluid-view --fluid-surface-fixture=2'),
                       @('play',''))){
      if($case[0] -notmatch $CaseFilter){continue}
      foreach($backend in @(@('plain','--ser=off'),@('dxr','--ser=dxr'),@('nvapi','--ser=nvapi'),@('fixed','--ser=off --atomics=fixed'))){
        if($backend[0] -notmatch $BackendFilter){continue}
        CheckIdle
        $name="render-parity-$variant-$($case[0])-$($backend[0])";$started=[DateTime]::UtcNow
        $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--width=960 --height=540 --frames=32 --frame-gen=off --capture --name=$name $($case[1]) $($backend[1])" -WorkingDirectory $out -PassThru
        try{
          if(!$p.WaitForExit(90000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Parity timeout: $name"}
          $p.Refresh();if($p.ExitCode){throw "Parity failed: $name"}
          if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale parity capture'}
          $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json
          if($r.frames -ne 32 -or $r.dlssEvaluations -ne 32 -or $r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid parity run'}
          if($case[0] -ne 'play' -and ($r.fluidProbes.badRoots -or $r.fluidProbes.truncated -or $r.fluidProbes.glassShellHits -ne 10)){throw 'Invalid fluid probes'}
          Write-Host "PASS $name | paths $($r.cameraPathTotals -join ', ') | truncated $($r.cameraTruncatedLastFrame)"
        }finally{$p.Dispose()}
      }
    }
  }
}finally{
  CheckIdle
  foreach($name in $names){Copy-Item "$saved/$name" "$shaders/$name" -Force}
}
