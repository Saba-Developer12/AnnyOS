$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root 'build'
$objectDir = Join-Path $buildDir 'obj'
$espDir = Join-Path $buildDir 'esp'
$efiDir = Join-Path $espDir 'EFI\BOOT'
$driverDir = Join-Path $espDir 'driver'
$fontDir = Join-Path $espDir 'fonts\segoe-ui'
$clang = (Get-Command clang++ -ErrorAction Stop).Source
$linker = (Get-Command lld-link -ErrorAction Stop).Source

New-Item -ItemType Directory -Force -Path $objectDir, $efiDir, $driverDir, $fontDir | Out-Null

$commonFlags = @(
    '--target=x86_64-pc-windows-msvc', '-std=c++20', '-O2', '-Wall', '-Wextra',
    '-ffreestanding', '-fno-builtin', '-fno-exceptions', '-fno-rtti', '-fno-stack-protector',
    '-fno-threadsafe-statics', '-fno-use-cxa-atexit', '-mno-red-zone',
    '-fshort-wchar', '-nostdlib', "-I$(Join-Path $root 'include')",
    "-I$(Join-Path $root 'src\boot')"
)
$sources = @(
    @{ Path = 'src\boot\efi_main.cpp'; Name = 'efi_main.obj' },
    @{ Path = 'src\boot\gop_helper.cpp'; Name = 'gop_helper.obj' },
    @{ Path = 'src\drivers\input\ps2_keyboard.cpp'; Name = 'ps2_keyboard.obj' },
    @{ Path = 'src\drivers\storage\fat32.cpp'; Name = 'fat32.obj' },
    @{ Path = 'src\drivers\storage\ata_pio.cpp'; Name = 'ata_pio.obj' },
    @{ Path = 'src\kernel\ap_loader.cpp'; Name = 'ap_loader.obj' },
    @{ Path = 'src\kernel\runtime.cpp'; Name = 'runtime.obj' },
    @{ Path = 'src\kernel\main.cpp'; Name = 'kernel_main.obj' }
)
$objects = @()
foreach ($source in $sources) {
    $sourcePath = Join-Path $root $source.Path
    $objectPath = Join-Path $objectDir $source.Name
    & $clang @commonFlags -c $sourcePath -o $objectPath
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $($source.Path)" }
    $objects += $objectPath
}

$moduleSources = @(
    @{ Path = 'src\drivers\storage\fat32_module.cpp'; Name = 'fat32_module.obj' },
    @{ Path = 'src\drivers\input\ps2kbd_module.cpp'; Name = 'ps2kbd_module.obj' }
)
foreach ($source in $moduleSources) {
    $sourcePath = Join-Path $root $source.Path
    $objectPath = Join-Path $objectDir $source.Name
    & $clang @commonFlags -c $sourcePath -o $objectPath
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $($source.Path)" }
}

$efiPath = Join-Path $efiDir 'BOOTX64.EFI'
$linkArgs = @(
    '/subsystem:efi_application', '/entry:efi_main', '/nodefaultlib', '/machine:x64',
    '/dynamicbase', '/nxcompat', '/opt:ref', '/timestamp:0', "/out:$efiPath"
) + $objects
& $linker @linkArgs
if ($LASTEXITCODE -ne 0) { throw 'UEFI link failed.' }

$fat32ModuleArgs = @(
    '/dll', '/noentry', '/nodefaultlib', '/machine:x64', '/dynamicbase', '/timestamp:0',
    '/export:annyos_fat32_mount', '/export:annyos_fat32_list_root',
    '/export:annyos_fat32_list_directory',
    '/export:annyos_fat32_find_file', '/export:annyos_fat32_read_file',
    "/out:$(Join-Path $buildDir 'fat32.sys')",
    (Join-Path $objectDir 'fat32.obj'), (Join-Path $objectDir 'fat32_module.obj')
)
& $linker @fat32ModuleArgs
if ($LASTEXITCODE -ne 0) { throw 'FAT32 driver module link failed.' }

$ataModuleArgs = @(
    '/dll', '/noentry', '/nodefaultlib', '/machine:x64', '/dynamicbase', '/timestamp:0',
    '/export:annyos_ata_pio_initialize', '/export:annyos_ata_pio_read_sector',
    "/out:$(Join-Path $buildDir 'ata.sys')", (Join-Path $objectDir 'ata_pio.obj')
)
& $linker @ataModuleArgs
if ($LASTEXITCODE -ne 0) { throw 'ATA PIO driver module link failed.' }

$keyboardModuleArgs = @(
    '/dll', '/noentry', '/nodefaultlib', '/machine:x64', '/dynamicbase', '/timestamp:0',
    '/export:annyos_ps2kbd_initialize', '/export:annyos_ps2kbd_poll',
    "/out:$(Join-Path $buildDir 'ps2kbd.sys')",
    (Join-Path $objectDir 'ps2_keyboard.obj'), (Join-Path $objectDir 'ps2kbd_module.obj')
)
& $linker @keyboardModuleArgs
if ($LASTEXITCODE -ne 0) { throw 'PS/2 keyboard driver module link failed.' }

$kernelImage = Join-Path $buildDir 'annyos.bin'
Copy-Item -LiteralPath $efiPath -Destination $kernelImage -Force
Copy-Item -LiteralPath $efiPath -Destination (Join-Path $efiDir 'BOOTX86.EFI') -Force
Copy-Item -LiteralPath $kernelImage -Destination (Join-Path $espDir 'ANNYOS.BIN') -Force
Copy-Item -LiteralPath $efiPath -Destination (Join-Path $espDir 'bootx86.efi') -Force
$fontFiles = @('Segoe UI.ttf', 'Segoe UI Bold.ttf', 'Segoe UI Italic.ttf', 'Segoe UI Bold Italic.ttf')
foreach ($fontFile in $fontFiles) {
    Copy-Item -LiteralPath (Join-Path $root "font\segoe-ui\$fontFile") -Destination $fontDir -Force
}
Copy-Item -LiteralPath (Join-Path $root 'font\segoe-ui\README.TXT') -Destination $fontDir -Force
Copy-Item -LiteralPath (Join-Path $buildDir 'fat32.sys') -Destination $driverDir -Force
Copy-Item -LiteralPath (Join-Path $buildDir 'ata.sys') -Destination $driverDir -Force
Copy-Item -LiteralPath (Join-Path $buildDir 'ps2kbd.sys') -Destination $driverDir -Force

Write-Host "Built $efiPath"
Write-Host "Built $kernelImage and driver modules in $driverDir"
