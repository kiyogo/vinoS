if (Test-Path build/qemu_serial.log) {
    Remove-Item -Force build/qemu_serial.log
}
 = @(
     -machine, q35,
    -cpu, max,
    -m, 256,
    -drive, if=pflash -machine", "q35",
 "-cpu", "max",
 "-m", "256",
 "-drive", "if=pflash,format=raw,readonly=on,file=C:/Program Files/qemu/share/edk2-x86_64-code.fd,
    -drive, if=pflash Files/qemu/share/edk2-x86_64-code.fd",
 "-drive", "if=pflash,format=raw,file=build/edk2-vars.fd",
 "-drive", "format=raw,file=fat:rw:build/esp",
 "-serial", "file:build/qemu_serial.log",
 "-monitor", "none",
 "-no-reboot",
 "-netdev", "user,id=kosnet,ipv4=on,ipv6=off,hostfwd=tcp::8080-:80",
 "-device", "e1000,netdev=kosnet,mac=52:54:00:12:34:56",
 "-display", "none"
)
$proc = Start-Process -FilePath "C:/Program Files/qemu/qemu-system-x86_64.exe -ArgumentList  -PassThru
Start-Sleep -Seconds 25
Stop-Process -Id .Id -Force
Get-Content build/qemu_serial.log | Select-Object -Last 100
