if (Test-Path build/qemu_serial.log) {
    Remove-Item -Force build/qemu_serial.log
}
$proc = Start-Process -FilePath "C:\Program Files\qemu\qemu-system-x86_64.exe" `
  -ArgumentList "-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file=`"C:\Program Files\qemu\share\edk2-x86_64-code.fd`" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -display none" -PassThru
Start-Sleep -Seconds 12
Stop-Process -Id $proc.Id -Force
Get-Content build/qemu_serial.log -Tail 150
