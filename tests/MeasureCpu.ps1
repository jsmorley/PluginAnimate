param([int]$SampleSeconds = 30)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class AnimateCpuCounter {
    [DllImport("kernel32.dll", SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool QueryProcessCycleTime(IntPtr process, out ulong cycles);
}
'@
$rainmeterProcess = Get-Process Rainmeter | Select-Object -First 1
$rainmeterExecutable = $rainmeterProcess.Path
$logicalProcessors = [Environment]::ProcessorCount
$samples = @()
function Read-Counters {
    $rainmeterProcess.Refresh()
    [ulong]$cycles = 0
    if (-not [AnimateCpuCounter]::QueryProcessCycleTime($rainmeterProcess.Handle, [ref]$cycles)) {
        throw 'Unable to read Rainmeter process cycle count.'
    }
    [PSCustomObject]@{
        CpuSeconds = $rainmeterProcess.TotalProcessorTime.TotalSeconds
        Cycles = $cycles
        Timestamp = [Diagnostics.Stopwatch]::GetTimestamp()
    }
}
try {
    # Paused / playing / playing / paused brackets playback with a baseline.
    foreach ($state in @('pause', 'start', 'start', 'pause')) {
        & $rainmeterExecutable '!CommandMeasure' 'MeasureAnimate' $state 'Animate'
        Start-Sleep -Seconds 2
        $begin = Read-Counters
        Write-Output "Sampling $state for $SampleSeconds seconds..."
        for ($second = 0; $second -lt $SampleSeconds; ++$second) { Start-Sleep -Seconds 1 }
        $finish = Read-Counters
        $elapsed = ($finish.Timestamp - $begin.Timestamp) / [double][Diagnostics.Stopwatch]::Frequency
        $cpuSeconds = $finish.CpuSeconds - $begin.CpuSeconds
        $row = [PSCustomObject]@{
            State = if ($state -eq 'pause') { 'Paused' } else { 'Playing' }
            ElapsedSeconds = $elapsed
            CpuSeconds = $cpuSeconds
            CpuPercentMachine = 100 * $cpuSeconds / $elapsed / $logicalProcessors
            CpuPercentOneCore = 100 * $cpuSeconds / $elapsed
            CpuCycles = $finish.Cycles - $begin.Cycles
        }
        $samples += $row
        $row | ConvertTo-Json -Compress | Write-Output
    }
}
finally {
    & $rainmeterExecutable '!CommandMeasure' 'MeasureAnimate' 'start' 'Animate'
}
$result = [PSCustomObject]@{
    ProcessId = $rainmeterProcess.Id
    LogicalProcessors = $logicalProcessors
    SampleSecondsRequested = $SampleSeconds
    Samples = $samples
}
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'cpu-results.json') -Encoding utf8
Write-Output 'Measurement complete; Animate left playing.'
