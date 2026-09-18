@echo off
echo ===================================================
echo        Installing Salivo Language Toolchain (SDK)
echo ===================================================

set USER_SALIVO=%USERPROFILE%\.salivo
if not exist "%USER_SALIVO%\bin" mkdir "%USER_SALIVO%\bin"
if not exist "%USER_SALIVO%\lib\salivo\std" mkdir "%USER_SALIVO%\lib\salivo\std"
if not exist "%USER_SALIVO%\std" mkdir "%USER_SALIVO%\std"

echo 1. Installing binaries (sf.exe, spm.exe)...
copy /Y "%~dp0bin\*" "%USER_SALIVO%\bin\" >nul

echo 2. Installing standard library...
xcopy /E /I /Y "%~dp0std\*" "%USER_SALIVO%\lib\salivo\std\" >nul
xcopy /E /I /Y "%~dp0std\*" "%USER_SALIVO%\std\" >nul

echo 3. Installing VS Code extension & configuring runner...
set VSCODE_EXT=%USERPROFILE%\.vscode\extensions\salivo.salivo-1.0.7
if not exist "%VSCODE_EXT%" mkdir "%VSCODE_EXT%"
xcopy /E /I /Y "%~dp0vscode-extension\*" "%VSCODE_EXT%\" >nul
powershell -NoProfile -Command "$p = Join-Path $env:APPDATA 'Code\User\settings.json'; if (Test-Path $p) { try { $j = Get-Content $p -Raw | ConvertFrom-Json } catch { $j = [PSCustomObject]@{} }; if (-not (Get-Member -InputObject $j -Name 'code-runner.executorMap')) { $j | Add-Member -Name 'code-runner.executorMap' -Value ([PSCustomObject]@{}) -MemberType NoteProperty -ErrorAction SilentlyContinue }; $j.'code-runner.executorMap'.'salivo' = 'sf run'; $j.'code-runner.executorMap'.'sal' = 'sf run'; $j.'code-runner.executorMap'.'sf' = 'sf run'; $j.'code-runner.fileDirectoryAsCwd' = $false; $j.'code-runner.runInTerminal' = $true; $j.'code-runner.saveFileBeforeRun' = $true; $j | ConvertTo-Json -Depth 10 | Set-Content $p -Encoding UTF8 }" >nul 2>&1

echo 4. Adding Salivo to Windows user PATH...
for /f "tokens=2*" %%a in ('reg query HKCU\Environment /v PATH 2^>nul') do set CURRENT_PATH=%%b
echo %CURRENT_PATH% | findstr /I /C:"%USER_SALIVO%\bin" >nul
if errorlevel 1 (
    setx PATH "%CURRENT_PATH%;%USER_SALIVO%\bin" >nul
)

echo 5. Teaching AI Assistants (Antigravity & Claude) about Salivo...
set GEMINI_CONFIG=%USERPROFILE%\.gemini\config
if not exist "%GEMINI_CONFIG%\rules" mkdir "%GEMINI_CONFIG%\rules"
copy /Y "%~dp0docs\salivo_quick_reference.md" "%GEMINI_CONFIG%\rules\salivo.md" >nul

set ANTIGRAVITY_REF=%USERPROFILE%\.gemini\antigravity-ide\knowledge\salivo_reference
if not exist "%ANTIGRAVITY_REF%\artifacts" mkdir "%ANTIGRAVITY_REF%\artifacts"
copy /Y "%~dp0docs\metadata.json" "%ANTIGRAVITY_REF%\metadata.json" >nul
copy /Y "%~dp0docs\salivo_quick_reference.md" "%ANTIGRAVITY_REF%\artifacts\salivo_quick_reference.md" >nul

