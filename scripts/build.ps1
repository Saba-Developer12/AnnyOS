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
    @{ Path = 'src\drivers\input\usb_hid.cpp'; Name = 'usb_hid.obj' },
    @{ Path = 'src\drivers\storage\fat32.cpp'; Name = 'fat32.obj' },
    @{ Path = 'src\drivers\storage\ntfs.cpp'; Name = 'ntfs.obj' },
    @{ Path = 'src\drivers\storage\apfs.cpp'; Name = 'apfs.obj' },
    @{ Path = 'src\drivers\storage\ata_pio.cpp'; Name = 'ata_pio.obj' },
    @{ Path = 'src\drivers\system\timer_pit.cpp'; Name = 'timer_pit.obj' },
    @{ Path = 'src\drivers\system\serial_com1.cpp'; Name = 'serial_com1.obj' },
    @{ Path = 'src\drivers\system\pc_speaker.cpp'; Name = 'pc_speaker.obj' },
    @{ Path = 'src\kernel\ap_loader.cpp'; Name = 'ap_loader.obj' },
    @{ Path = 'src\kernel\config_save.cpp'; Name = 'config_save.obj' },
    @{ Path = 'src\kernel\driver_registry.cpp'; Name = 'driver_registry.obj' },
    @{ Path = 'src\kernel\shell.cpp'; Name = 'shell.obj' },
    @{ Path = 'src\kernel\shell_files.cpp'; Name = 'shell_files.obj' },
    @{ Path = 'src\kernel\shell_ui.cpp'; Name = 'shell_ui.obj' },
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
    @{ Path = 'src\drivers\storage\ntfs_module.cpp'; Name = 'ntfs_module.obj' },
    @{ Path = 'src\drivers\storage\apfs_module.cpp'; Name = 'apfs_module.obj' },
    @{ Path = 'src\drivers\input\ps2kbd_module.cpp'; Name = 'ps2kbd_module.obj' },
    @{ Path = 'src\drivers\input\usbhid_module.cpp'; Name = 'usbhid_module.obj' },
    @{ Path = 'src\drivers\system\timer_module.cpp'; Name = 'timer_module.obj' },
    @{ Path = 'src\drivers\system\serial_module.cpp'; Name = 'serial_module.obj' },
    @{ Path = 'src\drivers\system\speaker_module.cpp'; Name = 'speaker_module.obj' }
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
    '/export:annyos_fat32_write_file', '/export:annyos_fat32_delete_file',
    '/export:annyos_fat32_make_directory', '/export:annyos_fat32_remove_directory',
    '/export:annyos_fat32_copy_file', '/export:annyos_fat32_move_file',
    "/out:$(Join-Path $buildDir 'fat32.sys')",
    (Join-Path $objectDir 'fat32.obj'), (Join-Path $objectDir 'fat32_module.obj')
)
& $linker @fat32ModuleArgs
if ($LASTEXITCODE -ne 0) { throw 'FAT32 driver module link failed.' }

$ntfsModuleArgs = @(
    '/dll', '/noentry', '/nodefaultlib', '/machine:x64', '/dynamicbase', '/timestamp:0',
    '/export:annyos_ntfs_probe', "/out:$(Join-Path $buildDir 'ntfs.sys')",
    (Join-Path $objectDir 'ntfs.obj'), (Join-Path $objectDir 'ntfs_module.obj')
)
& $linker @ntfsModuleArgs
if ($LASTEXITCODE -ne 0) { throw 'NTFS probe module link failed.' }

$apfsModuleArgs = @(
    '/dll', '/noentry', '/nodefaultlib', '/machine:x64', '/dynamicbase', '/timestamp:0',
    '/export:annyos_apfs_probe', "/out:$(Join-Path $buildDir 'apfs.sys')",
    (Join-Path $objectDir 'apfs.obj'), (Join-Path $objectDir 'apfs_module.obj')
)
& $linker @apfsModuleArgs
if ($LASTEXITCODE -ne 0) { throw 'APFS probe module link failed.' }

