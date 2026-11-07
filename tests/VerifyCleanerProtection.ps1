param(
    [ValidateSet('Prepare','Verify','CrashRecover','Unload')][string]$Stage = 'Prepare',
    [string]$StateFile = (Join-Path $PSScriptRoot 'x64\cleaner-state.json'),
    [string]$CleanerName = 'Direct deletion attempts only'
)
$ErrorActionPreference = 'Stop'
$taskRainmeter = 'C:\Program Files\Rainmeter\Rainmeter.exe'
function Start-TestInstance($state) {
    $launch = [Diagnostics.ProcessStartInfo]::new($taskRainmeter)
    $launch.UseShellExecute = $false
    $launch.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $launch.ArgumentList.Add($state.Ini)
    $launch.Environment['TMP'] = $state.TempRoot
    $launch.Environment['TEMP'] = $state.TempRoot
    $process = [Diagnostics.Process]::Start($launch)
    $state.Pid = $process.Id
    $state.ProcessStarted = $process.StartTime.ToUniversalTime().ToString('o')
}
function Get-TestProcess($state) {
    $process = Get-Process -Id $state.Pid
    $started = [DateTime]$state.ProcessStarted
    if ($process.Path -ne $taskRainmeter -or $process.StartTime.ToUniversalTime().Ticks -ne $started.ToUniversalTime().Ticks) {
        throw 'Process identity changed; refusing to control it.'
    }
    return $process
}
function Wait-TestFrames($state, [string]$old = '') {
    $deadline = [DateTime]::UtcNow.AddSeconds(12)
    do {
        $folders = @(Get-ChildItem -LiteralPath $state.TempRoot -Directory -Filter 'Rainmeter-Animate-*' |
            Where-Object { $_.FullName -ne $old -and @(Get-ChildItem -LiteralPath $_.FullName -Filter 'frame-*.bmp').Count -eq 8 -and
                -not (Test-Path -LiteralPath (Join-Path $_.FullName 'loading.png')) })
        if ($folders.Count -eq 1) { return $folders[0].FullName }
        Start-Sleep -Milliseconds 30
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Expected one complete eight-frame animation.'
}
function Assert-TestFrames($state) {
    $frames = @(Get-ChildItem -LiteralPath $state.Folder -Filter 'frame-*.bmp')
    if ($frames.Count -ne 8) { throw 'Frame count changed during cleaner test.' }
    foreach ($frame in $frames) {
        if ((Get-FileHash -LiteralPath $frame.FullName).Hash -ne $state.Hashes.($frame.Name)) { throw 'Frame contents changed.' }
        try { [IO.File]::Delete($frame.FullName); throw 'Protected deletion unexpectedly succeeded.' }
        catch [IO.IOException] { if (($_.Exception.HResult -band 0xffff) -ne 32) { throw } }
    }
    $log = Join-Path $state.Root 'Rainmeter.log'
    $errors = @(Get-Content -LiteralPath $log | Where-Object { $_ -match '^ERRO' -and $_ -match 'ProtectedCleaner' })
    if ($errors.Count) { $errors | Write-Output; throw 'Rainmeter logged errors during protected playback.' }
}
if ($Stage -eq 'Prepare') {
    $id = [Guid]::NewGuid().ToString('N')
    $root = Join-Path $PSScriptRoot ('x64\CleanerTest\' + $id)
    $skin = Join-Path $root 'Skins\ProtectedCleaner'
    $plugins = Join-Path $root 'Plugins'
    $temp = Join-Path ([IO.Path]::GetTempPath()) ('Rainmeter-Animate-Test-' + $id)
    New-Item -ItemType Directory -Path $skin,$plugins,$temp -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\x64\Release\Animate.dll') -Destination (Join-Path $plugins 'Animate.dll')
    $source = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\examples\Animate\@Resources\DancingBanana.webp')).Path
    $stop = Join-Path $root 'unload.flag'
    [IO.File]::WriteAllText((Join-Path $skin 'control.lua'),
        "function Update() local f=io.open([[$stop]],'r'); if f then f:close(); SKIN:Bang('!DeactivateConfig','ProtectedCleaner'); end; return 0 end")
    $skinText = "[Rainmeter]`r`nUpdate=10`r`nOnRefreshAction=[!CommandMeasure Animate Start]`r`n[Control]`r`nMeasure=Script`r`nScriptFile=control.lua`r`n[Animate]`r`nMeasure=Plugin`r`nPlugin=Animate`r`nImageName=$source`r`nMeterName=Image`r`nLoop=-1`r`nDisplayLoading=1`r`n[Image]`r`nMeter=Image`r`nW=125`r`nH=125`r`n"
    [IO.File]::WriteAllText((Join-Path $skin 'Test.ini'),$skinText)
    $ini = Join-Path $root 'Rainmeter.ini'
    [IO.File]::WriteAllText($ini,"[Rainmeter]`r`nSkinPath=$root\Skins\`r`nLogging=1`r`nUpdateCheck=0`r`n[ProtectedCleaner]`r`nActive=1`r`nWindowX=-10000`r`nWindowY=-10000`r`nKeepOnScreen=0`r`n")
    $state = [PSCustomObject]@{Root=$root; Ini=$ini; TempRoot=$temp; Pid=0; ProcessStarted=''; Folder=''; Hashes=@{}; CrashRecovered=$false}
    Start-TestInstance $state
    $state.Folder = Wait-TestFrames $state
    foreach ($frame in Get-ChildItem -LiteralPath $state.Folder -Filter 'frame-*.bmp') {
        $state.Hashes[$frame.Name] = (Get-FileHash -LiteralPath $frame.FullName).Hash
        # Old timestamps ensure age-based cleanup is exercised, without changing pixels.
        [IO.File]::SetLastWriteTimeUtc($frame.FullName,[DateTime]::UtcNow.AddDays(-10))
        [IO.File]::SetCreationTimeUtc($frame.FullName,[DateTime]::UtcNow.AddDays(-10))
    }
    [IO.Directory]::SetLastWriteTimeUtc($state.Folder,[DateTime]::UtcNow.AddDays(-10))
    [IO.Directory]::SetLastWriteTimeUtc($temp,[DateTime]::UtcNow.AddDays(-10))
    $control = Join-Path $temp 'cleanup-control.tmp'
    [IO.File]::WriteAllText($control,'Unprotected cleanup test control')
    [IO.File]::SetLastWriteTimeUtc($control,[DateTime]::UtcNow.AddDays(-10))
    [IO.File]::SetCreationTimeUtc($control,[DateTime]::UtcNow.AddDays(-10))
    $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $StateFile
    Write-Output 'Protected animation ready in Windows Temp for cleaner testing.'
    Write-Output $state.Folder
} else {
    $state = Get-Content -LiteralPath $StateFile | ConvertFrom-Json
    $expected = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'x64\CleanerTest')).TrimEnd('\') + '\'
    if (-not $state.Root.StartsWith($expected,[StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected test state path.' }
    $process = Get-TestProcess $state
    if ($Stage -eq 'Verify') {
        Assert-TestFrames $state
        [PSCustomObject]@{Version='1.8.0'; TestMethod=$CleanerName; Frames=8; AllHashesUnchanged=$true; DeletionDenied=$true; RainmeterErrors=0;
            UnprotectedControlRemoved=(-not (Test-Path -LiteralPath (Join-Path $state.TempRoot 'cleanup-control.tmp')))} |
            ConvertTo-Json | Tee-Object -LiteralPath (Join-Path $PSScriptRoot 'cleaner-protection-results.json')
    } elseif ($Stage -eq 'CrashRecover') {
        $old = $state.Folder
        Stop-Process -Id $process.Id
        if (-not $process.WaitForExit(5000)) { throw 'Isolated test process did not exit.' }
        foreach ($frame in Get-ChildItem -LiteralPath $old -Filter 'frame-*.bmp') {
            $lease = [IO.File]::Open($frame.FullName,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::ReadWrite)
            $lease.Dispose() # No bytes are changed; Windows released the process's locks.
        }
        Start-TestInstance $state
        $state.Folder = Wait-TestFrames $state $old
        if (Test-Path -LiteralPath $old) { throw 'Crash-orphaned frame folder was not recovered.' }
        Assert-TestFrames $state
        $state.CrashRecovered = $true
        $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $StateFile
        Write-Output 'Forced process exit released all frame handles; restart recovered the orphan and resumed playback.'
    } elseif ($Stage -eq 'Unload') {
        [IO.File]::WriteAllText((Join-Path $state.Root 'unload.flag'),'Unload isolated test skin')
        $deadline = [DateTime]::UtcNow.AddSeconds(8)
        while ((Test-Path -LiteralPath $state.Folder) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 30 }
        if (Test-Path -LiteralPath $state.Folder) { throw 'Normal unload did not release and delete protected frames.' }
        Stop-Process -Id $process.Id
        $control = Join-Path $state.TempRoot 'cleanup-control.tmp'
        if (Test-Path -LiteralPath $control) { Remove-Item -LiteralPath $control }
        if (@(Get-ChildItem -LiteralPath $state.TempRoot -Force).Count -ne 0) { throw 'Unexpected files in private test Temp; left untouched.' }
        [IO.Directory]::Delete($state.TempRoot)
        [PSCustomObject]@{Version='1.8.0'; CrashHandlesReleased=$state.CrashRecovered; OrphanRecovered=$state.CrashRecovered; NormalUnloadRemovedFrames=$true} |
            ConvertTo-Json | Tee-Object -LiteralPath (Join-Path $PSScriptRoot 'protected-cleanup-results.json')
    }
}
