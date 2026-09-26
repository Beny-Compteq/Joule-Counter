<#
.SYNOPSIS
    Build (and optionally flash) the PPK2 baseline firmware.

.DESCRIPTION
    Wraps `west build` with the two things this project needs that are not
    discoverable automatically: the nRF Connect SDK toolchain environment, and
    BOARD_ROOT pointing at this directory so sysbuild can find boards/nordic/ppk2.

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Pristine
    .\build.ps1 -Flash
#>
[CmdletBinding()]
param(
    # nRF Connect SDK version to build against.
    [string]$NcsVersion = "v3.4.1",

    # Where nrfutil installed the SDK and toolchains.
    [string]$NcsRoot = "C:\ncs",

    # Discard the existing build directory and reconfigure from scratch.
    [switch]$Pristine,

    # Program the board over SWD (J-Link) after a successful build.
    [switch]$Flash
)

# Deliberately not "Stop": west and cmake write progress to stderr, and
# Windows PowerShell turns native stderr output into a terminating error when
# it is. Exit codes are checked explicitly instead.
$ErrorActionPreference = "Continue"
$app = $PSScriptRoot

# nrfutil's sdk-manager ships inside the nRF Connect VS Code extension.
$sdkManager = Get-ChildItem `
    -Path "$env:USERPROFILE\.vscode\extensions" `
    -Filter "nrfutil-sdk-manager.exe" -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName

if (-not $sdkManager) {
    throw "nrfutil-sdk-manager.exe not found. Install the nRF Connect for VS Code extension pack."
}

# Put the Zephyr SDK, cmake, ninja, west and python on PATH for this process.
& $sdkManager toolchain env --ncs-version $NcsVersion --as-script powershell |
    Out-String | Invoke-Expression

$env:ZEPHYR_BASE = Join-Path $NcsRoot "$NcsVersion\zephyr"
if (-not (Test-Path $env:ZEPHYR_BASE)) {
    throw "Zephyr not found at $env:ZEPHYR_BASE. Install the SDK with: $sdkManager install $NcsVersion"
}

$westArgs = @("build", "-b", "ppk2/nrf52840", "-d", (Join-Path $app "build"), $app)
if ($Pristine) { $westArgs += "--pristine=always" }
# Everything after `--` goes to CMake. sysbuild does not inherit BOARD_ROOT
# from the application's CMakeLists.txt, so it has to be passed here.
$westArgs += @("--", "-DBOARD_ROOT=$app")

west @westArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

if ($Flash) {
    west flash -d (Join-Path $app "build") --runner jlink
    if ($LASTEXITCODE -ne 0) { throw "Flash failed." }
}
