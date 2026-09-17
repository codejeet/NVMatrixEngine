param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngineCUDA/build",[switch]$NoBuild,[switch]$NoShaders)
$ErrorActionPreference='Stop'
if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close the lab before narrow-band validation.'}
$runtime=(Resolve-Path "$BuildDir/bin/Release").Path
if(!$NoBuild){
  & 'C:/Program Files/CMake/bin/cmake.exe' -S $PSScriptRoot -B $BuildDir -DNVMATRIXENGINE_CUDA_FLUID=ON -DCMAKE_SUPPRESS_REGENERATION=ON
  if($LASTEXITCODE){throw 'Narrow-band configure failed'}
  & 'C:/Program Files/CMake/bin/cmake.exe' --build $BuildDir --config Release --target NVMatrixFluidLab NVMatrixEngineCudaNarrowBandTest NVMatrixEngineCudaGeometricTransportTest --parallel 4
  if($LASTEXITCODE){throw 'Narrow-band build failed'}
}
if(!$NoShaders){& "$PSScriptRoot/compile-shaders.ps1" -OutputDir "$runtime/shaders"}
function Run-Bounded([string]$Exe,[string]$Arguments,[string]$Name){
  $p=Start-Process $Exe -ArgumentList $Arguments -WorkingDirectory $runtime -PassThru -RedirectStandardOutput "$runtime/$Name.stdout.log" -RedirectStandardError "$runtime/$Name.stderr.log"
  $null=$p.Handle
  try{
    if(!$p.WaitForExit(120000)){$p.Kill();throw "Narrow-band validation timeout: $Name"}
    $p.Refresh()
    if($p.ExitCode -ne 0){Get-Content "$runtime/$Name.stderr.log";if(Test-Path "$runtime/lab-error.txt"){Get-Content "$runtime/lab-error.txt"};throw "Narrow-band validation failed: $Name ($($p.ExitCode))"}
  }finally{$p.Dispose()}
}
Run-Bounded "$runtime/NVMatrixEngineCudaNarrowBandTest.exe" 'validation' 'narrow-band-kernels'
Run-Bounded "$runtime/NVMatrixEngineCudaGeometricTransportTest.exe" 'validation' 'narrow-band-geometric'
$reports=@{}
foreach($case in @(@('reference','--fluid-backend=cuda --fluid-cuda-graphs=on --fluid-owned-particles'),@('calm','--fluid-narrow-band'),@('inlet','--fluid-narrow-band --fluid-emitter'),@('motion','--fluid-narrow-band --rolling-test --boat',300))){
  $name="narrow-band-deep-$($case[0])";$started=[DateTime]::UtcNow
  $frameCount=if($case.Count -gt 2){$case[2]}else{96}
  $secondary=if($case[0] -eq 'motion'){''}else{'--no-whitewater'}
  Run-Bounded "$runtime/NVMatrixFluidLab.exe" "--fluid-deep-pool --fluid-validate --frame-gen=off $secondary --width=1280 --height=720 --quality=balanced --frames=$frameCount --capture --name=$name $($case[1])" $name
  if((Get-Item "$runtime/$name.json").LastWriteTimeUtc -lt $started){throw "Stale narrow-band report: $name"}
  $r=Get-Content "$runtime/$name.json" -Raw|ConvertFrom-Json;$reports[$case[0]]=$r
  if(!$r.fluid.validated -or $r.fluid.cuda.rejectedFrames -or $r.fluid.particleAuthority.relativeVolumeError -gt 1e-11){throw "Ownership conservation failed: $name"}
  if($r.fluidProbes.badRoots -or $r.fluidProbes.truncated -or !$r.fluidProbes.hits -or !$r.fluidSurface.surfaceBricks -or !$r.waterPhotonEntries){throw "Narrow-band optical integration failed: $name"}
  if($case[0] -ne 'reference' -and (!$r.fluid.cuda.narrowBand -or $r.fluid.cuda.narrowRetired -lt 1000 -or $r.fluid.narrowBandOwnership.gridVolumeM3 -le 0)){throw "Live room did not retire particles: $name"}
  if($case[0] -eq 'calm' -and $r.fluid.active -ge $r.fluid.particles-1000){throw 'Retire/restore churn did not reduce the current live particle population.'}
  Write-Host "PASS $name | live particles $($r.fluid.active) | grid volume $($r.fluid.narrowBandOwnership.gridVolumeM3) | volume error $($r.fluid.particleAuthority.relativeVolumeError)"
}
$ratio=$reports.calm.fluidSurface.tetrahedralVolumeEstimate/$reports.reference.fluidSurface.tetrahedralVolumeEstimate
if([math]::Abs($ratio-1) -gt .025){throw "Narrow-band/reference rendered volume differs by more than 2.5%: $ratio"}
Write-Host 'PASS narrow-band conservation, live removal, inlet, moving ball/boat, density, procedural geometry and optical probes. This is not a speedup benchmark.'
