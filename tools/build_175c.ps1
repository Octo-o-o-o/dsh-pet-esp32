# Build + flash the Waveshare ESP32-S3-Touch-AMOLED-1.75C target.
#
#   pwsh -File tools\build_175c.ps1              # compile and flash app
#   pwsh -File tools\build_175c.ps1 -Assets      # also flash the 466x466 blob
#
# First flash (or after changing partitions.csv) must include the bootloader and
# partition table; -Full does that.

param(
    [string]$Port = "COM6",
    [switch]$Assets,
    [switch]$Full
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sketch = Join-Path $root "firmware_175c\dsh_pet_175c"
$build = Join-Path $root "firmware_175c\build"
$cli = if ($env:DSH_DIAL_ARDUINO_CLI) { $env:DSH_DIAL_ARDUINO_CLI } elseif (Test-Path (Join-Path $PSScriptRoot "arduino-cli\arduino-cli.exe")) { Join-Path $PSScriptRoot "arduino-cli\arduino-cli.exe" } else { "arduino-cli" }

$ToolRoot = if ($env:DSH_DIAL_TOOLCHAIN) { $env:DSH_DIAL_TOOLCHAIN } else { "C:\dshdial" }
$env:ARDUINO_DIRECTORIES_DATA = "$ToolRoot\arduino-data"
$env:ARDUINO_DIRECTORIES_DOWNLOADS = "$ToolRoot\arduino-dl"
$env:ARDUINO_DIRECTORIES_USER = "$ToolRoot\arduino-user"
$env:TMP = "$ToolRoot\tmp"
$env:TEMP = "$ToolRoot\tmp"

$fqbn = "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=32M,PartitionScheme=custom,PSRAM=opi,CPUFreq=240,FlashMode=qio,UploadSpeed=921600"

# esp_lcd_co5300 must be installed as an Arduino library (see libs/)
$libDir = Join-Path $env:ARDUINO_DIRECTORIES_USER "libraries\esp_lcd_co5300"
if (-not (Test-Path $libDir)) { throw "missing esp_lcd_co5300 library: copy libs\esp_lcd_co5300 into $libDir" }

Write-Host "== compile ==" -ForegroundColor Cyan
& $cli compile --fqbn $fqbn --output-dir $build $sketch
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

$argsList = @("--port", $Port, "--baud", "921600", "--chip", "esp32s3", "write_flash",
    "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "32MB")
if ($Full) {
    $argsList += @("0x0", "$build\dsh_pet_175c.ino.bootloader.bin",
                   "0x8000", "$build\dsh_pet_175c.ino.partitions.bin")
}
$argsList += @("0x10000", "$build\dsh_pet_175c.ino.bin")
if ($Assets) {
    $blob = Join-Path $root "firmware_175c\data\pet.bin"
    if (-not (Test-Path $blob)) { throw "missing $blob (run tools\build_assets.py with PET_SIZE=466)" }
    $argsList += @("0x410000", $blob)
}

Write-Host "== flash ==" -ForegroundColor Cyan
& python -m esptool @argsList
if ($LASTEXITCODE -ne 0) { throw "flash failed" }
Write-Host "done" -ForegroundColor Green


