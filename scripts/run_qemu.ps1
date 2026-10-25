param(
    [switch]$Headless,
    [string]$Firmware
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$imagePath = Join-Path $root 'build\AnnyOS.img'
if (-not (Test-Path $imagePath)) {
    throw "Disk image not found: $imagePath. Run scripts/build.ps1 to build the FAT32 boot disk first."
}

$qemu = (Get-Command qemu-system-x86_64 -ErrorAction Stop).Source
if ([string]::IsNullOrWhiteSpace($Firmware)) {
    $Firmware = Join-Path ${env:ProgramFiles} 'qemu\share\edk2-x86_64-code.fd'
}
if (-not (Test-Path $Firmware)) { throw "UEFI firmware not found: $Firmware (pass -Firmware to override)." }

$log = Join-Path $root 'build\qemu-debug.log'
$serialLog = Join-Path $root 'build\serial.log'
Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $serialLog -ErrorAction SilentlyContinue
$args = @(
    '-machine', 'pc', '-m', '512M', '-smp', '2',
    '-drive', "if=pflash,format=raw,readonly=on,file=$Firmware",
    '-drive', "if=ide,index=0,format=raw,file=$imagePath",
    '-serial', "file:$serialLog",
    '-global', 'isa-debugcon.iobase=0xe9', '-debugcon', "file:$log"
)
if ($Headless) { $args += @('-display', 'none', '-serial', 'none') }
& $qemu @args
