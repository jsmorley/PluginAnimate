param(
    [Parameter(Mandatory = $true)][string]$GifPath,
    [int]$DurationSeconds = 20,
    [int]$ExpectedFrames = 0
)
$ErrorActionPreference = 'Stop'
$animationPath = (Resolve-Path -LiteralPath $GifPath).Path
$rainmeterExecutable = (Get-Process Rainmeter | Select-Object -First 1).Path
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class AnimateUiProbe {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, string windowName);
    [DllImport("user32.dll", SetLastError=true)]
    public static extern IntPtr SendMessageTimeout(IntPtr window, uint message,
        UIntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
}
'@
$window = [AnimateUiProbe]::FindWindow('DummyRainWClass', 'Rainmeter control window')
if ($window -eq [IntPtr]::Zero) { throw 'Rainmeter control window not found.' }
$tempRoot = [IO.Path]::GetTempPath()
$before = @{}
$wasActive = $false
$inSection = $false
foreach ($line in Get-Content -LiteralPath (Join-Path $env:APPDATA 'Rainmeter\Rainmeter.ini')) {
    if ($line -match '^\[(.+)\]$') { $inSection = $Matches[1] -eq 'Animate' }
    elseif ($inSection -and $line -match '^Active=(\d+)') { $wasActive = [int]$Matches[1] -ne 0 }
}
$responses = @()
$timeouts = 0
$previewSeen = $false
$candidate = $null
function Send-RainmeterBang([string]$arguments) {
    $commandProcess = Start-Process -FilePath $rainmeterExecutable -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (-not $commandProcess.WaitForExit(5000)) { throw 'Rainmeter command did not finish promptly.' }
}
try {
    if (-not $wasActive) { Send-RainmeterBang '!ActivateConfig Animate Animate.ini' }
    Start-Sleep -Milliseconds 500
    Get-ChildItem -LiteralPath $tempRoot -Directory -Filter 'Rainmeter-Animate-*' | ForEach-Object { $before[$_.FullName] = $true }
    Send-RainmeterBang ('!SetOption MeasureAnimate ImageName "' + $animationPath + '" Animate')
    Send-RainmeterBang '!SetOption MeasureAnimate DisplayLoading 1 Animate'
    Send-RainmeterBang '!UpdateMeasure MeasureAnimate Animate'
    Send-RainmeterBang '!CommandMeasure MeasureAnimate Start Animate'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt $DurationSeconds) {
        [UIntPtr]$reply = [UIntPtr]::Zero
        $probe = [Diagnostics.Stopwatch]::StartNew()
        $sent = [AnimateUiProbe]::SendMessageTimeout($window, 0, [UIntPtr]::Zero, [IntPtr]::Zero, 2, 250, [ref]$reply)
        $probe.Stop()
        if ($sent -eq [IntPtr]::Zero) { ++$timeouts }
        else { $responses += $probe.Elapsed.TotalMilliseconds }
        if (-not $candidate) {
            $candidate = Get-ChildItem -LiteralPath $tempRoot -Directory -Filter 'Rainmeter-Animate-*' |
                Where-Object { -not $before.ContainsKey($_.FullName) } | Select-Object -First 1 -ExpandProperty FullName
        }
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'loading.png'))) { $previewSeen = $true }
        Start-Sleep -Milliseconds 100
    }
    $frames = if ($candidate) { @(Get-ChildItem -LiteralPath $candidate -File -Filter 'frame-*.png').Count } else { 0 }
    $discarded = $candidate -and -not (Test-Path -LiteralPath (Join-Path $candidate 'loading.png'))
    $result = [PSCustomObject]@{
        File = [IO.Path]::GetFileName($animationPath)
        DurationSeconds = $watch.Elapsed.TotalSeconds
        SuccessfulUiResponses = $responses.Count
        UiTimeouts = $timeouts
        MaximumUiResponseMilliseconds = ($responses | Measure-Object -Maximum).Maximum
        AverageUiResponseMilliseconds = ($responses | Measure-Object -Average).Average
        PreviewObserved = $previewSeen
        FramesExtracted = $frames
        PreviewDiscarded = [bool]$discarded
    }
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'live-load-results.json') -Encoding utf8
    $result | ConvertTo-Json | Write-Output
    if ($timeouts) { throw 'Rainmeter failed one or more 250 ms responsiveness probes.' }
    if (-not $frames) { throw 'No extraction was observed; responsiveness alone is not a valid load test.' }
    if ($ExpectedFrames -gt 0 -and $frames -ne $ExpectedFrames) { throw "Expected $ExpectedFrames extracted frames, observed $frames." }
}
finally {
    # Restore the user's saved source/options without modifying the skin file.
    if ($wasActive) { Send-RainmeterBang '!Refresh Animate' }
    else { Send-RainmeterBang '!DeactivateConfig Animate' }
}