$ataModuleArgs = @(
    '/dll', '/noentry', '/nodefaultlib', '/machine:x64', '/dynamicbase', '/timestamp:0',
    '/export:annyos_ata_pio_initialize', '/export:annyos_ata_pio_read_sector',
    '/export:annyos_ata_pio_write_sector',
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

$usbHidModuleArgs = @(
    '/dll', '/noentry', '/nodefaultlib', '/machine:x64', '/dynamicbase', '/timestamp:0',
    '/export:annyos_usbhid_probe', '/export:annyos_usbhid_decode_report',
    "/out:$(Join-Path $buildDir 'usbhid.sys')",
    (Join-Path $objectDir 'usb_hid.obj'), (Join-Path $objectDir 'usbhid_module.obj')
)
& $linker @usbHidModuleArgs
if ($LASTEXITCODE -ne 0) { throw 'USB HID driver module link failed.' }

$timerModuleArgs = @(
    '/dll', '/noentry', '/nodefaultlib', '/machine:x64', '/dynamicbase', '/timestamp:0',
    '/export:annyos_timer_initialize', '/export:annyos_timer_ticks', '/export:annyos_timer_sleep_ms',
    "/out:$(Join-Path $buildDir 'timer.sys')",
    (Join-Path $objectDir 'timer_pit.obj'), (Join-Path $objectDir 'timer_module.obj')
)
& $linker @timerModuleArgs
if ($LASTEXITCODE -ne 0) { throw 'PIT timer module link failed.' }

$serialModuleArgs = @(
    '/dll', '/noentry', '/nodefaultlib', '/machine:x64', '/dynamicbase', '/timestamp:0',
    '/export:annyos_serial_initialize', '/export:annyos_serial_write_byte', '/export:annyos_serial_write',
    '/export:annyos_serial_read_byte',
    "/out:$(Join-Path $buildDir 'serial.sys')",
    (Join-Path $objectDir 'serial_com1.obj'), (Join-Path $objectDir 'serial_module.obj')
)
& $linker @serialModuleArgs
if ($LASTEXITCODE -ne 0) { throw 'COM1 serial module link failed.' }

$speakerModuleArgs = @(
    '/dll', '/noentry', '/nodefaultlib', '/machine:x64', '/dynamicbase', '/timestamp:0',
    '/export:annyos_speaker_initialize', '/export:annyos_speaker_tone', '/export:annyos_speaker_stop',
    "/out:$(Join-Path $buildDir 'speaker.sys')",
    (Join-Path $objectDir 'pc_speaker.obj'), (Join-Path $objectDir 'timer_pit.obj'),
    (Join-Path $objectDir 'speaker_module.obj')
)
& $linker @speakerModuleArgs
if ($LASTEXITCODE -ne 0) { throw 'PC speaker module link failed.' }

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
Copy-Item -LiteralPath (Join-Path $buildDir 'ntfs.sys') -Destination $driverDir -Force
Copy-Item -LiteralPath (Join-Path $buildDir 'apfs.sys') -Destination $driverDir -Force
Copy-Item -LiteralPath (Join-Path $buildDir 'ata.sys') -Destination $driverDir -Force
Copy-Item -LiteralPath (Join-Path $buildDir 'ps2kbd.sys') -Destination $driverDir -Force
Copy-Item -LiteralPath (Join-Path $buildDir 'usbhid.sys') -Destination $driverDir -Force
Copy-Item -LiteralPath (Join-Path $buildDir 'timer.sys') -Destination $driverDir -Force
Copy-Item -LiteralPath (Join-Path $buildDir 'serial.sys') -Destination $driverDir -Force
Copy-Item -LiteralPath (Join-Path $buildDir 'speaker.sys') -Destination $driverDir -Force

$appsDir = Join-Path $espDir 'apps'
New-Item -ItemType Directory -Force -Path $appsDir | Out-Null
$driveLetter = $root.Substring(0, 1).ToLowerInvariant()
$rootWsl = "/mnt/$driveLetter/" + $root.Substring(3).Replace('\', '/')
if (-not (Test-Path $root)) {
    throw 'WSL with Python 3 is required to package AP files and create the FAT32 image.'
}
foreach ($appSource in Get-ChildItem -LiteralPath (Join-Path $root 'apps') -Filter '*.aadl' -File) {
    $appOutput = Join-Path $appsDir ($appSource.BaseName + '.AP')
    $appSourceWsl = "$rootWsl/apps/$($appSource.Name)"
    $appOutputWsl = "$rootWsl/build/esp/apps/$($appSource.BaseName).AP"
    & wsl.exe python3 "$rootWsl/tools/aadlc.py" $appSourceWsl $appOutputWsl
    if ($LASTEXITCODE -ne 0) { throw "AADL compilation failed: $($appSource.Name)" }
}
$espDirWsl = "$rootWsl/build/esp"
$imagePathWsl = "$rootWsl/build/AnnyOS.img"
& wsl.exe python3 "$rootWsl/tools/build_disk_image.py" $espDirWsl $imagePathWsl
if ($LASTEXITCODE -ne 0) { throw 'FAT32 disk image creation failed.' }

Write-Host "Built $efiPath"
Write-Host "Built $kernelImage, FAT32 disk image, and driver modules in $driverDir"
