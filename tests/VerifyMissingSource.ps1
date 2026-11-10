param(
    [string]$Plugin = (Join-Path $PSScriptRoot '..\x64\Release\Animate.dll'),
    [string]$Rainmeter = 'C:\Program Files\Rainmeter\Rainmeter.exe',
    [switch]$DeleteFrames
)
$ErrorActionPreference = 'Stop'
$taskRoot = Join-Path $PSScriptRoot ('x64\NativeLog\' + [Guid]::NewGuid().ToString('N'))
$taskSkinRoot = Join-Path $taskRoot 'Skins'
$taskSkin = Join-Path $taskSkinRoot 'MissingWeather'
$taskPlugins = Join-Path $taskRoot 'Plugins'
New-Item -ItemType Directory -Path $taskSkin,$taskPlugins -Force | Out-Null
Copy-Item -LiteralPath $Plugin -Destination (Join-Path $taskPlugins 'Animate.dll')
$taskGood = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\examples\Animate\@Resources\DancingBanana.webp')).Path
$taskMissing = Join-Path $taskSkin '.webp'
$taskText = @'
[Rainmeter]
Update=10
OnRefreshAction=[!CommandMeasure Animate "Start"]
[Clock]
Measure=Calc
Formula=Counter
IfCondition=Clock = 100
IfTrueAction=[!CommandMeasure Animate "Start"]
IfCondition2=Clock = 200
IfTrueAction2=[!CommandMeasure Animate "Start"]
IfCondition3=Clock = 300
IfTrueAction3=[!SetOption Animate ImageName "__GOOD__"][!UpdateMeasure Animate]
IfCondition4=Clock = 400
IfTrueAction4=[!SetOption Animate ImageName "__MISSING__"][!UpdateMeasure Animate]
IfCondition5=Clock = 500
IfTrueAction5=[!Quit]
[Animate]
Measure=Plugin
Plugin=Animate
ImageName=__MISSING__
MeterName=Image
Loop=-1
DynamicVariables=1
[Image]
Meter=Image
W=10
H=10
'@
if ($DeleteFrames) {
    $taskText = @'
[Rainmeter]
Update=10
OnRefreshAction=[!CommandMeasure Animate "Start"]
[Clock]
Measure=Calc
Formula=Counter
IfCondition=Clock = 500
IfTrueAction=[!Quit]
[Animate]
Measure=Plugin
Plugin=Animate
ImageName=__GOOD__
MeterName=Image
Loop=-1
Speed=30000
[Image]
Meter=Image
W=10
H=10
'@
}
[IO.File]::WriteAllText((Join-Path $taskSkin 'Test.ini'),$taskText.Replace('__GOOD__',$taskGood).Replace('__MISSING__',$taskMissing))
$taskIni = Join-Path $taskRoot 'Rainmeter.ini'
$taskSettings = "[Rainmeter]`r`nSkinPath=$taskSkinRoot\`r`nLogging=1`r`nUpdateCheck=0`r`n[MissingWeather]`r`nActive=1`r`nWindowX=-10000`r`nWindowY=-10000`r`nKeepOnScreen=0`r`n"
[IO.File]::WriteAllText($taskIni,$taskSettings)
# A separate settings file gives this test its own Rainmeter instance and log.
$taskTemp = Join-Path $taskRoot 'WindowsTemp'
New-Item -ItemType Directory -Path $taskTemp -Force | Out-Null
$taskLaunch = [Diagnostics.ProcessStartInfo]::new($Rainmeter)
$taskLaunch.UseShellExecute = $false
$taskLaunch.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$taskLaunch.ArgumentList.Add($taskIni)
$taskLaunch.Environment['TMP'] = $taskTemp
$taskLaunch.Environment['TEMP'] = $taskTemp
$taskProcess = [Diagnostics.Process]::Start($taskLaunch)
try {
    if ($DeleteFrames) {
        $taskDeadline = [DateTime]::UtcNow.AddSeconds(8)
        do {
            Start-Sleep -Milliseconds 20
            $taskFrames = @(Get-ChildItem -LiteralPath $taskTemp -Recurse -Filter 'frame-*.bmp' -ErrorAction SilentlyContinue)
        } while ($taskFrames.Count -lt 8 -and [DateTime]::UtcNow -lt $taskDeadline)
        if ($taskFrames.Count -ne 8) { throw 'Test frames were not extracted.' }
        $taskResolved = (Resolve-Path -LiteralPath $taskTemp).Path
        $taskAllowed = [IO.Path]::GetFullPath($taskRoot).TrimEnd('\') + '\'
        if (-not $taskResolved.StartsWith($taskAllowed,[StringComparison]::OrdinalIgnoreCase)) { throw 'Deletion target escaped isolated test directory.' }
        $taskDeleteErrors = @()
        Remove-Item -LiteralPath $taskResolved -Recurse -Force -ErrorAction SilentlyContinue -ErrorVariable taskDeleteErrors
        if (-not $taskDeleteErrors.Count -or @(Get-ChildItem -LiteralPath $taskTemp -Recurse -Filter 'frame-*.bmp').Count -ne 8) {
            throw 'Expected cleanup to be denied and all eight frames to remain.'
        }
    }
    if (-not $taskProcess.WaitForExit(20000)) { throw 'Isolated Rainmeter test did not finish.' }
    $taskLog = Join-Path $taskRoot 'Rainmeter.log'
    $taskErrors = @(Get-Content -LiteralPath $taskLog | Where-Object { $_ -match '^ERRO' -and $_ -match 'MissingWeather' })
    $taskExpected = if ($DeleteFrames) { 0 } else { 4 }
    $taskPattern = if ($DeleteFrames) { 'Animate: Extracted frames are missing' } else { 'Animate: Cannot open animation file' }
    $taskResult = [PSCustomObject]@{
        Version = (Get-Item -LiteralPath $Plugin).VersionInfo.FileVersion
        SkinUpdateMilliseconds = 10
        SkinUpdates = 500
        ExplicitStartCommands = if ($DeleteFrames) { 1 } else { 3 }
        ErrorsObserved = $taskErrors.Count
        OnlyExpectedErrors = @($taskErrors | Where-Object { $_ -notmatch $taskPattern }).Count -eq 0
        DeletionAttemptBlocked = [bool]$DeleteFrames
    }
    $taskResult | ConvertTo-Json | Write-Output
    if ($taskErrors.Count -ne $taskExpected -or -not $taskResult.OnlyExpectedErrors) {
        $taskErrors | Write-Output
        throw 'Expected only bounded creation/Start/source-change errors and no Image meter errors.'
    }
    $taskName = if ($DeleteFrames) { 'protected-frames-results.json' } else { 'missing-source-results.json' }
    $taskResult | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot $taskName) -Encoding utf8
} finally {
    if (-not $taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id }
}
