$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$qtRoot = Join-Path $projectRoot '.qt\6.6.3\msvc2019_64_static'
$vsDevCmd = 'E:\VisualStudio\VisualStudioIDE\Common7\Tools\VsDevCmd.bat'
$buildRoot = Join-Path $projectRoot 'build\AndironsAdbDemo'
$output = Join-Path $PSScriptRoot 'AndironsAdbDemo.exe'

cmd.exe /c "subst P: /d" 2>$null
cmd.exe /c "subst P: `"$projectRoot`""
cmd.exe /c "subst Q: /d" 2>$null
cmd.exe /c "subst Q: `"$qtRoot`""
cmd.exe /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 && set" | ForEach-Object {
    if ($_ -match '^(?<name>[^=]+)=(?<value>.*)$') {
        [Environment]::SetEnvironmentVariable($Matches.name, $Matches.value, 'Process')
    }
}

New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
Push-Location 'P:\build\AndironsAdbDemo'
try {
    & 'Q:\bin\qmake.exe' 'P:\AndironsAdbDemo\AndironsAdbDemo.pro' 'CONFIG+=release' 'CONFIG-=debug' '-spec' 'win32-msvc'
    if ($LASTEXITCODE -ne 0) { throw 'qmake failed' }
    nmake
    if ($LASTEXITCODE -ne 0) { throw 'nmake failed' }
}
finally {
    Pop-Location
}

$built = Join-Path $buildRoot 'release\AndironsAdbDemo.exe'
if (-not (Test-Path $built)) { throw "Build output not found: $built" }
Copy-Item -LiteralPath $built -Destination $output -Force
Write-Host "Built: $output"
