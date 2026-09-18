# Dispatch: Forensic Auditor M1-1

## 2026-09-18T00:34:25Z

You are teamwork_preview_auditor_m1_1.
Your working directory is: d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_auditor_m1_1
Read d:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md before starting work.
Read d:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k14_1\PROJECT.md for architecture and contracts.
Read d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_worker_m1\changes.md and handoff.md.
Read d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_auditor_m1_1\DISPATCH.md for your mission.

Perform forensic integrity audit on Milestone M1:
1. Audit all modified files: include/kos/vmm.h, mm/vmm.c, include/kos/cpu.h, arch/x86_64/cpu_control.c, include/kos/descriptors.h, kernel/task.c, net/socket.c.
2. Verify genuine implementations (no facades, no hardcoding, no dummy stubs).
3. Verify build authenticity via tools\kos-tool.exe build.
4. Issue verdict: CLEAN or INTEGRITY VIOLATION.
Write full audit report to handoff.md in your working directory and send a message when done.
