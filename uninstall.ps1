# Salivo SDK uninstaller: removes everything install.ps1 added for the current user (the toolchain in
# %USERPROFILE%\.salivo, its PATH entry, the VS Code extension and the .sal file association).
# LLVM and the Visual Studio build tools are left installed.

$ErrorActionPreference = 'Continue'
$root = Join-Path $env:USERPROFILE '.salivo'
$bin = Join-Path $root 'bin'

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if ($userPath) {
    $kept = $userPath.Split(';') | Where-Object { $_ -ne '' -and $_ -ne $bin }
    [Environment]::SetEnvironmentVariable('Path', ($kept -join ';'), 'User')
}

$code = Get-Command code -ErrorAction SilentlyContinue
if ($code) { & $code.Source --uninstall-extension salivo.salivo | Out-Null }

Remove-Item -Recurse -Force 'Registry::HKEY_CURRENT_USER\Software\Classes\.sal' -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force 'Registry::HKEY_CURRENT_USER\Software\Classes\Salivo.File' -ErrorAction SilentlyContinue

if (Test-Path $root) { Remove-Item -Recurse -Force $root }
Write-Host 'Salivo has been removed.' -ForegroundColor Green
