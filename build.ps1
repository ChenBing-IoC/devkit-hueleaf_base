# Build the ESP-IDF project (ESP-IDF v5.5.5, Windows install).
# Usage:  .\build.ps1                       # builds examples\3.7einkDvb
#         .\build.ps1 -Project examples\xxx # builds another example
[CmdletBinding()]
param(
    [string]$Project = 'examples\3.7einkDvb'
)

$ErrorActionPreference = 'Stop'

$activation = Get-ChildItem 'C:\Espressif\tools\Microsoft.v*.PowerShell_profile.ps1' -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $activation) { throw 'ESP-IDF activation script not found under C:\Espressif\tools' }

. $activation.FullName | Out-Null

$proj = Join-Path $PSScriptRoot $Project
if (-not (Test-Path (Join-Path $proj 'CMakeLists.txt'))) { throw "not an ESP-IDF project: $proj" }

idf.py -C $proj build
exit $LASTEXITCODE