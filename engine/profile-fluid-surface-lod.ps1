param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",
      [ValidateRange(1,5)][int]$Repeats=3)
$ErrorActionPreference='Stop'
for($repeat=1;$repeat -le $Repeats;$repeat++){
  $order=if($repeat%2){@('off','on')}else{@('on','off')}
  foreach($mode in $order){
    & "$PSScriptRoot/profile-room.ps1" -BuildDir $BuildDir -Tag "surface-lod-$mode" -Repeats 1 -FirstRepeat $repeat -CaseFilter '^orbit$' -SurfaceLod:($mode -eq 'on')
  }
}
