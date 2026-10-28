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
$wsl = (Get-Command wsl.exe -ErrorAction Stop).Source
if ([string]::IsNullOrWhiteSpace($Firmware)) {
    $Firmware = Join-Path ${env:ProgramFiles} 'qemu\share\edk2-x86_64-code.fd'
}
if (-not (Test-Path $Firmware)) { throw "UEFI firmware not found: $Firmware (pass -Firmware to override)." }

$log = Join-Path $root 'build\qemu-debug.log'
$serialLog = Join-Path $root 'build\serial.log'
$bridgeLog = Join-Path $root 'build\curl-bridge.log'
$bridgePort = 43210
Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $serialLog -ErrorAction SilentlyContinue
$wslRoot = (& $wsl -- wslpath -a $root).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($wslRoot)) {
    throw 'Could not map the project path into WSL. Use make run from WSL for the curl bridge.'
}
$buildCurlCommand = "if [ ! -x '$wslRoot/build/bin/curl' ]; then bash '$wslRoot/scripts/build_curl_host.sh'; fi"
& $wsl -- bash -lc $buildCurlCommand
if ($LASTEXITCODE -ne 0) { throw 'Building upstream curl in WSL failed.' }
$bridgeCommand = "mkdir -p '$wslRoot/build' && exec python3 '$wslRoot/scripts/curl_bridge.py' --port $bridgePort --curl '$wslRoot/build/bin/curl' > '$wslRoot/build/curl-bridge.log' 2>&1"
$bridgeProcess = Start-Process -FilePath $wsl `
    -ArgumentList @('--', 'bash', '-lc', "`"$bridgeCommand`"") `
    -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 700
if ($bridgeProcess.HasExited) { throw "The WSL curl bridge did not start. See $bridgeLog or run make run from WSL." }
$args = @(
    '-machine', 'pc', '-m', '512M', '-smp', '2',
    '-drive', "if=pflash,format=raw,readonly=on,file=$Firmware",
    '-drive', "if=ide,index=0,format=raw,file=$imagePath",
    '-netdev', 'user,id=net0', '-device', 'virtio-net-pci,netdev=net0,disable-modern=on',
    '-chardev', "socket,id=curlbridge,host=127.0.0.1,port=$bridgePort,server=off",
    '-serial', "file:$serialLog",
    '-serial', 'chardev:curlbridge',
    '-global', 'isa-debugcon.iobase=0xe9', '-debugcon', "file:$log"
)
if ($Headless) { $args += @('-display', 'none') }
try {
    & $qemu @args
} finally {
    if ($null -ne $bridgeProcess -and -not $bridgeProcess.HasExited) {
        Stop-Process -Id $bridgeProcess.Id -Force -ErrorAction SilentlyContinue
    }
    & $wsl -- bash -lc "pkill -f 'curl_bridge.py --port $bridgePort' 2>/dev/null || true" | Out-Null
}
