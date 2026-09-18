param([string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngineHgi/build")
$ErrorActionPreference='Stop'
$out="$BuildDir/bin/Release"
if (Get-Process NVMatrixFluidLab -ErrorAction SilentlyContinue) { throw 'Close the lab before serial GPU validation.' }
& "$out/NVMatrixEngineHamiltonianGpuTest.exe" $out
if ($LASTEXITCODE) { throw 'Hamiltonian numerical/region fixture failed.' }
$cases=@(
 @('adaptive-overview','--water-lab=large --fluid-view',600),
 @('adaptive-controls','--water-lab=large --wave-controls-test',12),
 @('adaptive-follow','--water-lab=large --wave-patch-test',240),
 @('adaptive-small-fill','--wave-fill-test',360),
 @('adaptive-large-fill','--water-lab=large --wave-fill-test',600),
 @('adaptive-hos3','--water-lab=large --wave-order=3 --wave-epsilon=1',600),
 @('adaptive-hos3-fill','--water-lab=large --wave-order=3 --wave-epsilon=1 --wave-fill-test',360),
 @('adaptive-jet','--water-lab=large --inlet-view --fluid-emitter',180)
)
$results=@()
foreach($case in $cases){
 $name=$case[0];$frames=$case[2];$started=[DateTime]::UtcNow
 $arguments="--water-path=hamiltonian --normal-lens --boat --width=1280 --height=720 --quality=balanced --frame-gen=off --frames=$frames --capture --name=$name $($case[1])"
 $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList $arguments -WorkingDirectory $out -PassThru
 try {
  if(!$p.WaitForExit(300000)){$p.Kill();throw "Timeout $name"}
  $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Failed $name"}
  if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw "Stale report $name"}
  $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$h=$r.hamiltonian
  if(!$h.adaptiveRegions -or !$h.activeColumns -or $h.nonfinite -or $h.boundaryOverflow -or $h.steps -ne $r.fluid.steps -or $h.inverseDnoRelativeResidual -gt .02){throw "Invalid regions $name"}
  if($h.receivedParticles+$h.freeWaterParticles -ne $h.sourceParticles){throw "Mass lost $name"}
  if([Math]::Abs($h.publishedMeanHeightMetres-$h.meanDepthMetres-$r.fluid.domainMinimum[1]) -gt .00003){throw "Mean height lost $name"}
  if($r.photonCounters[5] -or $r.photonCounters[6] -or !$r.fluidSurface.surfaceBricks){throw "Invalid rendering $name"}
  if($r.largeWaterLab -and ($h.activeColumns -ge 512 -or $r.fluid.particles -ge .6*$r.fluid.initialParticles)){throw "Calm basin retained excessive 3D work $name"}
  $results += [ordered]@{name=$name;frames=$frames;fluid=$r.fluid;hamiltonian=$h;surfaceBricks=$r.fluidSurface.surfaceBricks;photonCounters=$r.photonCounters}
  Write-Host "PASS $name | columns $($h.activeColumns)/1024 | particles $($r.fluid.particles) | region changes $($h.regionChanges)"
 }finally{$p.Dispose()}
}
$results|ConvertTo-Json -Depth 12|Set-Content "$out/hamiltonian-regions-validation.json"
