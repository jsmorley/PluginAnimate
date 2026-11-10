param([string]$Plugin = (Join-Path $PSScriptRoot '..\x64\Release\Animate.dll'))
$ErrorActionPreference = 'Stop'
$taskRoot = Join-Path $PSScriptRoot ('x64\NativeCreation\' + [Guid]::NewGuid().ToString('N'))
$taskSkin = Join-Path $taskRoot 'Skins\Creation'
$taskResources = Join-Path $taskSkin '@Resources'
$taskPlugins = Join-Path $taskRoot 'Plugins'
New-Item -ItemType Directory -Force -Path $taskResources,$taskPlugins | Out-Null
Copy-Item -LiteralPath $Plugin -Destination (Join-Path $taskPlugins 'Animate.dll')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fixtures\blend.webp') -Destination (Join-Path $taskResources 'valid.webp')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fixtures\bad.apng') -Destination (Join-Path $taskResources 'corrupt.apng')
$taskText = @'
[Rainmeter]
Update=10
[Variables]
Pending=
[Clock]
Measure=Calc
Formula=Counter
IfCondition=Clock = 100
IfTrueAction=[!CommandMeasure Static "Start"]
[Static]
Measure=Plugin
Plugin=Animate
ImageName=#@#valid.webp
MeterName=StaticImage
DisplayLoading=1
Loop=-1
[Missing]
Measure=Plugin
Plugin=Animate
ImageName=#@#missing.webp
MeterName=MissingImage
[Corrupt]
Measure=Plugin
Plugin=Animate
ImageName=#@#corrupt.apng
MeterName=CorruptImage
DisplayLoading=1
[Directory]
Measure=Plugin
Plugin=Animate
ImageName=#@#
MeterName=DirectoryImage
[Empty]
Measure=Plugin
Plugin=Animate
ImageName=
MeterName=EmptyImage
[ExtensionWebp]
Measure=Plugin
Plugin=Animate
ImageName=#@##Pending#.webp
MeterName=ExtensionWebpImage
[ExtensionApng]
Measure=Plugin
Plugin=Animate
ImageName=#@#.apng
MeterName=ExtensionApngImage
[ExtensionGif]
Measure=Plugin
Plugin=Animate
ImageName=#@#.gif
MeterName=ExtensionGifImage
[StaticImage]
Meter=Image
W=10
H=10
[MissingImage]
Meter=Image
W=10
H=10
[CorruptImage]
Meter=Image
W=10
H=10
[DirectoryImage]
Meter=Image
W=10
H=10
[EmptyImage]
Meter=Image
W=10
H=10
[ExtensionWebpImage]
Meter=Image
W=10
H=10
[ExtensionApngImage]
Meter=Image
W=10
H=10
[ExtensionGifImage]
Meter=Image
W=10
H=10
[Probe]
Measure=Script
ScriptFile=#@#Probe.lua
'@
[IO.File]::WriteAllText((Join-Path $taskSkin 'Creation.ini'),$taskText)
$taskProbe = @'
local tick=0
local first=''
local moved=false
local premature=false
function Initialize()
    local f=io.open(SKIN:GetVariable('@') .. 'results.txt','w'); f:close()
end
function Record(label,pass)
    local f=io.open(SKIN:GetVariable('@') .. 'results.txt','a')
    f:write((pass and 'PASS' or 'FAIL') .. '|' .. label .. '\n'); f:close()
end
function Update()
    tick=tick+1
    local image=SKIN:GetMeter('StaticImage'):GetOption('ImageName','')
    if tick==70 then
        first=image
        local last=image:gsub('frame%-0%.bmp$','frame-3.bmp')
        local f=last~='' and io.open(last,'rb')
        Record('AllFramesLoadedAndOnlyFrame1Displayed',image:match('frame%-0%.bmp$')~=nil and f~=nil)
        if f then f:close() end
        Record('CreationErrorsDoNotSendImages',SKIN:GetMeter('MissingImage'):GetOption('ImageName','')=='' and SKIN:GetMeter('CorruptImage'):GetOption('ImageName','')=='' and SKIN:GetMeter('DirectoryImage'):GetOption('ImageName','')=='' and SKIN:GetMeter('EmptyImage'):GetOption('ImageName','')=='')
        Record('ExtensionOnlyInputsRemainSilent',SKIN:GetMeter('ExtensionWebpImage'):GetOption('ImageName','')=='' and SKIN:GetMeter('ExtensionApngImage'):GetOption('ImageName','')=='' and SKIN:GetMeter('ExtensionGifImage'):GetOption('ImageName','')=='')
    elseif tick>70 and tick<100 and image~=first then premature=true
    elseif tick>100 and tick<200 and image~='' and image~=first then moved=true
    elseif tick==220 then Record('StillUntilStartThenPlays',not premature and moved)
    elseif tick==250 then SKIN:Bang('!Quit') end
    return tick
end
'@
[IO.File]::WriteAllText((Join-Path $taskResources 'Probe.lua'),$taskProbe)
$taskSettings = Join-Path $taskRoot 'Rainmeter.ini'
[IO.File]::WriteAllText($taskSettings,"[Rainmeter]`nSkinPath=$taskRoot\Skins\`nLogging=1`n[Creation]`nActive=1`nWindowX=-10000`nWindowY=-10000`nKeepOnScreen=0`n")
$taskProcess = Start-Process -FilePath 'C:\Program Files\Rainmeter\Rainmeter.exe' -ArgumentList ('"' + $taskSettings + '"') -WindowStyle Hidden -PassThru
try {
    if (-not $taskProcess.WaitForExit(15000)) { throw 'Native creation test timed out' }
    $taskResults = @(Get-Content (Join-Path $taskResources 'results.txt'))
    $taskErrors = @(Get-Content (Join-Path $taskRoot 'Rainmeter.log') | Where-Object { $_ -match '^ERRO' })
    $taskResults | Write-Output
    if ($taskResults.Count -ne 4 -or @($taskResults | Where-Object { $_ -notmatch '^PASS\|' }).Count -or
        $taskErrors.Count -ne 4 -or @($taskErrors | Where-Object { $_ -notmatch 'Animate:' }).Count) {
        $taskErrors | Select-Object -First 8 | Write-Output
        throw 'Native creation checks failed'
    }
    [PSCustomObject]@{Version=(Get-Item -LiteralPath $Plugin).VersionInfo.FileVersion; PrefetchBeforeStart=$true; FirstFrameOnly=$true;
        StartsOnlyOnCommand=$true; InvalidCreationErrors=$taskErrors.Count; ImageMeterErrors=0; Passed=$true} |
        ConvertTo-Json | Set-Content (Join-Path $PSScriptRoot 'creation-results.json')
} finally {
    if (-not $taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id }
}
