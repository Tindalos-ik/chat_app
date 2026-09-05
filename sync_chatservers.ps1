[CmdletBinding()]
param(
    [ValidateSet('ChatServer1', 'ChatServer2')]
    [string]$Source = 'ChatServer1',

    [ValidateSet('ChatServer1', 'ChatServer2')]
    [string]$Destination = 'ChatServer2',

    # Copy by default; use -Apply:$false for a dry-run.
    [switch]$Apply = $true
)

$ErrorActionPreference = 'Stop'

if ($Source -eq $Destination) {
    throw 'Source and Destination must be different ChatServer directories.'
}

$repoRoot = $PSScriptRoot
$sourceRoot = Join-Path $repoRoot $Source
$destinationRoot = Join-Path $repoRoot $Destination

if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) {
    throw "Source directory not found: $sourceRoot"
}
if (-not (Test-Path -LiteralPath $destinationRoot -PathType Container)) {
    throw "Destination directory not found: $destinationRoot"
}

# Keep per-instance configuration and project files independent. Only shared C++ code is copied.
$relativeFiles = @()
$rootCode = Join-Path $sourceRoot 'ChatServer.cpp'
if (Test-Path -LiteralPath $rootCode -PathType Leaf) {
    $relativeFiles += 'ChatServer.cpp'
}

foreach ($directory in @('include', 'src')) {
    $directoryPath = Join-Path $sourceRoot $directory
    if (-not (Test-Path -LiteralPath $directoryPath -PathType Container)) {
        continue
    }

    $relativeFiles += Get-ChildItem -LiteralPath $directoryPath -Recurse -File |
        ForEach-Object {
            $_.FullName.Substring($sourceRoot.Length).TrimStart('\', '/')
        }
}

$relativeFiles = $relativeFiles | Sort-Object -Unique
$changed = 0
$unchanged = 0

foreach ($relativePath in $relativeFiles) {
    $sourcePath = Join-Path $sourceRoot $relativePath
    $destinationPath = Join-Path $destinationRoot $relativePath

    $needsCopy = $true
    if (Test-Path -LiteralPath $destinationPath -PathType Leaf) {
        $sourceHash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
        $destinationHash = (Get-FileHash -LiteralPath $destinationPath -Algorithm SHA256).Hash
        $needsCopy = $sourceHash -ne $destinationHash
    }

    if (-not $needsCopy) {
        $unchanged++
        continue
    }

    $changed++
    if ($Apply) {
        $destinationDirectory = Split-Path -Parent $destinationPath
        New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
        Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
        Write-Host "Copied $relativePath"
    }
    else {
        Write-Host "Would copy $relativePath"
    }
}

$mode = if ($Apply) { 'applied' } else { 'dry-run' }
Write-Host "Sync $Source -> $Destination ($mode): $changed changed, $unchanged unchanged."
if (-not $Apply) {
    Write-Host 'No files were modified. Re-run with -Apply to copy the listed files.'
}
