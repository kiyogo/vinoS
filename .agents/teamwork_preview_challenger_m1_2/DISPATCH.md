# Dispatch: Challenger M1-2

## Mission
Adversarially verify Milestone M1 (Hardware & Paging Primitives):
1. Verify `kernel/task.c` and `arch/x86_64/descriptors.c`:
   - Inspect TSS RSP0 updates during task switches.
   - Verify alignment: `& ~0xFull` ensures 16-byte stack alignment.
   - Check if `kos_boot_stack_top` fallback works for early bootstrap tasks before a separate stack is allocated.
2. Verify GDT definitions:
   - Check `GDT_USER_CS = 0x2b` and `GDT_USER_DS = 0x33`.
   - Ensure these match descriptor indices 5 and 6 with RPL 3.
3. Verify build and run QEMU execution to ensure no regressions in scheduler or exception handling.
Report verdict in `handoff.md`.

## 2026-09-18T00:34:25Z
<USER_REQUEST>
You are teamwork_preview_challenger_m1_2.
Your working directory is: d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_challenger_m1_2
Read d:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md before starting work.
Read d:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k14_1\PROJECT.md for architecture and contracts.
Read d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_worker_m1\changes.md and handoff.md.
Read d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_challenger_m1_2\DISPATCH.md for your mission.

Adversarially verify Milestone M1:
1. Verify task switching, TSS RSP0 updates, and GDT selector constants.
2. Run tools\kos-tool.exe build and check build warnings/errors.
3. Verify verdict: APPROVE or REQUEST_CHANGES.
Write report to handoff.md in your working directory and send a message when done.
</USER_REQUEST>
