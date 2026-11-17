# Project Aegis: Advanced Network Forensics and Bare-Metal Driver Diagnostics
*Deep-Packet Inspection and Active Hardware-in-the-Loop Testing for the RTL8139*

---

## 1. Architectural Philosophy and Kernel-Space Execution

Project Aegis represents a paradigm shift in bare-metal network forensics, engineered specifically to operate within custom x86 kernel environments and low-level Rust driver implementations. By hooking directly into the `rtl8139_receive_one` polling routine, Aegis completely bypasses traditional socket abstractions. This provides unfiltered, ring-0 access to DMA receive buffers before they are ever touched or modified by higher-level OS network stacks, ensuring absolute cryptographic and forensic integrity of the captured data.

Operating directly in kernel space demands strict memory safety and bounded execution to prevent catastrophic system panics. Aegis is architected with rigid limits on maximum frame sizes (capped at 4096 bytes) and hardware polling iterations. This bounded architecture guarantees that forensic analysis will not hang the operating system in an infinite loop, even when confronted with maliciously malformed packets explicitly designed to trigger buffer over-reads or CPU starvation.

## 2. Basic Forensic Operations and Interface Validation

When initially bringing up network interfaces or verifying link-layer connectivity, basic commands provide immediate, noise-free visibility. A rapid diagnostic check is performed using `aegis capture --limit 5 --polls 256`. This instructs the kernel to poll the RTL8139 NIC up to 256 times to capture a maximum of 5 frames. It is highly effective for proving that hardware interrupts and DMA transfers are successfully placing data into system memory without committing the examiner to a long-term, blocking capture session.

For examiners preferring a guided interface over complex command-line arguments, Aegis includes a built-in terminal UI. Launching `aegis interactive` opens a custom console-based dashboard. This interface allows for rapid execution of single-frame captures, 60-second continuous follow modes, and hex-dump toggles, all while utilizing color-coded terminal output to distinguish between routine informational logs and critical security alerts.

## 3. The Protocol Analysis Engine

As raw frames are pulled from the receive ring, the Aegis engine systematically dissects them layer by layer. It begins by stripping Layer 2 Ethernet and VLAN (802.1Q) tags to identify the underlying protocol payload. It meticulously parses Layer 3 structures, automatically calculating IPv4 checksums in real-time. Aegis immediately flags packets with tampered, corrupted, or non-matching header checksums—a critical diagnostic feature when debugging custom network protocol implementations or identifying man-in-the-middle packet injection.

Beyond fundamental routing headers, the parser dives into Layer 4 and Layer 7 structures. It tracks TCP connection states by analyzing SYN, FIN, and RST flags, which is vital for spotting aggressive port scans or connection hijacking. At the application layer, Aegis scans plaintext payloads to parse DNS queries, detect TLS Client Hello messages, and monitor HTTP traffic for exposed state data like Authorization or Cookie headers, which are instantly flagged as security alerts.

## 4. Advanced Forensic Workflows and Threat Hunting

When the mission shifts from driver debugging to active threat hunting, Aegis deploys its advanced inspection parameters. Executing `aegis capture --follow --duration 120000 --verbose --hex` initiates a continuous, deep-inspection loop lasting exactly two minutes. The `--hex` directive provides a raw hexadecimal and ASCII dump of the first 96 bytes of every frame, giving analysts a direct visual on the binary payload structure traversing the wire.

Crucially, the `--verbose` flag in this advanced workflow activates real-time mathematical analysis of the packet payload. Aegis calculates the Shannon entropy score for every single frame on the fly. Entropy scores exceeding 7.0 automatically trigger a high-entropy alert. This is a primary indicator of obfuscated malware communications, packed executable transfers, or encrypted command-and-control (C2) payloads attempting to hide within standard traffic flows.

To assist analysts in bridging the gap between raw data and actionable intelligence, the `--explain` flag can be appended. Using `aegis capture --limit 100 --timestamps --explain` not only correlates every received frame with exact CPU tick counts for precise latency profiling, but it also outputs diagnostic `[WHY]` statements. For example, it will explicitly remind the examiner to cross-reference ARP sender addresses against target IPs to detect ARP spoofing attacks, or clarify how IPv6 extension headers are being reported.

## 5. Active Injection and Cryptographic Validation

Aegis is not strictly confined to passive listening; it includes a powerful, albeit highly restricted, active transmit capability designed for hardware-in-the-loop diagnostics and signature triggering. Because writing to the RTL8139 TX ring inherently modifies the network state and risks broadcast storms, active operations are guarded by mandatory safety locks. Simple link verification can be achieved via `aegis --send --confirm --lab-only --repeat 3 --delay 250`, which dispatches a standard 60-byte broadcast frame multiple times.

Advanced payload injection pushes this capability further. By executing `aegis --inject --write "CUSTOM_SHELLCODE_OR_DATA" --confirm --lab-only --prp-sha256`, an examiner can construct custom Ethernet frames containing precise byte sequences. The addition of the `--prp-sha256` argument is an advanced forensic safeguard: it forces the engine to compute and output a cryptographic SHA-256 hash of the exact frame structure *before* it is handed to the NIC for DMA transfer. This creates an irrefutable chain of custody, proving exactly what bytes were injected onto the network during a live fire test.