rem Configure Claude Code & Claude Desktop
set CLAUDE_DIR=%USERPROFILE%\.claude
if not exist "%CLAUDE_DIR%" mkdir "%CLAUDE_DIR%"
copy /Y "%~dp0CLAUDE.md" "%CLAUDE_DIR%\CLAUDE.md" >nul
if not exist "%CLAUDE_DIR%\skills\salivo" mkdir "%CLAUDE_DIR%\skills\salivo"
if exist "%~dp0skills\salivo\SKILL.md" copy /Y "%~dp0skills\salivo\SKILL.md" "%CLAUDE_DIR%\skills\salivo\SKILL.md" >nul
if not exist "%CLAUDE_DIR%\commands" mkdir "%CLAUDE_DIR%\commands"
if exist "%~dp0commands\salivo.md" copy /Y "%~dp0commands\salivo.md" "%CLAUDE_DIR%\commands\salivo.md" >nul
if exist "%~dp0mcp\server.js" (
    if not exist "%USER_SALIVO%\mcp" mkdir "%USER_SALIVO%\mcp"
    copy /Y "%~dp0mcp\server.js" "%USER_SALIVO%\mcp\server.js" >nul
    if exist "%~dp0configure_claude.js" node "%~dp0configure_claude.js" >nul 2>&1
)

echo 6. Setting up Windows File Icon & VS Code Association for .sal files...
copy /Y "%~dp0assets\salivo.ico" "%USER_SALIVO%\salivo.ico" >nul
powershell -NoProfile -ExecutionPolicy Bypass -Command "$code = ''; $c = @((Join-Path $env:LOCALAPPDATA 'Programs\Microsoft VS Code\Code.exe'), (Join-Path $env:ProgramFiles 'Microsoft VS Code\Code.exe'), (Join-Path ${env:ProgramFiles(x86)} 'Microsoft VS Code\Code.exe')); foreach($x in $c){ if($x -and (Test-Path $x)){ $code = $x; break } }; if(-not $code){ $m = Get-Command code.cmd -ErrorAction SilentlyContinue; if($m){ $d = Split-Path (Split-Path $m.Source -Parent) -Parent; $cand = Join-Path $d 'Code.exe'; if(Test-Path $cand){ $code = $cand } } }; if(-not $code){ $code = 'notepad.exe' }; New-Item -Path 'Registry::HKEY_CURRENT_USER\Software\Classes\.sal' -Force | Out-Null; Set-ItemProperty -Path 'Registry::HKEY_CURRENT_USER\Software\Classes\.sal' -Name '(default)' -Value 'Salivo.File'; Set-ItemProperty -Path 'Registry::HKEY_CURRENT_USER\Software\Classes\.sal' -Name 'Content Type' -Value 'text/plain'; Set-ItemProperty -Path 'Registry::HKEY_CURRENT_USER\Software\Classes\.sal' -Name 'PerceivedType' -Value 'text'; New-Item -Path 'Registry::HKEY_CURRENT_USER\Software\Classes\Salivo.File' -Force | Out-Null; Set-ItemProperty -Path 'Registry::HKEY_CURRENT_USER\Software\Classes\Salivo.File' -Name '(default)' -Value 'Salivo Source File'; New-Item -Path 'Registry::HKEY_CURRENT_USER\Software\Classes\Salivo.File\DefaultIcon' -Force | Out-Null; Set-ItemProperty -Path 'Registry::HKEY_CURRENT_USER\Software\Classes\Salivo.File\DefaultIcon' -Name '(default)' -Value \"$env:USERPROFILE\.salivo\salivo.ico,0\"; New-Item -Path 'Registry::HKEY_CURRENT_USER\Software\Classes\Salivo.File\shell\open\command' -Force | Out-Null; Set-ItemProperty -Path 'Registry::HKEY_CURRENT_USER\Software\Classes\Salivo.File\shell\open\command' -Name '(default)' -Value \"`\"$code`\" `\"%1`\"\"; New-Item -Path 'Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.sal\OpenWithProgids' -Force | Out-Null; Set-ItemProperty -Path 'Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.sal\OpenWithProgids' -Name 'Salivo.File' -Value ([byte[]]@()); Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public class S { [DllImport(\"shell32.dll\")] public static extern void SHChangeNotify(int e, int f, IntPtr a, IntPtr b); }'; [S]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)" >nul 2>&1

echo ===================================================
echo  SUCCESS: Salivo is now fully installed!
echo  - Toolchain added to PATH
echo  - VS Code extension & Run button configured
echo  - Windows File Explorer: .sal files open in VS Code
echo  - Official Salivo logo assigned to .sal files
echo  - AI Assistants configured (Antigravity & Claude)
echo  - Claude Code Skill, Slash Command, & MCP Server active
echo
echo  Restart VS Code or your terminal to begin:
echo     sf run your_file.sal
echo ===================================================
pause
