# KOS QA / CI Checklist

Run these gates from the repository root. A gate passes only when its command
returns code 0 and its generated checklist contains no `FAIL` entry.

| Gate | Command | Required result |
| --- | --- | --- |
| Boot smoke | `python tools/ci_runner.py smoke` | One-CPU boot, VFS multi-volume, DHCP, TCP, block, FAT32, and user file syscall markers pass |
| Storage integration | `python tools/storage_ci.py` | MBR and GPT images are read by AHCI, VirtIO, and NVMe; multi-sector and bounds checks pass |
| Full stress | `python tools/ci_runner.py full` | Four-CPU regression, fuzz, fault, and stress suites pass |
| Complete QA gates | `python tools/ci_runner.py all` | Smoke, full stress, and storage gates all return code 0 |

Generated logs and checklists live under `build/`, which is ignored by Git.
The local `c.py` source counter is also ignored; `tools/storage_ci.py` is the
repository storage integration harness and should remain part of the source.

## Phase 6 / Phase 7 Gates

The user file ABI gate also covers `lseek` (`SET` and `END`) and VFS-backed
`rename`. The full smoke command remains the required regression check after
changes to syscall numbers or handle lifecycle. It also requires the Ring 3
hardware `syscall` gate, which opens, writes, closes, and exits through the
real `IA32_LSTAR` entry path.

The common interrupt entry switches `GS` to the per-CPU kernel context for
ring-3 interrupts and restores the user context before `iretq`; this keeps
timer preemption and the `IA32_LSTAR` syscall path compatible.
