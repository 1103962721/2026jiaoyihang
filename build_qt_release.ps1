$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$qtRoot = Join-Path $projectRoot '.qt\6.6.3\msvc2019_64_static'
$qmake = Join-Path $qtRoot 'bin\qmake.exe'
$vsDevCmd = 'E:\VisualStudio\VisualStudioIDE\Common7\Tools\VsDevCmd.bat'
$buildRoot = Join-Path $projectRoot 'build\qt-release'
$outputRoot = Join-Path $projectRoot 'Relase'
$projectDrive = 'P:'
$qtDrive = 'Q:'

if (-not (Test-Path -LiteralPath $qmake)) { throw "qmake not found: $qmake" }
if (-not (Test-Path -LiteralPath $vsDevCmd)) { throw "Visual Studio environment script not found: $vsDevCmd" }

New-Item -ItemType Directory -Force -Path $buildRoot, $outputRoot | Out-Null

# qmake/nmake do not reliably preserve this project's Unicode path in generated
# Makefiles. Use short ASCII drive mappings for the build and keep outputs in
# the real project directory.
& subst $projectDrive /d 2>$null
& subst $projectDrive $projectRoot
if ($LASTEXITCODE -ne 0) { throw "Unable to map $projectDrive to $projectRoot" }
& subst $qtDrive /d 2>$null
& subst $qtDrive $qtRoot
if ($LASTEXITCODE -ne 0) { throw "Unable to map $qtDrive to $qtRoot" }

$mappedQtRoot = "$qtDrive\"
$mappedProjectRoot = "$projectDrive\"
$mappedBuildRoot = Join-Path $mappedProjectRoot 'build\qt-release'
$mappedQmake = Join-Path $mappedQtRoot 'bin\qmake.exe'

function Invoke-NmakeTarget([string]$projectFile, [string]$targetName) {
    $targetBuild = Join-Path $mappedBuildRoot $targetName
    $physicalTargetBuild = Join-Path $buildRoot $targetName
    New-Item -ItemType Directory -Force -Path $physicalTargetBuild | Out-Null
    Push-Location $targetBuild
    try {
        & $mappedQmake $projectFile 'CONFIG+=release' 'CONFIG-=debug' '-spec' 'win32-msvc' '-r'
        if ($LASTEXITCODE -ne 0) { throw "qmake failed for $targetName" }
        nmake
        if ($LASTEXITCODE -ne 0) { throw "nmake failed for $targetName" }
    }
    finally {
        Pop-Location
    }
    $exeCandidates = @(
        (Join-Path $physicalTargetBuild "release\$targetName.exe"),
        (Join-Path $physicalTargetBuild "$targetName.exe")
    )
    $exe = $exeCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $exe) { throw "Build output not found under: $physicalTargetBuild" }
    Copy-Item -LiteralPath $exe -Destination (Join-Path $outputRoot "$targetName.exe") -Force
}

$env:PATH = "$mappedQtRoot\bin;E:\VisualStudio\VisualStudioIDE\MSBuild\Current\Bin;$env:PATH"
$env:QTDIR = $mappedQtRoot

cmd.exe /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 && set" | ForEach-Object {
    if ($_ -match '^(?<name>[^=]+)=(?<value>.*)$') {
        [Environment]::SetEnvironmentVariable($Matches.name, $Matches.value, 'Process')
    }
}

Invoke-NmakeTarget (Join-Path $mappedProjectRoot 'DF_PlayClick_Client_Qt\DF_PlayClick_Client_Qt.pro') 'DF_PlayClick_Client_Qt'
Invoke-NmakeTarget (Join-Path $mappedProjectRoot 'DF_PlayClick_Server_Qt\DF_PlayClick_Server_Qt.pro') 'DF_PlayClick_Server_Qt'

Write-Host "Built outputs:" -ForegroundColor Green
Write-Host (Join-Path $outputRoot 'DF_PlayClick_Client_Qt.exe')
Write-Host (Join-Path $outputRoot 'DF_PlayClick_Server_Qt.exe')
