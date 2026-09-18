param([string]$OutputDirectory = 'Out')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root $OutputDirectory
New-Item -ItemType Directory -Force -Path $out | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio Installer / vswhere.exe not found' }
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio C++ desktop tools not found' }
$devShell = Join-Path $vsRoot 'Common7\Tools\Launch-VsDevShell.ps1'
& $devShell -Arch amd64 -HostArch amd64 -NoLogo
$resource = Join-Path $out 'launcher.res'
$object = Join-Path $out 'launcher.obj'
$executable = Join-Path $out 'DeepSeekHarnessLauncher.exe'
Push-Location $PSScriptRoot
try {
    & rc.exe /nologo "/fo$resource" launcher.rc
    if ($LASTEXITCODE -ne 0) { throw 'rc failed' }
    & cl.exe /nologo /std:c++20 /utf-8 /O1 /GL /MT /EHsc /DUNICODE /D_UNICODE /c launcher.cpp "/Fo$object"
    if ($LASTEXITCODE -ne 0) { throw 'cl failed' }
    & link.exe /nologo /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS "/OUT:$executable" $object $resource user32.lib gdi32.lib gdiplus.lib dwmapi.lib shell32.lib ole32.lib uuid.lib comctl32.lib iphlpapi.lib ws2_32.lib winhttp.lib bcrypt.lib advapi32.lib version.lib
    if ($LASTEXITCODE -ne 0) { throw 'link failed' }
} finally {
    Pop-Location
    if (Test-Path -LiteralPath $resource) { Remove-Item -LiteralPath $resource }
    if (Test-Path -LiteralPath $object) { Remove-Item -LiteralPath $object }
}
Get-Item -LiteralPath $executable | Select-Object FullName,Length
