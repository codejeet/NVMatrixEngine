param(
    [string]$RuntimeDirectory,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'profiles'),
    [ValidateRange(64, 100000)][int]$Frames = 240,
    [ValidateRange(320, 16384)][int]$Width = 1920,
    [ValidateRange(240, 16384)][int]$Height = 1080,
    [ValidateRange(1, 8)][int]$Paths = 1,
    [ValidateRange(1, 8)][int]$LightSamples = 2,
    [ValidateRange(1, 8)][int]$LightCandidates = 4,
    [ValidateRange(1, 8)][int]$Bounces = 4,
    [ValidateSet('quality', 'balanced', 'performance')][string]$Quality = 'quality',
    [ValidateSet('off', 'nvapi', 'dxr')][string]$Ser = 'off',
    [ValidateSet('ris', 'reference')][string]$Sampling = 'ris',
    [ValidateSet('on', 'off')][string]$Roulette = 'on',
    [switch]$Reference,
    [switch]$Sweep,
    [switch]$Capture
)
$ErrorActionPreference = 'Stop'

# Use the same runtime search order as Play Neon Night.cmd.
if (!$RuntimeDirectory) {
    foreach ($candidate in @($PSScriptRoot, (Split-Path $PSScriptRoot),
        "$env:LOCALAPPDATA/NVMatrixEngineHgi/build/bin/Release",
        "$env:LOCALAPPDATA/NVMatrixEngine/build/bin/Release",
        "$env:LOCALAPPDATA/NVMatrixModelTest/build/bin/Release")) {
        if ((Test-Path "$candidate/NVMatrixFluidLab.exe") -and
            (Test-Path "$candidate/assets/neon-night/neon-night.gltf") -and
            (Test-Path "$candidate/shaders/BloomVertical.dxil")) {
            $RuntimeDirectory = $candidate
            break
        }
    }
}
if (!$RuntimeDirectory -or !(Test-Path "$RuntimeDirectory/NVMatrixFluidLab.exe")) {
    throw 'No Neon Night runtime found. Build the engine or pass -RuntimeDirectory.'
}
$RuntimeDirectory = (Resolve-Path $RuntimeDirectory).ProviderPath
$runDirectory = Join-Path $OutputDirectory (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
New-Item -ItemType Directory -Force $runDirectory | Out-Null
$runDirectory = (Resolve-Path $runDirectory).ProviderPath

function Measure-Column($Table, [string]$Column, [int]$Warmup) {
    $index = [array]::IndexOf(@($Table.columns), $Column)
    if ($index -lt 0) { throw "Missing timing column '$Column'; rebuild the engine." }
    $values = @($Table.rows | Where-Object { $_[0] -ge $Warmup } |
        ForEach-Object { [double]$_[$index] } | Sort-Object)
    if (!$values.Count) { throw "No steady-state samples for '$Column'." }
    foreach ($value in $values) {
        if ([double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -lt 0) {
            throw "Invalid timing in '$Column'."
        }
    }
    $mid = [int][Math]::Floor($values.Count / 2)
    $median = if ($values.Count % 2) { $values[$mid] } else { ($values[$mid - 1] + $values[$mid]) / 2 }
    [ordered]@{
        medianMs = [Math]::Round($median, 4)
        p95Ms = [Math]::Round($values[[int][Math]::Ceiling(.95 * $values.Count) - 1], 4)
        samples = $values.Count
    }
}

if ($Reference) { $Sampling = 'reference'; $Roulette = 'off' }
$cases = @(@{ name = 'configured'; paths = $Paths; lights = $LightSamples; bounces = $Bounces; sampling = $Sampling })
if ($Sweep) {
    $cases = @(
        @{ name = 'reference'; paths = $Paths; lights = $LightSamples; bounces = $Bounces; sampling = 'reference' },
        @{ name = 'ris'; paths = $Paths; lights = $LightSamples; bounces = $Bounces; sampling = 'ris' }
    )
}
$results = @()
$csvRows = @()
foreach ($case in $cases) {
    $name = "neon-profile-$($case.name)"
    $arguments = @('--scene=neon-night', "--quality=$Quality", '--frame-gen=off', "--ser=$Ser", '--atomics=fixed',
        '--profile-latency', "--frames=$Frames", "--width=$Width", "--height=$Height", "--name=$name",
        "--path-samples=$($case.paths)", "--light-samples=$($case.lights)", "--path-bounces=$($case.bounces)",
        "--light-candidates=$LightCandidates", "--light-sampling=$($case.sampling)", "--russian-roulette=$Roulette")
    if ($Capture) { $arguments += '--capture' }
    Write-Host "Profiling $($case.name) at ${Width}x${Height}; frame generation off."
    $process = Start-Process -FilePath "$RuntimeDirectory/NVMatrixFluidLab.exe" -ArgumentList $arguments -Wait -PassThru
    if ($process.ExitCode) { throw "Profile '$name' failed ($($process.ExitCode)); see $RuntimeDirectory/NVMatrixEngine.log." }
    $raw = Join-Path $runDirectory "$name.json"
    Copy-Item "$RuntimeDirectory/$name.json" $raw
    Copy-Item "$RuntimeDirectory/NVMatrixEngine.log" "$runDirectory/$name.log"
    if ($Capture) {
        Copy-Item "$RuntimeDirectory/$name.ppm" $runDirectory
        Copy-Item "$RuntimeDirectory/$name.inputs" $runDirectory
    }
    $report = Get-Content -Raw $raw | ConvertFrom-Json
    if (!$report.gpuFrameBreakdown -or !$report.latency) { throw 'Missing GPU/CPU profiling data; rebuild the engine.' }
    if ($report.frameGeneration.enabled -or $report.frameGeneration.extraPresents) { throw 'Frame generation must be disabled for this benchmark.' }
    $metrics = [ordered]@{}
    foreach ($metric in @('wholeFrame', 'rendererFrame', 'photons', 'atlasEma', 'camera', 'composite', 'dlssRR')) {
        $metrics[$metric] = Measure-Column $report.latency $metric $report.warmupFrames
    }
    foreach ($metric in @('totalGpu', 'setupAndAcceleration', 'postRRPreparation', 'bloom', 'presentationAndHud')) {
        $metrics[$metric] = Measure-Column $report.gpuFrameBreakdown $metric $report.warmupFrames
    }
    $result = [ordered]@{
        name = $case.name; adapter = $report.adapter; output = @($report.outputWidth, $report.outputHeight)
        internal = @($report.internalWidth, $report.internalHeight); dlssMode = $report.dlssMode
        frames = $report.frames; warmupFrames = $report.warmupFrames; sampling = $report.sampling
        adaptiveCacheBytes = $report.neeAT.allocatedBytes
        anyHitReference = $report.modelAnyHitReference; frameGeneration = $report.frameGeneration
        transportHash = $report.transportHash; arguments = $arguments; metrics = $metrics
        renderFpsFromMedian = [Math]::Round(1000 / $metrics.wholeFrame.medianMs, 2)
    }
    $results += $result
    foreach ($metric in $metrics.Keys) {
        $csvRows += [pscustomobject]@{
            case = $case.name; metric = $metric; medianMs = $metrics[$metric].medianMs
            p95Ms = $metrics[$metric].p95Ms; samples = $metrics[$metric].samples
        }
    }
    $results | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 "$runDirectory/summary.json"
    $csvRows | Export-Csv -NoTypeInformation -Encoding UTF8 "$runDirectory/timings.csv"
    Write-Host ("  Frame {0:N2} ms ({1:N1} rendered FPS), camera {2:N2} ms, GPU {3:N2} ms" -f
        $metrics.wholeFrame.medianMs, $result.renderFpsFromMedian, $metrics.camera.medianMs, $metrics.totalGpu.medianMs)
}
Write-Host "Raw reports, logs and timing summaries: $runDirectory"
