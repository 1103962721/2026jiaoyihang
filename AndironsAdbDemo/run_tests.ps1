$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$qtRoot = Join-Path $projectRoot '.qt\6.6.3\msvc2019_64_static'
$vsDevCmd = 'E:\VisualStudio\VisualStudioIDE\Common7\Tools\VsDevCmd.bat'
$buildRoot = Join-Path $projectRoot 'build\andirons-demo-tests'
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
& subst P: /d 2>$null
& subst P: $projectRoot
if ($LASTEXITCODE -ne 0) { throw 'Unable to map project drive' }
& subst Q: /d 2>$null
& subst Q: $qtRoot
if ($LASTEXITCODE -ne 0) { throw 'Unable to map Qt drive' }
$devCommand = '"' + $vsDevCmd + '" -arch=x64 -host_arch=x64 && set'
cmd.exe /c $devCommand | ForEach-Object {
    if ($_ -match '^(?<name>[^=]+)=(?<value>.*)$') {
        [Environment]::SetEnvironmentVariable($Matches.name, $Matches.value, 'Process')
    }
}
Push-Location 'P:\build\andirons-demo-tests'
try {
    & 'Q:\bin\qmake.exe' 'P:\AndironsAdbDemo\SelectionTests.pro' 'CONFIG+=release' 'CONFIG-=debug' '-spec' 'win32-msvc'
    if ($LASTEXITCODE -ne 0) { throw 'test qmake failed' }
    nmake
    if ($LASTEXITCODE -ne 0) { throw 'test build failed' }
    & '.\release\AdbSelectionTests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'selection/client capture tests failed' }
}
finally { Pop-Location }
