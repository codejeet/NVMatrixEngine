param([Parameter(Mandatory=$true)][string]$BaselineShaderDir,
      [string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",
      [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Tag='paired',
      [ValidateRange(1,10)][int]$Repeats=3)
$ErrorActionPreference='Stop'
$out="$BuildDir/bin/Release";$shaders="$out/shaders"
function CheckIdle {
  if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close the game/lab before switching profile shaders.'}
}
CheckIdle
$names=@(foreach($mode in 0..2){foreach($atomic in 0..1){"Transport-$mode-$atomic.dxil"}})
foreach($name in $names){
  if(!(Test-Path "$BaselineShaderDir/$name") -or !(Test-Path "$shaders/$name")){throw "Missing shader: $name"}
}
# Compare original compiled libraries, not a runtime reference branch that can
# change shader register allocation. Save the current candidate and restore it
# even if a test fails. Only generated Transport DXIL files are switched.
$saved=(New-Item -ItemType Directory -Path "$out/profile-shaders-$Tag-$([Guid]::NewGuid().ToString('N'))").FullName
foreach($name in $names){Copy-Item "$shaders/$name" "$saved/$name"}
$manifest=[ordered]@{tag=$Tag;baselineDirectory=$BaselineShaderDir;candidateDirectory=$saved;
  executableSha256=(Get-FileHash "$out/NVMatrixFluidLab.exe").Hash;shaderHashes=@();runs=@()}
foreach($name in $names){$manifest.shaderHashes+=@{name=$name;baseline=(Get-FileHash "$BaselineShaderDir/$name").Hash;candidate=(Get-FileHash "$saved/$name").Hash}}
try{
  foreach($repeat in 1..$Repeats){foreach($view in 'play','orbit','rolling'){
    $order=if($repeat%2){@('before','after')}else{@('after','before')}
    foreach($version in $order){
      CheckIdle
      $source=if($version -eq 'before'){$BaselineShaderDir}else{$saved}
      foreach($name in $names){Copy-Item "$source/$name" "$shaders/$name" -Force}
      & "$PSScriptRoot/profile-room.ps1" -BuildDir $BuildDir -Tag "$Tag-$version" -Repeats 1 -FirstRepeat $repeat -CaseFilter "^$view`$"
      $manifest.runs+="room-perf-$Tag-$version-$view-$repeat"
      $manifest|ConvertTo-Json -Depth 8|Set-Content "$out/room-perf-$Tag-manifest.json"
    }
  }}
}finally{
  CheckIdle
  foreach($name in $names){Copy-Item "$saved/$name" "$shaders/$name" -Force}
}
