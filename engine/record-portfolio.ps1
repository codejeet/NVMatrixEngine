param(
  [string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngineCUDA/build",
  [string]$OutputDir="$PSScriptRoot/../docs/videos",
  [string]$FFmpeg='ffmpeg',
  [ValidateRange(3,30)][int]$Seconds=14,
  [ValidateSet('all','water-room','wall-inlet','deep-pool','underwater')][string]$Scene='all'
)
$ErrorActionPreference='Stop'
if(Get-Process NVMatrixFluidLab -ErrorAction SilentlyContinue){throw 'Close the demo before recording.'}
$runtime=(Resolve-Path "$BuildDir/bin/Release").Path
$encoder=(Get-Command $FFmpeg -ErrorAction Stop).Source
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$OutputDir=[IO.Path]::GetFullPath((Resolve-Path $OutputDir).ProviderPath)
# Capture only the topmost demo's client rectangle with Desktop Duplication.
# HWND GDI capture can return stale flip-model frames. Inspect every take before
# publishing: desktop notifications can still occlude an application.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PortfolioWindow {
  [StructLayout(LayoutKind.Sequential)] public struct Point { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int command);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out Rect rect);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref Point point);
  [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
}
'@
$cases=@(
  @('water-room','--fluid-room --boat --fluid-emitter --demo-tour --fluid-depth=.55 --fluid-particles=200000'),
  @('wall-inlet','--fluid-room --boat --fluid-emitter --fluid-view'),
  @('deep-pool','--fluid-deep-pool'),
  @('underwater','--fluid-room --boat --underwater-view --orbit-test --fluid-depth=1.4 --fluid-particles=400000 --fluid-emitter')
)
foreach($case in $cases){
  if($Scene -ne 'all' -and $Scene -ne $case[0]){continue}
  $name=$case[0]
  $output=Join-Path $OutputDir "$name.mp4"
  if(Test-Path $output){throw "Refusing to overwrite recording: $output"}
  $argsList="--normal-lens --width=1280 --height=720 --quality=quality --frame-gen=off --frames=12000 --name=record-$name $($case[1])"
  $p=Start-Process "$runtime/NVMatrixFluidLab.exe" -ArgumentList $argsList -WorkingDirectory $runtime -PassThru
  $null=$p.Handle
  try{
    $timer=[Diagnostics.Stopwatch]::StartNew()
    do{
      Start-Sleep -Milliseconds 100
      $p.Refresh()
      if($p.HasExited){throw 'Demo exited before recording.'}
      if($timer.Elapsed.TotalSeconds -gt 30){throw 'Demo window did not appear.'}
    }while(!$p.MainWindowHandle)
    $null=[PortfolioWindow]::ShowWindow($p.MainWindowHandle,9)
    $null=[PortfolioWindow]::SetWindowPos($p.MainWindowHandle,[IntPtr]::new(-1),20,20,0,0,0x0041)
    $null=[PortfolioWindow]::SetForegroundWindow($p.MainWindowHandle)
    # Allow startup and reconstruction history to settle. Recording below uses
    # wall-clock screen capture: no per-render-frame readback or time stretching.
    Start-Sleep -Seconds 3
    $p.Refresh()
    $oldDpi=[PortfolioWindow]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))
    $point=[PortfolioWindow+Point]::new()
    $rect=[PortfolioWindow+Rect]::new()
    $null=[PortfolioWindow]::ClientToScreen($p.MainWindowHandle,[ref]$point)
    $null=[PortfolioWindow]::GetClientRect($p.MainWindowHandle,[ref]$rect)
    $null=[PortfolioWindow]::SetThreadDpiAwarenessContext($oldDpi)
    if($point.X -lt 0 -or $point.Y -lt 0 -or $rect.Right -ne 1280 -or $rect.Bottom -ne 720){throw 'Capture requires a 1280x720 client on the primary monitor.'}
    $inputName="ddagrab=output_idx=0:framerate=30:draw_mouse=0:video_size=1280x720:offset_x=$($point.X):offset_y=$($point.Y)"
    & $encoder -hide_banner -nostdin -n -f lavfi -i $inputName -t $Seconds -an `
      -c:v h264_nvenc -preset p4 -cq 21 -b:v 0 -movflags +faststart $output
    if($LASTEXITCODE){throw "Recording failed: $name"}
    Write-Host "RECORDED: $output"
  }finally{
    if(!$p.HasExited){
      $null=[PortfolioWindow]::PostMessage($p.MainWindowHandle,0x0010,[IntPtr]::Zero,[IntPtr]::Zero)
      if(!$p.WaitForExit(10000)){$p.Kill();$p.WaitForExit()}
    }
    $p.Dispose()
  }
}
