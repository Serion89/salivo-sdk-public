# Salivo installer for Windows. Downloads the latest release, checks its SHA-256, and runs it.
#   irm https://raw.githubusercontent.com/Serion89/salivo-sdk-public/main/get.ps1 | iex
$ErrorActionPreference = 'Stop'
$repo = 'Serion89/salivo-sdk-public'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$release = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" -Headers @{ 'User-Agent' = 'salivo-installer' }
$setup = $release.assets | Where-Object { $_.name -like 'salivo-*-windows-x64-setup.exe' } | Select-Object -First 1
$sums = $release.assets | Where-Object { $_.name -eq 'SHA256SUMS.txt' } | Select-Object -First 1
if (-not $setup -or -not $sums) { throw "Release $($release.tag_name) has no Windows installer or SHA256SUMS.txt." }

$dir = Join-Path $env:TEMP ("salivo-install-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $dir | Out-Null
try {
    $file = Join-Path $dir $setup.name
    Write-Host "Downloading $($setup.name) ($($release.tag_name))..."
    Invoke-WebRequest $setup.browser_download_url -OutFile $file -UseBasicParsing
    $line = (Invoke-RestMethod $sums.browser_download_url -UseBasicParsing -Headers @{ 'User-Agent' = 'salivo-installer' }) -split "`n" |
        Where-Object { $_ -match [regex]::Escape($setup.name) } | Select-Object -First 1
    if (-not $line) { throw "No checksum for $($setup.name) in SHA256SUMS.txt." }
    $expected = ($line -split '\s+')[0].ToLower()
    $actual = (Get-FileHash $file -Algorithm SHA256).Hash.ToLower()
    if ($actual -ne $expected) { throw "Checksum mismatch for $($setup.name). Expected $expected, got $actual." }
    Write-Host 'Checksum OK. Installing...'
    Start-Process -FilePath $file -ArgumentList '/SILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait
    Write-Host 'Salivo is installed. Open a new terminal and run: sf --version'
} finally {
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
}
