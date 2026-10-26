## Description

Please include a summary of the change and which subsystem is affected. List any dependencies that are required for this change.

Closes # (issue number if applicable)

## Type of Change

- [ ] :bug: Bug fix (non-breaking change which fixes an issue)
- [ ] :sparkles: New feature (non-breaking change which adds functionality)
- [ ] :zap: Performance optimization (e.g. framebuffer blit, memory paging)
- [ ] :recycle: Refactoring / Code cleanup
- [ ] :books: Documentation update
- [ ] :white_check_mark: Tests / CI automation

## Affected Subsystems

- [ ] Kernel core (`arch/x86_64`, `kernel/`, `mm/`)
- [ ] Drivers (`drivers/`, `framebuffer`, `storage`, `network`)
- [ ] File system (`fs/`, FAT32, partitions)
- [ ] User space & GUI (`assets/gui/`, `user/`, `lib/`)
- [ ] Build tools & CI (`tools/`)

## Checklist

- [ ] My code follows the kernel coding standards (`-Wall -Wextra -Werror` clean).
- [ ] I have performed a self-review of my own code.
- [ ] I have commented my code, particularly in hard-to-understand areas.
- [ ] I have verified changes using `python tools/ci_runner.py smoke`.
- [ ] If changing storage, `python tools/storage_ci.py` returns code 0.
- [ ] All QA regression checklist gates pass without FAIL entries.

## Screenshots / Demos (for GUI changes)

*(If applicable, paste screenshots or GIF captures from QEMU)*
