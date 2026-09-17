param([Parameter(Mandatory = $true)][string]$InstallRoot)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$releaseBase = 'https://nodejs.org/dist/latest-v24.x'
$parent = Split-Path -Parent $InstallRoot
New-Item -ItemType Directory -Force -Path $parent | Out-Null
$staging = Join-Path $parent ('.node-download-' + [guid]::NewGuid().ToString('N'))
$archive = Join-Path $staging 'node.zip'
$expanded = Join-Path $staging 'expanded'

try {
    Write-Output 'Reading official Node.js LTS checksums...'
    $manifest = (Invoke-WebRequest -Uri "$releaseBase/SHASUMS256.txt" -UseBasicParsing -TimeoutSec 60).Content
    $match = [regex]::Match($manifest, '(?m)^([0-9a-fA-F]{64})\s+(node-v24\.\d+\.\d+-win-x64\.zip)\s*$')
    if (-not $match.Success) { throw 'Windows x64 Node.js archive not found in checksums' }
    $expectedHash = $match.Groups[1].Value
    $fileName = $match.Groups[2].Value

    New-Item -ItemType Directory -Force -Path $staging | Out-Null
    Write-Output "Downloading $fileName..."
    Invoke-WebRequest -Uri "$releaseBase/$fileName" -OutFile $archive -UseBasicParsing -TimeoutSec 300
    $actualHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    if ($actualHash -ine $expectedHash) { throw 'Node.js SHA-256 checksum mismatch' }

    Write-Output 'Extracting verified Node.js archive...'
    Expand-Archive -LiteralPath $archive -DestinationPath $expanded -Force
    $folder = Join-Path $expanded ([IO.Path]::GetFileNameWithoutExtension($fileName))
    if (-not (Test-Path -LiteralPath (Join-Path $folder 'node.exe') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $folder 'node_modules\npm\bin\npm-cli.js') -PathType Leaf)) {
        throw 'Downloaded Node.js archive is incomplete'
    }

    if (Test-Path -LiteralPath $InstallRoot) { Remove-Item -LiteralPath $InstallRoot -Recurse -Force }
    Move-Item -LiteralPath $folder -Destination $InstallRoot
    Write-Output 'Node.js and npm are ready locally.'
} finally {
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
}
