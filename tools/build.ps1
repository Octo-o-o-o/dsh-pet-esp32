# Build + flash the dsh-pet Dial firmware.
#
#   pwsh -File tools\build.ps1                 # compile and flash app
#   pwsh -File tools\build.ps1 -Assets         # also reflash the animation blob
#   pwsh -File tools\build.ps1 -CompileOnly    # just compile
#
# Notes
# - The Arduino toolchain lives at C:\dshdial (short path on purpose: GCC cannot
#   open its own C++ headers when the install path pushes past MAX_PATH).
# - Always flash with --flash_mode dio. The S3 bootloader is built for DIO; forcing
#   qio at flash time rewrites the image header and the ROM then fails to load it.

param(
    [string]$Port = "COM5",
    [switch]$Assets,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sketch = Join-Path $root "firmware\dsh_pet_dial"
$build = Join-Path $root "firmware\build"
$cli = Join-Path $PSScriptRoot "arduino-cli\arduino-cli.exe"

$ToolRoot = if ($env:DSH_DIAL_TOOLCHAIN) { $env:DSH_DIAL_TOOLCHAIN } else { "C:\dshdial" }
$env:ARDUINO_DIRECTORIES_DATA = "$ToolRoot\arduino-data"
$env:ARDUINO_DIRECTORIES_DOWNLOADS = "$ToolRoot\arduino-dl"
$env:ARDUINO_DIRECTORIES_USER = "$ToolRoot\arduino-user"
$env:TMP = "$ToolRoot\tmp"
$env:TEMP = "$ToolRoot\tmp"

$fqbn = "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=custom,PSRAM=disabled,CPUFreq=240,FlashMode=qio,UploadSpeed=921600"

Write-Host "== compile ==" -ForegroundColor Cyan
& $cli compile --fqbn $fqbn --output-dir $build $sketch
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
if ($CompileOnly) { return }

Write-Host "== flash app to $Port ==" -ForegroundColor Cyan
& python -m esptool --port $Port --baud 921600 --chip esp32s3 write_flash `
    --flash_mode dio --flash_freq 80m --flash_size 8MB `
    0x10000 "$build\dsh_pet_dial.ino.bin"
if ($LASTEXITCODE -ne 0) { throw "app flash failed" }

if ($Assets) {
    $blob = Join-Path $root "firmware\data\pet.bin"
    if (-not (Test-Path $blob)) { throw "missing $blob (run tools\build_assets.py first)" }
    Write-Host "== flash animation blob ==" -ForegroundColor Cyan
    & python -m esptool --port $Port --baud 921600 --chip esp32s3 write_flash `
        --flash_mode dio --flash_freq 80m --flash_size 8MB `
        0x210000 $blob
    if ($LASTEXITCODE -ne 0) { throw "asset flash failed" }
}

Write-Host "done" -ForegroundColor Green
