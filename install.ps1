# Salivo SDK installer.
#
# Installs the Salivo toolchain for the current user into %USERPROFILE%\.salivo, adds it to the user
# PATH, installs the VS Code extension (syntax highlighting, snippets, run/build/check commands) and
# gives .sal files the Salivo icon. Salivo links programs with clang and the Microsoft C++ libraries,
# so the installer checks for LLVM and the Visual Studio C++ build tools and offers to install them.
# Nothing outside the current user's profile is modified except by those optional installers.
#
#   -Quiet             answer yes to every question (unattended install)
#   -TargetHome <dir>  install below <dir> instead of %USERPROFILE% (testing)
#   -NoSystemChanges   skip PATH, VS Code, file association and prerequisite installs (testing)
#   -RepairFileIcons   only restore the .sal icon and association (if another program took them over)

param(
    [switch]$Quiet,
    [string]$TargetHome = $env:USERPROFILE,
    [switch]$NoSystemChanges,
    [switch]$RepairFileIcons
)

$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$root = Join-Path $TargetHome '.salivo'
$bin = Join-Path $root 'bin'
$total = 8

function Step([int]$n, [string]$text) { Write-Host "[$n/$total] $text" -ForegroundColor Cyan }
function Ok([string]$text) { Write-Host "      $text" -ForegroundColor Green }
function Warn([string]$text) { Write-Host "      $text" -ForegroundColor Yellow }
function Ask([string]$question) {
    if ($Quiet) { return $true }
    $answer = Read-Host "      $question [Y/n]"
    return ($answer -eq '' -or $answer -match '^[Yy]')
}
function Find-Clang {
    $cmd = Get-Command clang -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $default = Join-Path $env:ProgramFiles 'LLVM\bin\clang.exe'
    if (Test-Path $default) { return $default }
    return $null
}
function Find-VcTools {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($path) { return $path }
    return $null
}
function Install-WithWinget([string]$id, [string[]]$extra) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Warn "winget is not available; install $id manually."
        return
    }
    $wingetArgs = @('install', '-e', '--id', $id, '--accept-package-agreements', '--accept-source-agreements') + $extra
    & winget @wingetArgs
}

