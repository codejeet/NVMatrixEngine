param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",
      [ValidateRange(1,5)][int]$Repeats=3)
$ErrorActionPreference='Stop'
# Real particle rooms: prove actual savings on calm water separately from the
# dynamic inlet/wake case. Interleave same-build modes; no validation or FG.
for($repeat=1;$repeat -le $Repeats;$repeat++){
  foreach($scene in 'calm','live'){
    $order=if($repeat%2){@('off','xz')}else{@('xz','off')}
    foreach($mode in $order){
      & "$PSScriptRoot/profile-room.ps1" -BuildDir $BuildDir -Tag "surface-band-$scene-$mode" -Repeats 1 -FirstRepeat $repeat -CaseFilter '^orbit$' -SurfaceLod:($mode -eq 'xz') -SurfaceLodAxes xz -Calm:($scene -eq 'calm')
    }
  }
}
