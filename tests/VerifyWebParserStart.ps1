param([string]$Plugin = (Join-Path $PSScriptRoot '..\x64\Release\Animate.dll'))
$ErrorActionPreference = 'Stop'
$taskRoot = Join-Path $PSScriptRoot ('x64\NativeWebParser\' + [Guid]::NewGuid().ToString('N'))
$taskSkin = Join-Path $taskRoot 'Skins\WeatherStartup'
$taskResources = Join-Path $taskSkin '@Resources'
$taskPlugins = Join-Path $taskRoot 'Plugins'
New-Item -ItemType Directory -Force -Path $taskResources,$taskPlugins | Out-Null
Copy-Item -LiteralPath $Plugin -Destination (Join-Path $taskPlugins 'Animate.dll')
$taskFixture = Join-Path $PSScriptRoot 'fixtures\blend.webp'
Copy-Item -LiteralPath $taskFixture -Destination (Join-Path $taskResources 'first.webp')
Copy-Item -LiteralPath $taskFixture -Destination (Join-Path $taskResources 'second.webp')
Set-Content -LiteralPath (Join-Path $taskSkin 'first.txt') -Value 'code=first' -Encoding ascii
Set-Content -LiteralPath (Join-Path $taskSkin 'second.txt') -Value 'code=second' -Encoding ascii
$taskText = @'
[Rainmeter]
Update=10
[Clock]
Measure=Calc
Formula=Counter
IfCondition=Clock = 100
IfTrueAction=[!EnableMeasure Parent][!CommandMeasure Parent "Update"]
IfCondition2=Clock = 250
IfTrueAction2=[!SetOption Parent URL "file://#CURRENTPATH#second.txt"][!CommandMeasure Parent "Update"]
[Parent]
Measure=WebParser
URL=file://#CURRENTPATH#first.txt
RegExp=code=(\w+)
Disabled=1
UpdateRate=10000
FinishAction=[!CommandMeasure Animate "Start"]
[Icon]
Measure=WebParser
URL=[Parent]
StringIndex=1
[Animate]
Measure=Plugin
Plugin=Animate
ImageName=#@#[Icon].webp
MeterName=Image
Loop=-1
[Image]
Meter=Image
W=10
H=10
[Probe]
Measure=Script
ScriptFile=#@#Probe.lua
'@
[IO.File]::WriteAllText((Join-Path $taskSkin 'Weather.ini'),$taskText)
$taskProbe = @'
local tick=0
function Initialize()
    local f=io.open(SKIN:GetVariable('@') .. 'results.txt','w');f:close()
end
function Record(label,expected)
    local source=SKIN:GetMeasure('Animate'):GetStringValue()
    local delay=SKIN:GetMeasure('Animate'):GetValue()
    local pass=expected=='' and source=='' and delay==0 or expected~='' and source:sub(-#expected)==expected and delay>0
    local f=io.open(SKIN:GetVariable('@') .. 'results.txt','a')
    f:write((pass and 'PASS' or 'FAIL') .. '|' .. label .. '|' .. source .. '|' .. delay .. '\n');f:close()
end
function Update()
    tick=tick+1
    if tick==70 then Record('DormantBeforeFinishAction','')
    elseif tick==220 then Record('FirstFinishActionWithoutDynamicVariables','first.webp')
    elseif tick==400 then Record('SecondFinishActionWithoutDynamicVariables','second.webp')
    elseif tick==450 then SKIN:Bang('!Quit') end
    return tick
end
'@
[IO.File]::WriteAllText((Join-Path $taskResources 'Probe.lua'),$taskProbe)
$taskSettings = Join-Path $taskRoot 'Rainmeter.ini'
[IO.File]::WriteAllText($taskSettings,"[Rainmeter]`nSkinPath=$taskRoot\Skins\`nLogging=1`n[WeatherStartup]`nActive=1`nWindowX=-10000`nWindowY=-10000`nKeepOnScreen=0`n")
$taskProcess = Start-Process -FilePath 'C:\Program Files\Rainmeter\Rainmeter.exe' -ArgumentList ('"' + $taskSettings + '"') -WindowStyle Hidden -PassThru
try {
    if (-not $taskProcess.WaitForExit(18000)) { throw 'Native WebParser test timed out' }
    $taskResults = @(Get-Content (Join-Path $taskResources 'results.txt'))
    $taskErrors = @(Get-Content (Join-Path $taskRoot 'Rainmeter.log') | Where-Object { $_ -match '^ERRO' })
    $taskResults | Write-Output
    if ($taskResults.Count -ne 3 -or @($taskResults | Where-Object { $_ -notmatch '^PASS\|' }).Count -or $taskErrors.Count) {
        $taskErrors | Select-Object -First 5 | Write-Output
        throw 'Native WebParser Start checks failed'
    }
    [PSCustomObject]@{ Version=(Get-Item -LiteralPath $Plugin).VersionInfo.FileVersion; DynamicVariables=$false; ChildUpdates=0; FinishAction='Start only'; Passed=$true; Errors=0 } | ConvertTo-Json | Set-Content (Join-Path $PSScriptRoot 'webparser-start-results.json')
} finally {
    if (-not $taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id }
}
