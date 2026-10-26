# Security Policy

## Supported Versions

vinoS is an active research and desktop operating system project. Security updates are applied directly to the `main` branch.

| Version | Supported          |
| ------- | ------------------ |
| `main`  | :white_check_mark: |
| < 1.0   | :x:                |

---

## Reporting a Vulnerability

The vinoS team takes security seriously. If you discover a security vulnerability (such as privilege escalation from Ring 3 to Ring 0, memory safety faults in kernel page tables, or remote network buffer overflows), please report it responsibly:

1. **Do NOT report security vulnerabilities via public GitHub issues.**
2. Send an advisory report via private GitHub Security Advisories at [Security Advisories](https://github.com/kiyogo/vinoS/security/advisories) or contact the project maintainers directly via repository channels.
3. Please include:
   - A detailed description of the vulnerability.
   - Proof-of-concept (PoC) code or steps to reproduce under QEMU.
   - Analysis of the potential impact.

---

## Response Timeline

- **Initial Response**: Within 48 hours of report submission.
- **Triage & Assessment**: Within 5 business days.
- **Fix & Public Advisory**: Coordinated release following confirmation and patch validation.
