param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngineCUDA/build")
$ErrorActionPreference='Stop'
if(Get-Process NVMatrixFluidLab -ErrorAction SilentlyContinue){throw 'Close the demo before capturing.'}
$runtime=(Resolve-Path "$BuildDir/bin/Release").Path
$cases=@(
  @('water-room',180,'--fluid-room --boat --fluid-emitter'),
  @('wall-inlet',240,'--fluid-room --boat --fluid-emitter --fluid-view'),
  @('deep-pool',96,'--fluid-deep-pool'),
  @('underwater',180,'--fluid-room --boat --underwater-view --fluid-depth=1.4 --fluid-particles=400000 --fluid-emitter')
)
foreach($case in $cases){
  $name="portfolio-$($case[0])"
  $argsList="--normal-lens --width=1600 --height=900 --quality=quality --frame-gen=off --frames=$($case[1]) --capture --name=$name $($case[2])"
  $p=Start-Process "$runtime/NVMatrixFluidLab.exe" -ArgumentList $argsList -WorkingDirectory $runtime -PassThru
  $null=$p.Handle
  try{
    if(!$p.WaitForExit(120000)){$p.Kill();throw "Capture timeout: $name"}
    $p.Refresh()
    if($p.ExitCode -ne 0){Get-Content "$runtime/lab-error.txt";throw "Capture failed: $name"}
  }finally{$p.Dispose()}
  $r=Get-Content "$runtime/$name.json" -Raw|ConvertFrom-Json
  if($r.frames -ne $case[1] -or $r.dlssEvaluations -ne $case[1] -or !$r.fluidSurface.surfaceBricks){throw "Incomplete capture: $name"}
  Write-Host "CAPTURE: $runtime/$name.ppm"
}
