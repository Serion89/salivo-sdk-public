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

# Undo only Salivo's own association; other programs' .sal handlers are left intact
$classes = 'Registry::HKEY_CURRENT_USER\Software\Classes'
if ((Get-ItemProperty -Path "$classes\.sal" -ErrorAction SilentlyContinue).'(default)' -eq 'Salivo.File') {
    Set-ItemProperty -Path "$classes\.sal" -Name '(default)' -Value ''
}
Remove-ItemProperty -Path "$classes\.sal\OpenWithProgids" -Name 'Salivo.File' -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force "$classes\Salivo.File" -ErrorAction SilentlyContinue

if (Test-Path $root) { Remove-Item -Recurse -Force $root }
Write-Host 'Salivo has been removed.' -ForegroundColor Green
