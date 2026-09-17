param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build", [string]$CaseFilter='.*')
$ErrorActionPreference='Stop'
$out="$BuildDir/bin/Release"
if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close game/lab before DLSS settings validation.'}
foreach($fg in @('off','2')){
    if($fg -notmatch $CaseFilter){continue}
    $name="dlss-settings-$fg"; $started=[DateTime]::UtcNow
    $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--dlss-settings-test --frames=200 --width=1280 --height=720 --quality=quality --normal-lens --frame-gen=$fg --fluid --restir-pt --capture --name=$name" -WorkingDirectory $out -PassThru
    try{
        if(!$p.WaitForExit(120000)){$p.Kill();throw "DLSS settings timeout: $name"}
        $p.Refresh()
        if($p.ExitCode){Get-Content "$out/lab-error.txt" -ErrorAction SilentlyContinue;throw "DLSS settings failed: $name"}
        if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale DLSS settings report'}
        $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json
        $balanced=Get-Content "$out/$name-settings-32.json" -Raw|ConvertFrom-Json
        $performance=Get-Content "$out/$name-settings-80.json" -Raw|ConvertFrom-Json
        $quality=Get-Content "$out/$name-settings-128.json" -Raw|ConvertFrom-Json
        $fisheye=Get-Content "$out/$name-settings-176.json" -Raw|ConvertFrom-Json
        $resized=Get-Content "$out/$name-settings-182.json" -Raw|ConvertFrom-Json
        if($r.frames -ne 200 -or $r.dlssEvaluations -ne 200){throw 'Incomplete RR rendering'}
        if($balanced.dlssMode -ne 'balanced' -or $performance.dlssMode -ne 'performance' -or $quality.dlssMode -ne 'quality' -or $r.dlssMode -ne 'quality'){throw 'Menu preset did not reach RR'}
        if(!($quality.internalWidth -gt $balanced.internalWidth -and $balanced.internalWidth -gt $performance.internalWidth)){throw 'Preset rendering resolutions did not change'}
        if($balanced.rrHistoryResets -ne 2 -or $performance.rrHistoryResets -ne 3 -or $quality.rrHistoryResets -ne 4 -or $r.rrHistoryResets -ne 11){throw 'RR history was not reset exactly once per preset/lens/FOV/resize change'}
        if($resized.outputWidth -ne 1281 -or $resized.outputHeight -ne 721 -or $resized.frameGeneration.status){throw 'Odd-size fisheye resize failed'}
        foreach($checkpoint in @($balanced,$performance,$quality,$fisheye,$r)){
            if($checkpoint.outputWidth -ne 1280 -or $checkpoint.outputHeight -ne 720){throw 'Display resolution changed'}
            if($checkpoint.frameGeneration.status){throw 'Frame Generation runtime error'}
        }
        if($balanced.frameGeneration.enabled -or $performance.frameGeneration.enabled -or $quality.frameGeneration.enabled){throw 'FG did not suspend for menus'}
        if($fg -eq 'off'){
            if($r.frameGeneration.presentedFrames -ne 200 -or $r.frameGeneration.extraPresents){throw 'FG Off reported extra frames'}
        }else{
            if(!$r.frameGeneration.enabled -or !$fisheye.frameGeneration.enabled){throw 'FG did not remain available in both lens modes'}
            if(!$fisheye.frameGeneration.distortionTagged -or $r.frameGeneration.distortionTagged){throw 'Fisheye distortion tag did not follow the selected lens'}
            if(!$resized.frameGeneration.distortionTagged -or $r.frameGenerationDistortionUpdates -ne 4){throw 'Lens map cache did not follow FOV/resize changes'}
        }
        Copy-Item "$out/NVMatrixEngine.log" "$out/$name.log"
        $issues=Select-String -Path "$out/$name.log" -Pattern 'SL ERROR:|SL WARN:|LAB ERROR:' |
            Where-Object {$_.Line -notmatch "Ignoring plugin 'sl.dlss' since it is was not requested|slDLSSGGetState must be synchronized with the present thread|Frame rate over 100.00ms, reseting frame timer"}
        if($issues){$issues|ForEach-Object{Write-Host $_.Line};throw 'Unexpected Streamline warning/error'}
        Write-Host "PASS $name | internal widths $($quality.internalWidth)/$($balanced.internalWidth)/$($performance.internalWidth) | 200 RR frames | $($r.frameGeneration.presentedFrames) actual presents | water preserved"
        # This checks settings and honest telemetry. Enabled FG need not insert
        # frames; the HUD must expose that condition, not fabricate a multiplier.
    }finally{$p.Dispose()}
}
