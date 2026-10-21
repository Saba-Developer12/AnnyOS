param(
    [switch]$Headless,
    [string]$Firmware
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$imagePath = Join-Path $root 'build\AnnyOS.img'
if (-not (Test-Path $imagePath)) {
    throw "Disk image not found: $imagePath. Image creation scripts were removed; provide a bootable AnnyOS.img first."
}

$qemu = (Get-Command qemu-system-x86_64 -ErrorAction Stop).Source
if ([string]::IsNullOrWhiteSpace($Firmware)) {
    $Firmware = Join-Path ${env:ProgramFiles} 'qemu\share\edk2-x86_64-code.fd'
}
if (-not (Test-Path $Firmware)) { throw "UEFI firmware not found: $Firmware (pass -Firmware to override)." }

$log = Join-Path $root 'build\qemu-debug.log'
Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
$args = @(
    '-machine', 'pc', '-m', '512M', '-smp', '2',
    '-drive', "if=pflash,format=raw,readonly=on,file=$Firmware",
    '-drive', "if=ide,index=0,format=raw,file=$imagePath",
    '-global', 'isa-debugcon.iobase=0xe9', '-debugcon', "file:$log",
    '-no-reboot', '-no-shutdown'
)
if ($Headless) { $args += @('-display', 'none', '-serial', 'none') }
& $qemu @args
