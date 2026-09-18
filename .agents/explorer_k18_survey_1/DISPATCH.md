## 2026-09-18T08:15:55Z
You are an Explorer subagent (Driver & Hardware Focus) for Milestone K18 & K19 in KOS (x86_64).
Your working directory is: d:/DATA/source/OS/KOS/OS2/.agents/explorer_k18_survey_1
You MUST read d:/DATA/source/OS/KOS/OS2/.agents/ORIGINAL_REQUEST.md (specifically the section ## Follow-up — 2026-09-18T08:10:49Z) before starting.

YOUR MISSION:
Investigate the existing codebase of KOS (d:/DATA/source/OS/KOS/OS2) focusing on Requirement R1:
1. Current Network Interface & E1000 Driver:
   - Where are the network drivers and network code located? (e.g. drivers/net/, net/, include/net/, kernel/net/ etc.)
   - How is the E1000 driver currently implemented? Does it use MMIO or I/O ports? PCI discovery?
   - How are RX and TX descriptor rings currently configured? Are they using circular rings, head/tail pointers, DMA buffers?
   - How are interrupts (IMS/ICR) handled? Is it currently polling or interrupt-driven? How does it integrate with APIC/IDT?
   - What network device / netif abstraction exists currently? Is the network stack tightly coupled to e1000?
   - How are packet buffers (sk_buff, net_buf, pbuf, or custom packet structs) allocated, managed, and freed? Is there memory leak risk or ring overflow handling?
2. Build system and headers:
   - How does `tools/kos-tool.exe build` compile the kernel? What flags and warnings are used?
   - What headers define network types and driver interfaces?

OUTPUT REQUIREMENTS:
- Write your comprehensive findings to `d:/DATA/source/OS/KOS/OS2/.agents/explorer_k18_survey_1/report.md` and create a `handoff.md`.
- When finished, send a message to your caller ("parent" with conversation ID 7fc9ec7b-5526-474f-b63e-fbc22772976f) with a summary and link to your report.
Remember: Do NOT edit any source code. You are an Explorer (read-only investigation).
