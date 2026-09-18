# Dispatch: Challenger M2-1

## Mission
Adversarially challenge and verify Milestone M2:
1. Run `tools\kos-tool.exe build` and verify 0 warnings and 0 errors.
2. Run QEMU smoke test with UEFI firmware:
   Command:
   ```powershell
   $p = Start-Process -FilePath "C:\Program Files\qemu\qemu-system-x86_64.exe" -ArgumentList "-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file=`"C:\Program Files\qemu\share\edk2-x86_64-code.fd`" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -display none" -PassThru; Start-Sleep -Seconds 8; Stop-Process -Id $p.Id -Force; Get-Content build/qemu_serial.log -Tail 50
   ```
   Check for:
   - `VMM address space lifecycle self-test passed (0 frame leaks).`
   - `ELF loader & user stack self-test passed (0 frame leaks).`
   - Zero kernel panics or crashes.
3. Report verdict (APPROVE or REQUEST_CHANGES) in `handoff.md`.


## 2026-09-18T00:50:28Z
You are teamwork_preview_challenger_m2_1.
Your working directory is: d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_challenger_m2_1
Read d:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md before starting work.
Read d:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k14_1\PROJECT.md for architecture and contracts.
Read d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_worker_m2\changes.md and handoff.md.
Read d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_challenger_m2_1\DISPATCH.md for your mission.

Adversarially challenge and verify Milestone M2:
1. Build kernel with tools\kos-tool.exe build.
2. Run QEMU smoke test with UEFI firmware:
   $p = Start-Process -FilePath "C:\Program Files\qemu\qemu-system-x86_64.exe" -ArgumentList "-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file=`"C:\Program Files\qemu\share\edk2-x86_64-code.fd`" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -display none" -PassThru; Start-Sleep -Seconds 8; Stop-Process -Id $p.Id -Force; Get-Content build/qemu_serial.log -Tail 50
3. Verify that VMM address space lifecycle self-test and ELF loader self-test pass with 0 frame leaks.
4. Determine verdict: APPROVE or REQUEST_CHANGES.
Write report to handoff.md in your working directory and send a message when done.
