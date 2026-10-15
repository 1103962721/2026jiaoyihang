param(
    [string]$SourceExe = (Join-Path $PSScriptRoot 'build\qt-release\DF_PlayClick_Client_Qt\release\DF_PlayClick_Client_Qt.exe'),
    [string]$ReleaseDirectory = (Join-Path $PSScriptRoot 'Relase')
)
$ErrorActionPreference = 'Stop'
$sourceFile = Get-Item -LiteralPath $SourceExe -ErrorAction Stop
if ($sourceFile.Length -eq 0) { throw 'Empty client build output' }
New-Item -ItemType Directory -Force -Path $ReleaseDirectory | Out-Null
$releaseRoot = (Resolve-Path -LiteralPath $ReleaseDirectory).Path
$stem = 'DF_PlayClick_Client_Qt_Andirons'
$statePath = Join-Path $releaseRoot ($stem + '.version.json')
$previous = @(Get-ChildItem -LiteralPath $releaseRoot -File | Where-Object {
    $_.Name -eq ($stem + '.exe') -or $_.Name -match ('^' + $stem + '_1\.([0-9]+)\.exe$')
})
$lastMinor = -1
if (Test-Path -LiteralPath $statePath) {
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    if ($state.major -ne 1 -or [string]$state.minor -notmatch '^[0-9]+$') { throw 'Invalid version record' }
    $lastMinor = [int]$state.minor
}
foreach ($file in $previous) {
    if ($file.Name -match '_1\.([0-9]+)\.exe$') { $lastMinor = [Math]::Max($lastMinor, [int]$Matches[1]) }
}
$minor = $lastMinor + 1
$outputPath = Join-Path $releaseRoot ($stem + '_1.' + $minor + '.exe')
Copy-Item -LiteralPath $sourceFile.FullName -Destination $outputPath -ErrorAction Stop
$sourceHash = (Get-FileHash -LiteralPath $sourceFile.FullName -Algorithm SHA256).Hash
$releaseHash = (Get-FileHash -LiteralPath $outputPath -Algorithm SHA256).Hash
if ($sourceHash -ne $releaseHash) { throw 'Published client SHA256 mismatch; old products retained' }
[ordered]@{ major=1; minor=$minor; file=[IO.Path]::GetFileName($outputPath); sha256=$releaseHash } |
    ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding UTF8
foreach ($file in $previous) {
    if ([IO.Path]::GetDirectoryName($file.FullName) -ne $releaseRoot) { throw 'Old product outside release directory' }
    Remove-Item -LiteralPath $file.FullName -ErrorAction Stop
}
[PSCustomObject]@{ Version="1.$minor"; Path=$outputPath; SHA256=$releaseHash; Removed=$previous.Name }