# Points .sal files at Salivo.File: the official Salivo icon, opening in VS Code. Keys are only created
# when missing (New-Item -Force would recreate an existing key and wipe other programs' values), and a
# handler another program had installed for .sal is reported and left in place for "Open with".
function Register-SalivoFiles {
    $ico = Join-Path $root 'salivo.ico'
    $shippedIco = Join-Path $src 'assets\salivo.ico'
    if (Test-Path $shippedIco) { Copy-Item -Force $shippedIco $ico }
    $classes = 'Registry::HKEY_CURRENT_USER\Software\Classes'
    foreach ($key in @("$classes\.sal", "$classes\.sal\OpenWithProgids", "$classes\Salivo.File",
                       "$classes\Salivo.File\DefaultIcon", "$classes\Salivo.File\shell\open\command")) {
        if (-not (Test-Path $key)) { New-Item -Path $key | Out-Null }
    }
    $previous = (Get-ItemProperty -Path "$classes\.sal" -ErrorAction SilentlyContinue).'(default)'
    Set-ItemProperty -Path "$classes\.sal" -Name '(default)' -Value 'Salivo.File'
    Set-ItemProperty -Path "$classes\.sal\OpenWithProgids" -Name 'Salivo.File' -Value ([byte[]]@())
    Set-ItemProperty -Path "$classes\Salivo.File" -Name '(default)' -Value 'Salivo Source File'
    Set-ItemProperty -Path "$classes\Salivo.File\DefaultIcon" -Name '(default)' -Value "$ico,0"
    $codeExe = Join-Path $env:LOCALAPPDATA 'Programs\Microsoft VS Code\Code.exe'
    if (-not (Test-Path $codeExe)) { $codeExe = Join-Path $env:ProgramFiles 'Microsoft VS Code\Code.exe' }
    if (Test-Path $codeExe) {
        Set-ItemProperty -Path "$classes\Salivo.File\shell\open\command" -Name '(default)' -Value "`"$codeExe`" `"%1`""
    }
    if (-not ('SalivoSetup.Shell' -as [type])) {
        Add-Type -Namespace SalivoSetup -Name Shell -MemberDefinition '[DllImport("shell32.dll")] public static extern void SHChangeNotify(int e, int f, System.IntPtr a, System.IntPtr b);'
    }
    [SalivoSetup.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($previous -and $previous -ne 'Salivo.File') {
        Warn "another program ('$previous') had taken over .sal files; restored the Salivo icon"
    }
    Ok '.sal files use the official Salivo icon and open in VS Code'
}

if ($RepairFileIcons) {
    Register-SalivoFiles
    exit 0
}

Write-Host ''
Write-Host '  ============================================' -ForegroundColor Magenta
Write-Host '           Salivo SDK - Windows x64 Setup' -ForegroundColor Magenta
Write-Host '  ============================================' -ForegroundColor Magenta
Write-Host "  Installing into $root"
Write-Host ''

if (-not (Test-Path (Join-Path $src 'bin\sf.exe'))) {
    Write-Host 'The SDK files were not found next to this installer.' -ForegroundColor Red
    Write-Host 'Extract the whole zip to a folder first, then run "Salivo Setup.exe" from there.' -ForegroundColor Red
    exit 1
}

# 1. Prerequisites -----------------------------------------------------------------------------
Step 1 'Checking prerequisites (LLVM clang, Visual Studio C++ build tools)'
$clang = Find-Clang
$vc = Find-VcTools
if ($clang) { Ok "clang: $clang" } else { Warn 'clang (LLVM) was not found.' }
if ($vc) { Ok "C++ build tools: $vc" } else { Warn 'Visual Studio C++ build tools were not found.' }
if (-not $NoSystemChanges) {
    if (-not $clang -and (Ask 'Install LLVM (clang) now with winget?')) {
        Install-WithWinget 'LLVM.LLVM' @()
        $clang = Find-Clang
    }
    if (-not $vc -and (Ask 'Install the Visual Studio 2022 C++ build tools now with winget? (large download)')) {
        Install-WithWinget 'Microsoft.VisualStudio.2022.BuildTools' @('--override', '--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
        $vc = Find-VcTools
    }
}
if (-not $clang -or -not $vc) {
    Warn 'Salivo will install, but it cannot build programs until both prerequisites are present:'
    Warn '  winget install -e --id LLVM.LLVM'
    Warn '  winget install -e --id Microsoft.VisualStudio.2022.BuildTools --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"'
}

# 2. Toolchain binaries ------------------------------------------------------------------------
Step 2 'Installing the compiler and tools (sf, Stage 3 compiler, spm, formatter, linter, docs, LSP)'
New-Item -ItemType Directory -Force -Path $bin | Out-Null
try {
    Copy-Item -Force -Path (Join-Path $src 'bin\*') -Destination $bin
} catch {
    Write-Host "Could not replace files in $bin (is sf or VS Code using them?). Close VS Code and terminals, then run setup again." -ForegroundColor Red
    exit 1
}
Ok "$((Get-ChildItem $bin).Count) files in $bin"

# 3. Standard library --------------------------------------------------------------------------
Step 3 'Installing the standard library'
foreach ($stdDir in @((Join-Path $root 'lib\salivo\std'), (Join-Path $root 'std'))) {
    if (Test-Path $stdDir) { Remove-Item -Recurse -Force $stdDir }
    New-Item -ItemType Directory -Force -Path $stdDir | Out-Null
    Copy-Item -Recurse -Force -Path (Join-Path $src 'std\*') -Destination $stdDir
}
Ok "$((Get-ChildItem (Join-Path $root 'std') -Filter *.sal).Count) modules"
# Runtime objects for sf's Rust pipeline (packages and fallback builds)
$rt = Join-Path $root 'lib\salivo\rt'
if (Test-Path $rt) { Remove-Item -Recurse -Force $rt }
Copy-Item -Recurse -Force -Path (Join-Path $src 'lib\salivo\rt') -Destination $rt

# 4. Stage 3 runtime ---------------------------------------------------------------------------
Step 4 'Installing the Stage 3 compiler runtime'
$stage3 = Join-Path $root 'stage3'
if (Test-Path $stage3) { Remove-Item -Recurse -Force $stage3 }
Copy-Item -Recurse -Force -Path (Join-Path $src 'stage3') -Destination $stage3
Ok "runtime sources and prebuilt objects in $stage3"

# 5. PATH --------------------------------------------------------------------------------------
Step 5 'Adding Salivo to your PATH'
if ($NoSystemChanges) {
    Ok 'skipped (-NoSystemChanges)'
} else {
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $parts = @()
    if ($userPath) { $parts = $userPath.Split(';') | Where-Object { $_ -ne '' } }
    if ($parts -notcontains $bin) {
        [Environment]::SetEnvironmentVariable('Path', (($parts + $bin) -join ';'), 'User')
        Ok "added $bin (new terminals will see it)"
    } else {
        Ok 'already on PATH'
    }
    if (($env:Path.Split(';')) -notcontains $bin) { $env:Path = "$env:Path;$bin" }
}

# 6. VS Code extension -------------------------------------------------------------------------
Step 6 'Installing the Salivo VS Code extension (syntax highlighting, snippets, run and build commands)'
$vsix = Get-ChildItem (Join-Path $src 'vscode') -Filter 'salivo-*.vsix' | Select-Object -First 1
if ($NoSystemChanges) {
    Ok 'skipped (-NoSystemChanges)'
} else {
    $codeCli = $null
    $cmd = Get-Command code -ErrorAction SilentlyContinue
    if ($cmd) { $codeCli = $cmd.Source }
    foreach ($candidate in @((Join-Path $env:LOCALAPPDATA 'Programs\Microsoft VS Code\bin\code.cmd'), (Join-Path $env:ProgramFiles 'Microsoft VS Code\bin\code.cmd'))) {
        if (-not $codeCli -and (Test-Path $candidate)) { $codeCli = $candidate }
    }
    if ($codeCli -and $vsix) {
        & $codeCli --install-extension $vsix.FullName --force | Out-Null
        if ($LASTEXITCODE -eq 0) { Ok "installed $($vsix.Name)" } else { Warn "VS Code could not install $($vsix.Name); install it from the Extensions view (... > Install from VSIX)." }
    } else {
        Warn "VS Code was not found. After installing it, open Extensions > ... > Install from VSIX and pick vscode\$($vsix.Name)."
    }
}

# 7. File icon and association -----------------------------------------------------------------
Step 7 'Giving .sal files the Salivo icon'
if ($NoSystemChanges) {
    Ok 'skipped (-NoSystemChanges)'
} else {
    Register-SalivoFiles
}

# 8. Self-test ---------------------------------------------------------------------------------
Step 8 'Testing the installation: compiling and running a program'
$sf = Join-Path $bin 'sf.exe'
$version = & $sf version
Ok "$version"
$testDir = Join-Path ([IO.Path]::GetTempPath()) ('salivo-setup-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $testDir | Out-Null
Set-Content -Path (Join-Path $testDir 'hello.sal') -Encoding ASCII -Value @'
module hello;

><salivo.std.core::{outln};

func main() -> int {
    let name = "Salivo";
    outln($"Hello from {name}!");
    return 0;
}
'@
Push-Location $testDir
try {
    $out = & $sf run --verbose hello.sal 2>&1 | Out-String
} finally {
    Pop-Location
}
Remove-Item -Recurse -Force $testDir -ErrorAction SilentlyContinue
$ok = $out -match 'Hello from Salivo!'
if ($ok -and $out -match 'with Stage 3') {
    Ok 'compiled with the Stage 3 compiler and ran: Hello from Salivo!'
} elseif ($ok) {
    Warn 'the test program ran, but not through the Stage 3 compiler:'
    Write-Host $out
} else {
    Warn 'the test program did not build or run. Output:'
    Write-Host $out
    Warn 'This almost always means clang or the Visual Studio C++ build tools are missing (see step 1).'
}

Write-Host ''
Write-Host '  ============================================' -ForegroundColor Magenta
if ($ok) {
    Write-Host '   Salivo is installed.' -ForegroundColor Green
} else {
    Write-Host '   Salivo is installed, but building programs needs the prerequisites above.' -ForegroundColor Yellow
}
Write-Host '   Open a NEW terminal (or restart VS Code) and try:'
Write-Host '       sf run hello.sal'
Write-Host '   In VS Code: open a .sal file and press Ctrl+F5 to run it.'
Write-Host '  ============================================' -ForegroundColor Magenta
if ($ok) { exit 0 } else { exit 2 }
