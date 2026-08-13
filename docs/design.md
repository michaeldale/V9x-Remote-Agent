# V9x Remote Agent

## Project plan and design context

> **Historical design document.** This plan was written on 2026-08-09, before the
> first public release. Milestones M0 to M4 described below are implemented and
> shipping; later sections describe intent, not necessarily current behaviour.
> Paths and environment details are illustrative. For current usage, start with
> the repository README and the docs alongside this file.

Status: planning draft  
Recorded: 2026-08-09  
Initial target: Windows 98 Second Edition running under 86Box  
Initial consumer: the Velocity9x display-driver project  
Intended repository: this repository

---

## 1. Executive summary

V9x Remote Agent will be a deliberately small remote-control and automated-test system for Windows 9x development guests. Its first purpose is to shorten the Velocity9x display-driver edit/build/install/reboot/test/recover loop, but its protocol and tooling should remain general enough to support other Windows 95/98/Me development tasks later.

The system will have two primary components:

1. `V9XAGNT.EXE`, a runtime-light 32-bit Win32 executable installed inside the Windows 98 guest.
2. `v9xctl.ps1`, a host-side command-line client and automation surface designed to be called by a developer, Claude, Codex, or another scripted agent.

The guest executable will listen on a TCP port, accept versioned framed requests, execute Win32 or `COMMAND.COM` commands, transfer files, report machine state, reboot or shut down Windows, and return structured results. It will start automatically on boot and reconnect cleanly after a reboot.

The TCP connection is the control plane. The existing 86Box COM1 named-pipe path remains the independent diagnostic plane for display DRV and VxD output. This separation is essential: if the display stack hangs or crashes the guest, the TCP agent may disappear, while serial output and the host-side VM watchdog can still reveal the last successful checkpoint and preserve or restore the virtual disk.

The protocol will intentionally have no authentication, encryption, or user-account model. This is acceptable only because the guest is a disposable development sandbox. The containment boundary must instead be enforced by 86Box networking and the host firewall. The service must not be exposed directly to a home, office, public, or otherwise untrusted network.

The fastest useful first release is not a complete remote debugger. It is a reliable remote execution, file transfer, reboot, and result-collection agent. Source-level and ring-0 debugging remain the responsibility of 86Box's debugger, serial instrumentation, and any later DDK-compatible debugger arrangement.

---

## 2. Problem statement

Velocity9x is a ground-up Windows 9x display-driver project. The current development loop includes several slow or manual operations:

- building Win16 display-driver and 32-bit mini-VDD artifacts on the host;
- transferring artifacts into the Windows 98 guest;
- running preflight programs;
- associating or updating a display driver;
- rebooting or cold-starting the guest;
- waiting for the Windows desktop;
- checking serial output;
- launching GDI tests and dismissing dialogs;
- copying results back to the host;
- recovering a failed VM when the display stack prevents a usable desktop.

The current folder-CD and manual Device Manager workflow is safe but expensive when repeated frequently. A remote guest agent can eliminate most mouse-driven work and provide stable, machine-readable results to an AI coding agent.

There are two distinct failure classes:

1. **The guest remains alive.** The display output may be wrong, but networking and Win32 process execution still work. The TCP agent can collect diagnostics, run tests, repair files, change configuration, and reboot.
2. **The guest is hung or cannot boot.** No guest program can repair this reliably. The host must detect the missing agent, stop the disposable VM, preserve the failed artifacts, and restore a known-good disk/configuration/NVR set.

The design must address both cases and must never treat the guest agent as the only recovery mechanism.

---

## 3. Current Velocity9x and VM context

This section captures the environment that motivated the project so the new repository will retain enough context after it is separated from Velocity9x.

### 3.1 Driver project

The current source repository is:

```text
<velocity9x-repo>
```

The first hardware target is:

```text
S3 ViRGE/DX 86C375
PCI vendor/device: 5333:8A01
Guest OS: Windows 98 Second Edition
Primary emulator: 86Box
```

At the time of this plan, the driver project includes:

- an Open Watcom host and Win9x build path;
- a Windows 98 DDK-dependent mini-VDD build path;
- a Win16 display DRV with a DIB Engine-based framebuffer candidate;
- a mini-VDD that currently preserves master-VDD defaults;
- 640x480, 800x600, and 1024x768 modes at 8 and 16 bpp;
- a hardware-inert dynamic VxD lifecycle probe;
- a Win16 loader and GDIINFO/mode-validation probe;
- a consolidated `V9XSTAGE.EXE` preflight utility;
- a `V9XGDI.EXE` framebuffer/GDI test;
- an installable, tightly matched Windows 98 INF;
- a repair INF and batch file for replacing already-installed driver binaries;
- live COM1 capture through an 86Box named pipe;
- cold profile backup and recovery scripts.

Dynamic in-session display mode switching is deliberately disabled. Mode changes currently take effect after reboot.

### 3.2 Relevant existing files

The future implementation should integrate with or learn from these current files:

```text
<velocity9x-repo>\README.md
<velocity9x-repo>\PLAN.md
<velocity9x-repo>\docs\vm-environment.md
<velocity9x-repo>\docs\specifications\logging-protocol.md
<velocity9x-repo>\packaging\win98se\velocity9x.inf
<velocity9x-repo>\packaging\win98se\V9XFIX.INF
<velocity9x-repo>\packaging\win98se\V9XFIX.BAT
<velocity9x-repo>\packaging\win98se\INSTALL.TXT
<velocity9x-repo>\packaging\win98se\FIRSTBOOT.TXT
<velocity9x-repo>\packaging\win98se\RECOVER.TXT
<velocity9x-repo>\scripts\build-active-package.ps1
<velocity9x-repo>\scripts\prepare-vm-probe.ps1
<velocity9x-repo>\scripts\capture-serial-pipe.ps1
<velocity9x-repo>\scripts\backup-86box-profile.ps1
<velocity9x-repo>\scripts\build-win32-serial-smoke.ps1
<velocity9x-repo>\tools\diag\driver_stage_win32.c
<velocity9x-repo>\tools\diag\gdi_smoke_win32.c
```

These are source inputs and integration references. The standalone agent repository must not silently modify the Velocity9x repository.

### 3.3 Current 86Box environment

The currently documented 86Box 6 profile is:

```text
86Box executable: C:\86Box\86Box.exe
Profile:          <your 86Box VMs directory>\Win86SE
Direct launch:    C:\86Box\86Box.exe -P "<your 86Box VMs directory>\Win86SE"
Configuration:    <your 86Box VMs directory>\Win86SE\86box.cfg
```

The profile currently contains an emulated NE2000 PnP adapter configured for SLiRP:

```ini
[Network]
net_01_card = ne2kpnp
net_01_net_type = slirp
```

The working guest disk recorded by Velocity9x is approximately 504 MB and is backed up cold together with `86box.cfg` and the profile's NVR directory. The existing backup script refuses to operate while any 86Box process is running.

COM1 is already proven as an 86Box named-pipe server named:

```text
velocity9x-com1
```

This path has successfully delivered live ring-0 dynamic-VxD lifecycle messages to the host.

### 3.4 Existing build constraints

Open Watcom is installed at approximately:

```text
C:\WATCOM
```

The current installation contains:

```text
C:\WATCOM\h\nt\winsock2.h
C:\WATCOM\lib386\nt\ws2_32.lib
C:\WATCOM\lib386\nt\wsock32.lib
```

Existing Win32 diagnostic executables are built without the Open Watcom C runtime. This is intentional because the default runtime can introduce Unicode startup imports that the Windows 9x loader does not provide. The remote agent should use the same approach initially:

- `-bt=nt` compilation target;
- Windows 4.0 runtime declaration in the PE image;
- no default libraries;
- explicit Win32 import libraries;
- custom entry point;
- ANSI Win32 APIs only;
- static, bounded memory wherever practical.

Expected import libraries for the first agent build are likely:

```text
KERNEL32.LIB
ADVAPI32.LIB
WS2_32.LIB
GDI32.LIB       only when screenshot support is enabled
USER32.LIB      only for desktop detection or screenshot support
```

The implementation must audit the produced import table and reject accidental Unicode-only or post-Windows-98 imports.

---

## 4. Goals

### 4.1 Primary goals

1. Provide reliable remote native-command execution inside Windows 98SE.
2. Transfer build artifacts and logs without remounting an 86Box folder CD for every edit.
3. Return stdout, stderr, exit codes, Win32 errors, and timeouts in a machine-readable form.
4. Remain installed and available across normal Windows reboots.
5. Make reboot-and-reconnect a first-class operation rather than an accidental disconnect.
6. Integrate with existing Velocity9x build, VxD probe, display-driver staging, GDI testing, serial capture, and recovery procedures.
7. Let an AI coding agent drive repeatable tests using stable CLI commands and JSON output.
8. Support future Windows 9x development workflows without being coupled internally to one display driver.
9. Fail safely under malformed requests, dropped TCP connections, child-process hangs, and partial file transfers.
10. Preserve independent host-side recovery for guest hangs and boot failures.

### 4.2 Secondary goals

- Capture BMP screenshots after the Windows desktop is ready.
- Query and update carefully scoped registry values.
- List and terminate test processes.
- Maintain build and job identities across reboots.
- Package all artifacts from a test cycle into one host-side result directory.
- Allow more specialized helper executables, such as a Windows 98 display-device installer, to be invoked through the generic agent.

---

## 5. Non-goals

The first releases will not attempt to provide:

- authentication, encryption, certificates, or user accounts;
- internet-safe or LAN-safe remote administration;
- a general-purpose production remote-access product;
- kernel debugging, breakpoints, register inspection, or source-level VxD debugging;
- protection against a malicious client that can reach the TCP port;
- recovery from a completely hung guest using guest-side code;
- transparent live migration or snapshots;
- a replacement for cold VHD/config/NVR backups;
- Unicode filesystem support beyond what Windows 98 and ANSI APIs provide;
- concurrent multi-user sessions;
- arbitrary plugin loading in the guest process;
- a dependency on modern runtimes, .NET, PowerShell, Python, or Visual C++ inside the guest.

The agent will effectively grant full control over the Windows 98 guest to anyone who can connect. That is an explicit property of this development-only design.

---

## 6. Security and containment model

### 6.1 Protocol security

The protocol will contain no login, password, encryption, or authorization step. A client that completes protocol negotiation can execute commands and alter guest files.

This deliberately minimizes implementation complexity and avoids bringing cryptographic dependencies into the Windows 98 guest. It also means the network configuration is part of the safety design, not an optional deployment detail.

### 6.2 Required containment

The recommended initial topology is:

```text
host client -> 127.0.0.1:9869 -> 86Box SLiRP forward -> guest TCP 9869
```

Add a port-forwarding section to the VM configuration:

```ini
[SLiRP Port Forwarding #1]
0_protocol = tcp
0_external = 9869
0_internal = 9869
```

86Box documents that SLiRP-forwarded ports can be reached through both localhost and the host's network address. Therefore the host firewall should restrict incoming TCP port 9869 to the local machine. If another physical machine must drive tests, access should go through a trusted host-level mechanism such as an existing VPN or remote desktop session, then connect locally to the 86Box forward.

Alternative containment for later evaluation is a private PCap network using the Microsoft KM-TEST loopback adapter. That can provide a host-only virtual Ethernet segment, but it adds host setup and Npcap dependencies. SLiRP forwarding is the lowest-friction starting point for the current Windows host.

### 6.3 Defensive limits despite trusted use

Even in the sandbox, the server should enforce mechanical limits:

- maximum frame payload: 64 KiB;
- maximum command-line size: 4 KiB;
- maximum working-directory size: 260 ANSI bytes;
- maximum environment override data: 16 KiB;
- default command timeout: 60 seconds;
- maximum command timeout: configurable, initially 15 minutes;
- default captured output ceiling: 4 MiB per stream;
- maximum single uploaded file: initially 64 MiB;
- only one active execution request;
- only one active upload and one active download;
- incomplete uploads use `.PART` files and never replace the destination;
- all protocol lengths are validated before allocation or read;
- every accepted connection begins with a version handshake;
- unsupported opcodes return an error rather than being ignored.

These limits defend against accidents, corrupt clients, and implementation bugs. They do not create a security boundary against a hostile client with `EXEC` access.

---

## 7. High-level architecture

```text
Developer / Claude / Codex
        |
        | invokes stable commands and consumes JSON
        v
scripts\v9xctl.ps1
        |
        | TCP to 127.0.0.1:9869
        v
86Box SLiRP port forwarding
        |
        | TCP to guest port 9869
        v
V9XAGNT.EXE (Win98 user mode)
        |        |          |           |
        |        |          |           +-- reboot/shutdown
        |        |          +-------------- file and registry operations
        |        +------------------------- child-process execution
        +---------------------------------- machine and display information

Independent diagnostic path:

display DRV / mini-VDD / probes -> COM1 -> 86Box named pipe -> host log

Independent recovery path:

host watchdog -> 86Box process -> disposable VHD/config/NVR restore
```

The guest agent must not directly contain Velocity9x driver logic. Driver-specific behavior belongs in host orchestration or narrowly scoped guest helpers such as `V9XINST.EXE`.

---

## 8. Guest executable design

### 8.1 Program identity

Proposed executable name:

```text
V9XAGNT.EXE
```

The short name is intentional for Windows 9x, DOS tools, recovery consoles, and log readability.

Proposed installation directory:

```text
C:\V9XREMOTE
```

Proposed files:

```text
C:\V9XREMOTE\V9XAGNT.EXE
C:\V9XREMOTE\AGENT.INI
C:\V9XREMOTE\AGENT.LOG
C:\V9XREMOTE\BOOT.DAT
C:\V9XREMOTE\PENDING.DAT
C:\V9XREMOTE\JOBS\<job-id>\...
C:\V9XREMOTE\TEMP\...
```

No configuration file should be required for defaults. `AGENT.INI` may later override the port, log level, staging root, output limits, and serial startup marker.

### 8.2 Startup lifecycle

The agent should be registered under the Windows 9x machine-wide `RunServices` key so it starts without requiring a user to open it after every reboot:

```text
HKLM\Software\Microsoft\Windows\CurrentVersion\RunServices
    V9xRemoteAgent = C:\V9XREMOTE\V9XAGNT.EXE -service
```

A small installer batch file or `V9XAGNT.EXE -install` mode can create the directory, copy the executable, and set the registry value. An `-uninstall` mode should remove the autostart entry and optionally leave logs intact.

Boot behavior:

1. Acquire a global named mutex.
2. Load configuration and increment the persistent boot counter.
3. Open the rolling log.
4. Call `WSAStartup` and retry if the network subsystem is not ready.
5. Create the listening socket.
6. Accept one client.
7. Serve requests until disconnect.
8. Return to `accept` without exiting.

The process should tolerate the TCP/IP stack becoming available several seconds after `RunServices` programs begin.

### 8.3 Process model

The first implementation should remain simple:

- one listener;
- one connected client;
- one active long-running operation;
- a small network/control loop;
- a worker path for child-process execution and file I/O;
- bounded buffers;
- no dynamically loaded guest plugins.

The control loop must remain able to answer `PING` and `CANCEL` while a child process runs. This can be implemented with either:

- one network thread and one worker thread; or
- a single loop using finite `WaitForSingleObject` intervals and socket polling.

The two-thread design will likely be clearer. Thread creation and teardown should be fixed and bounded rather than one thread per message.

### 8.4 Command execution

Two explicit modes are required.

#### Direct execution

Launch an executable with `CreateProcessA` using the supplied command line and working directory. This is preferred for test programs because it preserves an unambiguous exit code.

Example conceptual request:

```text
application: C:\V9XREMOTE\JOBS\abc123\V9XSTAGE.EXE
arguments:   /quiet
directory:   C:\V9XREMOTE\JOBS\abc123
timeout:     60000 ms
```

#### Shell execution

Run legacy built-ins and batch files through:

```text
C:\COMMAND.COM /D /C <command>
```

The exact supported `COMMAND.COM` switches must be verified on the target Windows 98SE image. Do not assume `cmd.exe` exists.

#### Output capture

The implementation should try anonymous-pipe redirection first, using inheritable handles passed through `STARTUPINFOA`. The server must drain both stdout and stderr while the process runs to avoid pipe-buffer deadlocks.

If pipe behavior proves unreliable for Win16 or specific Windows 9x processes, support a fallback that redirects to unique temporary files and streams them after process completion. The protocol should report which capture mode was used.

GUI applications may not write output. Their exit code and any generated result file remain valid signals.

#### Timeout and cancellation

On timeout or `CANCEL`:

1. attempt a controlled close only if the request opted into it;
2. otherwise call `TerminateProcess` for the child created by the request;
3. close all inherited handles;
4. return a timeout/cancel status plus any captured output;
5. leave the agent alive.

Windows 9x does not provide modern job objects, so terminating an entire descendant process tree cannot be guaranteed. Tests should prefer single-process wrappers that wait for their own children, as `V9XSTAGE.EXE` already does for the Win16 loader.

### 8.5 File transfer

Uploads should be transactional:

1. client sends `FILE_OPEN_WRITE` with final path, expected size, and expected CRC32;
2. agent creates a unique `.PART` file in the destination directory;
3. client sends ordered chunks;
4. agent tracks received length and running CRC32;
5. client sends `FILE_COMMIT`;
6. agent flushes and closes the file;
7. agent validates size and CRC32;
8. only then does it rename the temporary file to the final name.

An interrupted upload leaves only a disposable `.PART` file. It must never truncate a known-good driver binary before the final validation succeeds.

Downloads use a similar open/chunk/end sequence and report size plus CRC32.

Directory upload is implemented by the host enumerating files and issuing `MKDIR` plus individual uploads. This avoids requiring ZIP, CAB, or another archive implementation in the guest.

### 8.6 Machine information

`HELLO` or `INFO` should return at least:

- protocol version;
- agent version and embedded build ID;
- capability flags;
- computer name;
- Windows version reported by the guest;
- system directory and Windows directory;
- current working directory;
- Winsock version negotiated;
- boot counter;
- process uptime;
- current job/resume token, if any;
- configured TCP port;
- current screen width, height, and bits per pixel when available;
- whether a desktop window appears ready;
- free disk space for the staging volume, if practical with Win98-compatible APIs.

The host should record this information at the beginning and end of every driver cycle.

### 8.7 Screenshot support

Screenshot capture is valuable for display-driver testing but should be an optional operation invoked only after basic guest health is established.

Proposed implementation:

1. obtain the screen DC with `GetDC(NULL)`;
2. obtain screen dimensions with `GetSystemMetrics` or `GetDeviceCaps`;
3. create a compatible memory DC and bitmap;
4. copy the screen with `BitBlt`;
5. convert the result with `GetDIBits` to a 24-bit bottom-up or top-down DIB;
6. write a standard uncompressed BMP;
7. return it with the normal file-download mechanism.

The returned metadata should include source width, height, guest-reported bpp, captured BMP format, and Win32 error on failure.

A broken display driver can hang or fault during GDI capture. Therefore screenshot failure must not replace serial checkpoint evaluation, and the host should invoke it only after the agent has responded to a fresh `PING`.

### 8.8 Logging

The guest agent log should be plain ASCII and bounded, for example 256 KiB with one rotated predecessor:

```text
AGENT.LOG
AGENT.OLD
```

Each entry should contain:

- boot counter;
- monotonic tick count;
- event name;
- request ID where applicable;
- result/error code;
- short bounded detail.

The agent should not normally use COM1 because the serial channel is reserved for the code under test and interleaved messages may make ring-0 traces harder to interpret. An optional single startup marker can be considered if it materially improves reboot correlation.

---

## 9. Wire protocol

### 9.1 Design principles

The protocol should be:

- small enough to implement without a C runtime;
- explicitly versioned;
- insensitive to TCP fragmentation and coalescing;
- capable of binary file transfer;
- easy to fuzz and unit-test on the host;
- deterministic for AI-driven automation;
- independent of JSON parsing inside Windows 98;
- extensible through capability flags and ignored optional fields.

The guest does not need to parse JSON. The host client can translate structured binary or TLV responses into JSON for callers.

### 9.2 Byte order and encoding

- All integers are unsigned little-endian unless explicitly stated.
- Text fields are length-prefixed byte strings.
- Initial implementation text is restricted to 7-bit ASCII for commands, paths, keys, and values.
- Protocol strings are not NUL-terminated on the wire.
- Guest filesystem operations use ANSI `A` APIs.
- Frame payloads must not exceed 65,536 bytes.

ASCII-only paths are sufficient for the current environment and avoid ambiguous code-page conversion. Later protocol versions may define UTF-8-to-ANSI behavior explicitly.

### 9.3 Frame header

Proposed fixed header, 24 bytes:

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `V9XR` |
| 4 | 2 | version | protocol major/minor encoding |
| 6 | 2 | type | request, response, stream, or event opcode |
| 8 | 4 | request_id | nonzero client-selected correlation ID |
| 12 | 4 | flags | type-specific flags |
| 16 | 4 | payload_length | bytes following the header |
| 20 | 4 | reserved | zero in protocol v1 |

TCP receive code must read exactly the header, validate it, then read exactly the declared payload. It must not assume one `recv` call returns one complete frame.

### 9.4 Handshake

Immediately after connection:

1. client sends `HELLO_REQUEST` with supported protocol range and client label;
2. server replies `HELLO_RESPONSE` with selected version, capabilities, limits, agent build, boot counter, and pending job ID;
3. either side closes the connection if there is no compatible version.

No operational request is accepted before a successful handshake.

### 9.5 Initial message types

Proposed request/response groups:

```text
0x0001 HELLO_REQUEST
0x8001 HELLO_RESPONSE

0x0002 PING_REQUEST
0x8002 PING_RESPONSE

0x0010 EXEC_REQUEST
0x8010 EXEC_ACCEPTED
0x9010 EXEC_STDOUT
0x9011 EXEC_STDERR
0x8011 EXEC_COMPLETE
0x0011 CANCEL_REQUEST
0x8012 CANCEL_RESPONSE

0x0020 FILE_STAT_REQUEST
0x8020 FILE_STAT_RESPONSE
0x0021 FILE_LIST_REQUEST
0x8021 FILE_LIST_RESPONSE
0x0022 FILE_MKDIR_REQUEST
0x8022 FILE_MKDIR_RESPONSE
0x0023 FILE_OPEN_WRITE
0x0024 FILE_WRITE_CHUNK
0x0025 FILE_COMMIT
0x8025 FILE_WRITE_COMPLETE
0x0026 FILE_OPEN_READ
0x9026 FILE_READ_CHUNK
0x8026 FILE_READ_COMPLETE

0x0030 INFO_REQUEST
0x8030 INFO_RESPONSE
0x0031 PROCESS_LIST_REQUEST
0x8031 PROCESS_LIST_RESPONSE
0x0032 PROCESS_KILL_REQUEST
0x8032 PROCESS_KILL_RESPONSE

0x0040 REBOOT_REQUEST
0x8040 REBOOT_ACCEPTED
0x0041 SHUTDOWN_REQUEST
0x8041 SHUTDOWN_ACCEPTED

0x0050 SCREENSHOT_REQUEST
0x8050 SCREENSHOT_RESPONSE

0x0060 REG_GET_REQUEST
0x8060 REG_GET_RESPONSE
0x0061 REG_SET_REQUEST
0x8061 REG_SET_RESPONSE

0x8FFF ERROR_RESPONSE
```

Exact numeric values may change before implementation, but once a released protocol version is used in a guest image they should remain stable.

### 9.6 Execution request fields

`EXEC_REQUEST` should contain TLV or fixed ordered fields for:

- execution mode: direct or shell;
- application path, optional for shell mode;
- command line;
- working directory;
- timeout milliseconds;
- stdout limit;
- stderr limit;
- window policy: hidden by default;
- environment inheritance flag;
- optional environment overrides;
- optional expected exit-code set;
- optional job label.

`EXEC_COMPLETE` should contain:

- result category;
- child exit code if available;
- `GetLastError` value when creation or capture failed;
- elapsed milliseconds;
- stdout and stderr byte counts;
- truncation flags;
- timeout flag;
- cancellation flag;
- capture-mode indicator.

The host CLI decides whether a nonzero child exit code becomes a failing host process exit code.

### 9.7 Error model

Errors should contain both a stable protocol error and an optional native Win32 error:

```text
OK
BAD_MAGIC
UNSUPPORTED_VERSION
UNSUPPORTED_OPERATION
INVALID_PAYLOAD
LIMIT_EXCEEDED
BUSY
NOT_FOUND
ACCESS_DENIED
CREATE_FAILED
IO_FAILED
CRC_MISMATCH
TIMEOUT
CANCELLED
REBOOT_REQUIRED
INTERNAL_ERROR
```

Textual detail is diagnostic only. Automation must use the stable numeric code.

### 9.8 Capability discovery

The handshake should advertise flags such as:

```text
CAP_EXEC_DIRECT
CAP_EXEC_SHELL
CAP_EXEC_CANCEL
CAP_FILE_READ
CAP_FILE_WRITE
CAP_POWER_CONTROL
CAP_SCREENSHOT_BMP
CAP_PROCESS_LIST
CAP_PROCESS_KILL
CAP_REGISTRY_READ
CAP_REGISTRY_WRITE
CAP_DRIVER_HELPER
```

This lets a newer host client work with an older guest agent and select a compatible workflow.

---

## 10. Host client design

### 10.1 Why PowerShell first

The host is a current Windows system and the Velocity9x repository already uses PowerShell extensively. A PowerShell client can use .NET sockets, emit JSON, integrate with existing build scripts, and be invoked easily by both humans and AI coding agents.

The guest must remain independent of PowerShell and .NET.

The first host interface should be:

```text
scripts\v9xctl.ps1
```

A later native `v9xctl.exe` can be added if startup performance, distribution, or cross-platform operation justifies it. The wire protocol must not depend on the PowerShell implementation.

### 10.2 CLI behavior

Every command should:

- have stable parameter names;
- write human-readable output by default;
- support `-Json` for structured output;
- return exit code 0 only when the requested operation succeeds;
- distinguish transport failure, protocol failure, and guest-command failure;
- include the request ID and agent boot counter in JSON;
- avoid prompts in normal operation;
- accept explicit `-Host` and `-Port` overrides;
- default to `127.0.0.1:9869`.

Conceptual examples:

```powershell
.\scripts\v9xctl.ps1 ping
.\scripts\v9xctl.ps1 info -Json
.\scripts\v9xctl.ps1 exec `
    -Application C:\V9XREMOTE\JOBS\abc123\V9XSTAGE.EXE `
    -Arguments /quiet `
    -WorkingDirectory C:\V9XREMOTE\JOBS\abc123 `
    -TimeoutSeconds 60
.\scripts\v9xctl.ps1 shell -Command "VER"
.\scripts\v9xctl.ps1 put `
    -Source .\build\win98se-active\V9XDISP.DRV `
    -Destination C:\V9XREMOTE\JOBS\abc123\V9XDISP.DRV
.\scripts\v9xctl.ps1 push-tree `
    -Source .\build\win98se-active `
    -Destination C:\V9XREMOTE\JOBS\abc123
.\scripts\v9xctl.ps1 reboot -JobId abc123 -WaitSeconds 180
.\scripts\v9xctl.ps1 screenshot `
    -Destination .\build\vm-results\abc123\desktop.bmp
.\scripts\v9xctl.ps1 get `
    -Source C:\V9XREMOTE\AGENT.LOG `
    -Destination .\build\vm-results\abc123\agent.log
```

### 10.3 Host exit-code categories

Suggested host exit codes:

```text
0   success
10  CLI usage error
11  local file/precondition error
20  TCP connection failure
21  handshake/version failure
22  protocol violation
23  guest returned protocol error
30  guest process could not start
31  guest process returned unexpected exit code
32  guest process timed out
33  guest process was cancelled
40  file transfer or CRC failure
50  reboot did not disconnect as expected
51  guest did not reconnect before deadline
52  reconnected guest did not report expected job ID
60  driver-test acceptance failure
70  VM recovery required
```

The exact values can change before first release, but category separation is important for automated diagnosis.

### 10.4 Reboot waiting

`reboot -WaitSeconds N` should implement a state machine:

1. connect and record agent build plus boot counter;
2. send reboot request with a generated or supplied job ID;
3. require `REBOOT_ACCEPTED` before treating disconnect as expected;
4. wait for the socket to close;
5. poll the configured endpoint with bounded retry and backoff;
6. complete a new handshake;
7. require a greater boot counter;
8. require the expected persisted job ID;
9. optionally wait for the desktop-ready condition;
10. report elapsed boot time and new machine information.

A simple reconnect without a changed boot counter is not proof of reboot.

---

## 11. Driver-specific integration

### 11.1 Separation of responsibilities

The generic agent should provide transport and native operations. Driver-specific policy belongs in:

- the host orchestration script;
- the Velocity9x package and manifests;
- a dedicated guest installer/helper when Windows 98 device setup requires one.

This prevents the remote agent from becoming tightly coupled to one hardware ID, INF layout, or driver family.

### 11.2 Development package upload

The host builds the existing Velocity9x active package, then uploads it into an immutable job directory:

```text
C:\V9XREMOTE\JOBS\<build-id>
```

The package already contains SHA-256 hashes generated on the host. The transfer protocol validates CRC32 during transfer. The guest does not need a complete SHA-256 implementation in the first release; the host can download critical files after transfer if end-to-end host-side SHA verification becomes necessary.

The host should reject ambiguous build IDs and use the same restricted character set currently used by Velocity9x:

```text
letters, digits, dot, underscore, plus, and hyphen
```

### 11.3 Preflight

The remote workflow should run the consolidated lifecycle preflight before altering the installed driver:

```text
V9XSTAGE.EXE /quiet
```

The current program passes `/quiet` to its Win16 child but still shows a final message box. Velocity9x should gain a true unattended mode that:

- shows no message boxes;
- returns a stable exit code;
- optionally writes a small result file;
- continues writing lifecycle checkpoints to COM1.

The remote controller should refuse to install/update after a failed preflight unless an explicit force flag is supplied by the developer.

### 11.4 Updating an already-associated driver

The current Velocity9x repair path is the fastest automation target. `V9XFIX.INF` contains a `DefaultInstall` section that replaces `V9XDISP.DRV` and `V9XMINI.VXD`. `V9XFIX.BAT` invokes Windows 98's `RUNDLL.EXE` and `SETUPX.DLL,InstallHinfSection` with mode 132.

Once the Velocity9x driver is already associated with the correct display devnode, the remote cycle can:

1. upload a new package;
2. run the preflight;
3. run the repair INF command from the uploaded directory;
4. confirm that the command returned and any expected staging state exists;
5. request a controlled shutdown or reboot;
6. reconnect after boot;
7. correlate TCP job identity and COM1 build identity;
8. run post-boot tests;
9. collect artifacts.

This should eliminate most repeated Device Manager interaction.

### 11.5 First display-device association

The main `VELOCITY9X.INF` contains a Chicago display class and manufacturer/model section matching only:

```text
PCI\VEN_5333&DEV_8A01
```

The current safe procedure selects it through Device Manager, Update Driver, and Have Disk. A generic `DefaultInstall` invocation is not equivalent to binding that model to the existing Plug and Play display devnode.

The first releases of V9x Remote Agent should preserve this one-time manual association. A later milestone should build a separate helper:

```text
V9XINST.EXE
```

That helper should use Windows 98 DDK-supported SetupX/Configuration Manager facilities to:

- enumerate present display-class/devnode information;
- locate exactly `PCI\VEN_5333&DEV_8A01`;
- report the current driver/provider/description;
- validate the supplied INF and selected model;
- perform the same binding as the interactive Have Disk flow;
- defer reboot so the controller can finish logging and state capture;
- return a structured result and reboot-required indication;
- refuse unsupported hardware IDs;
- refuse installation if the recovery prerequisites are not recorded.

This requires a focused compatibility spike against the actual Windows 98 DDK headers and exports. Modern Windows SetupAPI documentation cannot be assumed to describe Windows 98 behavior exactly.

The helper must remain separately testable and separately versioned from the TCP agent.

### 11.6 Mode selection

Velocity9x currently selects modes through registry state and requires reboot for each change. Future remote mode-matrix automation may:

1. query the active display devnode and supported mode registry branches;
2. set only a known supported mode value;
3. persist the desired mode in the job manifest;
4. reboot;
5. compare the post-boot screen dimensions/bpp to the request;
6. run GDI and pixel tests;
7. capture a screenshot;
8. proceed to the next mode only after the current mode passes.

Registry paths differ between machines and devnode instances. The host must not hard-code an instance path discovered on only one VM. A guest helper should locate the correct active display branch from the exact PCI hardware identity.

### 11.7 GDI test automation

The current `V9XGDI.EXE`:

- creates a visible test window;
- draws colors, lines, text, BitBlt and StretchBlt content;
- performs tolerant `GetPixel` checks;
- shows a PASS/FAIL message box;
- leaves the window open for manual cursor inspection.

Add an unattended mode, conceptually:

```text
V9XGDI.EXE /quiet /result C:\V9XREMOTE\JOBS\abc123\GDI.TXT /bmp C:\V9XREMOTE\JOBS\abc123\GDI.BMP
```

The quiet mode should:

- perform deterministic drawing and readback;
- suppress all message boxes;
- return a stable exit code;
- write the build ID, current display metrics, and individual check results;
- optionally save the test window or screen as BMP;
- close automatically;
- keep a separate interactive mode for cursor inspection.

The host should treat the numeric/pixel result as primary and the screenshot as supporting evidence.

### 11.8 Serial correlation

Every test cycle should use a single build/job ID in:

- the compiled Velocity9x driver artifacts;
- the uploaded job directory;
- host CLI output;
- the reboot resume token;
- the serial-capture filename;
- the result directory;
- any screenshot and GDI result files.

Expected serial checkpoints currently include mini-VDD initialization and display-driver query/mapping/enable milestones. The host orchestrator should parse these as bounded exact prefixes, retain unknown lines, and report the last observed checkpoint on failure.

### 11.9 Recovery

Preferred recovery remains restoration of a cold pre-install disk/config/NVR copy. Safe Mode and Standard PCI Graphics Adapter recovery are useful manual fallbacks, but network support may not be available in Safe Mode and the remote agent must not be assumed reachable.

Routine automated testing should use a disposable working copy rather than repeatedly mutate the golden baseline.

On boot timeout or hard hang, the host should:

1. stop waiting for the guest agent after the configured deadline;
2. stop or terminate only the explicitly identified 86Box test process;
3. preserve the serial log, host log, job manifest, and failed working disk if requested;
4. never overwrite the golden backup with failed state;
5. restore or recreate the disposable working profile;
6. report that host-level recovery was required.

The exact stop/kill policy must account for virtual-disk corruption risk. A graceful guest shutdown is always preferred when the agent is responsive.

---

## 12. End-to-end host orchestration

Proposed high-level script:

```text
scripts\run-driver-cycle.ps1
```

### 12.1 State machine

```text
PRECHECK
  -> BUILD
  -> PREPARE_DISPOSABLE_VM
  -> START_SERIAL_CAPTURE
  -> START_VM
  -> WAIT_AGENT
  -> RECORD_GUEST_INFO
  -> UPLOAD_PACKAGE
  -> RUN_PREFLIGHT
  -> APPLY_UPDATE
  -> REQUEST_REBOOT_OR_SHUTDOWN
  -> WAIT_DISCONNECT
  -> WAIT_RECONNECT
  -> VERIFY_JOB_AND_BOOT_COUNTER
  -> VERIFY_SERIAL_CHECKPOINTS
  -> WAIT_DESKTOP
  -> RUN_GDI_TEST
  -> CAPTURE_SCREENSHOT
  -> COLLECT_RESULTS
  -> CONTROLLED_SHUTDOWN
  -> FINALIZE_RESULT
```

Failure from any state transitions to:

```text
CAPTURE_AVAILABLE_EVIDENCE
  -> ATTEMPT_CONTROLLED_SHUTDOWN_IF_RESPONSIVE
  -> HOST_STOP_IF_REQUIRED
  -> PRESERVE_FAILED_STATE_IF_CONFIGURED
  -> RESTORE_DISPOSABLE_VM
  -> REPORT_FAILURE
```

### 12.2 Result directory

Each run should create one host-side directory such as:

```text
build\vm-results\20260809-143000-<build-id>
```

Suggested contents:

```text
RUN.JSON
SUMMARY.TXT
HOST.LOG
AGENT-BEFORE.JSON
AGENT-AFTER.JSON
AGENT.LOG
SERIAL.LOG
PREFLIGHT.STDOUT
PREFLIGHT.STDERR
PREFLIGHT.JSON
INSTALL.JSON
REBOOT.JSON
GDI.TXT
GDI.BMP
DESKTOP.BMP
PACKAGE-SHA256.TXT
86BOX-CONFIG-SNAPSHOT.INI
FAILURE.TXT             only on failure
```

`RUN.JSON` is the authoritative machine-readable record. It should include timestamps, durations, tool versions, build IDs, guest boot counters, requested mode, exit codes, serial checkpoints, and recovery actions.

### 12.3 Automated agent usability

An AI coding agent should be able to execute one command and receive a bounded result:

```powershell
.\scripts\run-driver-cycle.ps1 -BuildId <id> -Mode 800x600x16 -Json
```

The script should never require the AI to infer success from a screenshot alone. It must produce an explicit final status such as:

```json
{
  "status": "pass",
  "buildId": "abc123",
  "mode": "800x600x16",
  "bootCounterBefore": 41,
  "bootCounterAfter": 42,
  "preflightExitCode": 0,
  "gdiExitCode": 0,
  "serialEnableCheckpoint": true,
  "desktopScreenshot": "...\\DESKTOP.BMP",
  "recoveryRequired": false
}
```

---

## 13. Proposed standalone repository layout

```text
v9x-remote-agent\
  README.md
  PLAN.md
  LICENSE                 decision required
  .gitignore

  include\
    v9xremote\
      protocol.h
      status.h
      version.h

  src\
    guest\
      agent.c
      entry.c
      protocol.c
      socket.c
      execute.c
      files.c
      power.c
      machine_info.c
      screenshot.c
      registry.c
      logging.c
      crc32.c
    common\
      frame.c
      bounds.c
      crc32.c

  tools\
    guest\
      driver_install.c
    host\
      protocol_fixture.c

  scripts\
    build-guest.ps1
    build-host-tests.ps1
    package-guest.ps1
    v9xctl.ps1
    install-guest-agent.ps1
    configure-86box-forward.ps1
    run-driver-cycle.ps1

  packaging\
    win98se\
      INSTALL.BAT
      UNINSTALL.BAT
      README.TXT
      AGENT.INI

  tests\
    host\
      test_main.c
      test_frames.c
      test_crc32.c
      test_limits.c
      test_exec_payload.c
      test_file_payload.c
      test_reboot_state.c
    integration\
      README.md

  docs\
    protocol.md
    guest-agent.md
    host-cli.md
    86box-networking.md
    velocity9x-integration.md
    recovery.md
    compatibility.md
    decisions\
```

The initial implementation may use fewer source files, but protocol, execution, file transfer, power handling, and screenshot code should not remain permanently merged into one unreviewable source file.

---

## 14. Build and packaging plan

### 14.1 Guest build

`build-guest.ps1` should:

1. locate Open Watcom from `$env:WATCOM` or `C:\WATCOM`;
2. validate required compiler, linker, dumper, headers, and import libraries;
3. compile for 32-bit Windows with warnings as errors;
4. link without the default C runtime;
5. set Windows runtime version 4.0 in the PE image;
6. embed a sanitized build ID;
7. verify MZ and PE signatures;
8. inspect imports with `wdump`;
9. reject Unicode startup APIs and unexpected DLL dependencies;
10. verify the build ID is present in the executable;
11. produce a package directory and hashes.

The source should provide small internal replacements for required runtime operations such as bounded string length/copy, byte encoding, integer formatting, and memory clearing. Avoid recreating a large general-purpose C runtime.

### 14.2 Host tests

Protocol and bounds logic should be portable C where possible and compiled on the host with both Open Watcom and MSVC. The guest networking/process code will still require 86Box integration testing.

### 14.3 Guest package

Proposed package contents:

```text
V9XAGNT.EXE
INSTALL.BAT
UNINSTALL.BAT
AGENT.INI
README.TXT
SHA256.TXT
```

The first installation can be transferred through the existing 86Box folder-CD path. Once installed and proven, future agent and driver updates can use the TCP channel.

Agent self-update should be a later feature because a running executable cannot safely replace itself and validate restart in one trivial operation. A helper or reboot-time replacement mechanism can be designed after the basic system is reliable.

---

## 15. Testing strategy

### 15.1 Host unit tests

Test all protocol parsing without a VM:

- fragmented headers one byte at a time;
- headers and payloads coalesced into one receive buffer;
- invalid magic;
- unsupported version;
- zero and maximum payloads;
- payload lengths above limit;
- integer overflow around header plus payload sizes;
- unknown message types;
- duplicate request IDs;
- malformed TLVs;
- truncated fields;
- CRC32 known vectors;
- file transfer offset/order checks;
- command-line and path length limits;
- reboot job serialization;
- error-code translation;
- output truncation flags.

### 15.2 Host fake-server tests

Create a modern-host protocol fixture that can emulate the guest. Use it to test `v9xctl.ps1` behavior for:

- normal handshake and command completion;
- stdout/stderr streaming;
- guest error response;
- connection loss during upload;
- CRC mismatch;
- command timeout;
- reboot disconnect/reconnect;
- wrong boot counter;
- wrong resume job ID;
- unsupported capabilities;
- malformed server frames.

### 15.3 Windows 98 guest tests

Minimum guest test matrix:

1. install and uninstall agent;
2. automatic start after cold boot;
3. automatic start after warm reboot;
4. Winsock delayed-start retry;
5. 100 sequential connect/handshake/disconnect cycles;
6. 20 reboot/reconnect cycles;
7. direct execution returning exit code 0;
8. direct execution returning nonzero exit code;
9. shell execution of `VER` and file commands;
10. stdout and stderr capture;
11. output larger than one socket frame;
12. output-limit truncation;
13. child timeout and cancellation;
14. upload/download of zero-byte, small, and multi-megabyte files;
15. interrupted upload preserving the old destination;
16. invalid path and missing file handling;
17. screenshot at standard VGA and Velocity9x modes;
18. reboot resume-token validation;
19. agent behavior when Explorer is not yet ready;
20. continued operation after a child process crashes.

### 15.4 Velocity9x integration tests

After the generic agent passes:

1. upload and run COM1 smoke utility;
2. upload and run VxD lifecycle probe;
3. upload and run Win16 loader probe;
4. run consolidated stage preflight unattended;
5. update already-associated driver files using the repair INF;
6. reboot and require matching TCP plus serial build identities;
7. run quiet GDI test;
8. download numeric result and BMP;
9. repeat across all supported mode/bpp combinations;
10. inject one known-bad candidate into a disposable VM and prove timeout, artifact preservation, and host restore.

### 15.5 Soak testing

Before calling the agent reliable:

- 100 command executions without agent restart;
- 100 file uploads with verified CRC;
- 50 connection losses and reconnects;
- 20 consecutive warm reboot/reconnect cycles;
- 20 consecutive cold start/connect/shutdown cycles;
- 20 consecutive already-installed driver update cycles on disposable images;
- no growth beyond documented log and temporary-file bounds;
- no stale child or `.PART` files after successful runs.

---

## 16. Acceptance gates

### Gate A: protocol core

- Frame parser passes host unit and malformed-input tests.
- All size arithmetic is overflow checked.
- Protocol version and capability negotiation are documented.

### Gate B: guest liveness

- Agent builds with audited Windows 98-compatible imports.
- Agent starts automatically on Windows 98SE.
- Host connects through the 86Box SLiRP forward.
- Twenty reboot/reconnect cycles preserve correct boot and job identity.

### Gate C: remote execution and transfer

- Direct and shell commands return deterministic exit status.
- Stdout and stderr capture does not deadlock.
- Timeout/cancel leaves the agent responsive.
- Transactional upload cannot corrupt an existing destination on interruption.

### Gate D: diagnostic automation

- `V9XSTAGE.EXE` has a true quiet mode.
- `V9XGDI.EXE` has a true quiet/result mode.
- Screenshot capture works at standard VGA and supported Velocity9x modes.
- One command produces a complete result directory.

### Gate E: driver update loop

- An already-associated Velocity9x driver can be remotely updated, rebooted, verified, and tested.
- Serial and TCP build identities match.
- Failure triggers host-side evidence preservation and disposable-image restoration.

### Gate F: first-install helper

- Exact hardware-ID matching is proven.
- The helper performs the equivalent of the intended Windows 98 Have Disk association.
- Unsupported hardware and ambiguous devnode state are rejected.
- Standard-VGA recovery or cold restore is demonstrated.

---

## 17. Implementation milestones

### Milestone 0: repository and compatibility skeleton

- Create standalone repository.
- Add plan, README, license decision placeholder, and source layout.
- Copy only generic protocol/build knowledge, not Velocity9x binaries or external DDK content.
- Build a minimal `V9XAGNT.EXE` that writes its build ID to a file and exits.
- Add import-table audit.

### Milestone 1: ping and information

- Winsock startup/retry.
- Listen/accept loop.
- Handshake, capabilities, `PING`, `INFO`.
- Host PowerShell client.
- Configure SLiRP forwarding.
- Prove reconnect after client disconnect.

### Milestone 2: execution

- Direct and shell execution.
- stdout/stderr streaming.
- exit code, timeout, cancellation.
- host JSON output and stable exit codes.
- guest rolling log.

### Milestone 3: files

- stat/list/mkdir.
- transactional put/get with CRC32.
- push-tree and collect operations.
- cleanup of stale partial transfers.

### Milestone 4: boot lifecycle

- persistent boot counter.
- pending job/resume token.
- controlled reboot/shutdown.
- wait-disconnect/reconnect state machine.
- desktop-ready detection.

### Milestone 5: Velocity9x unattended diagnostics

- true quiet `V9XSTAGE` behavior.
- quiet/result-producing `V9XGDI` behavior.
- upload active package.
- collect agent and serial logs.
- screenshot BMP support.

Changes to Velocity9x should remain in the Velocity9x repository; the standalone agent repository should document the required version/capabilities.

### Milestone 6: installed-driver update automation

- invoke the existing repair INF safely.
- correlate build IDs.
- perform reboot and post-boot validation.
- run the complete mode matrix.

### Milestone 7: host watchdog and disposable VM

- explicit 86Box process launch/identity tracking.
- cold-copy/disposable profile creation.
- serial-capture lifecycle.
- boot timeout.
- failure preservation and restore.
- one-command driver cycle.

### Milestone 8: first-install helper

- Windows 98 SetupX/Configuration Manager research spike.
- read-only enumeration/report mode first.
- quarantined exact-device install mode.
- removal/rollback investigation.
- repeated clean-image validation.

---

## 18. Risks and mitigations

### 18.1 Guest hard lock

**Risk:** A faulty VxD or display DRV can stop scheduling, networking, or all guest execution.

**Mitigation:** COM1 remains independent evidence; host watchdog owns timeout, VM stop, failed-state preservation, and disposable-image restore.

### 18.2 Agent starts before TCP/IP

**Risk:** `RunServices` may launch before Winsock networking is ready.

**Mitigation:** bounded-delay retry loop around `WSAStartup`, socket creation, bind, and listen. Do not exit permanently after an early failure.

### 18.3 Agent destabilized by screenshot/GDI calls

**Risk:** A broken display driver can make screenshot capture hang or crash.

**Mitigation:** screenshot is optional and late in the flow; invoke only on a
confirmed stable desktop. All GDI capture runs in a disposable helper with a
hard timeout, while the network agent has no GDI32 import. During transitions
or suspected wedges, prefer INFO, direct trace-dump execution, and file download.

### 18.4 Child process blocks output pipes

**Risk:** Incorrect stdout/stderr draining deadlocks the child or agent.

**Mitigation:** concurrently drain both streams, enforce output limits, and provide temporary-file fallback.

### 18.5 Win16 process semantics

**Risk:** Win16 programs may share a subsystem and exhibit different termination or output behavior.

**Mitigation:** prefer Win32 wrapper executables such as `V9XSTAGE.EXE` that own and wait for the Win16 helper; test Win16 behavior explicitly.

### 18.6 Incorrect first-install automation

**Risk:** Modern SetupAPI assumptions do not match Windows 98 SetupX behavior, or the wrong display devnode is modified.

**Mitigation:** separate helper, exact PCI ID, report-only mode first, clean disposable image, one install attempt per restored state, and manual workflow retained until proven.

### 18.7 Unauthenticated service exposure

**Risk:** Any reachable client obtains arbitrary guest execution.

**Mitigation:** SLiRP port forward plus host firewall restricted to local traffic; no bridged LAN deployment; conspicuous DEV-ONLY documentation.

### 18.8 Virtual-disk corruption after forced stop

**Risk:** Killing 86Box while the guest writes its disk may corrupt the working image.

**Mitigation:** use disposable copies, attempt controlled shutdown first, preserve golden backup read-only by process, and never promote a failed working image automatically.

### 18.9 Protocol drift

**Risk:** Host and guest are updated independently and become incompatible.

**Mitigation:** handshake version range, capabilities, embedded build ID, documented stable opcodes, and compatibility tests.

### 18.10 Agent self-update

**Risk:** Replacing the executable that owns the control connection can strand the VM.

**Mitigation:** defer self-update until core stability; later use a small updater or reboot-time replacement with rollback.

---

## 19. Open design decisions

These should be resolved through small prototypes rather than assumption:

1. Whether `RunServices` alone is reliable on the exact Windows 98SE guest or whether a Startup-folder fallback is also needed.
2. Whether anonymous pipes reliably capture both Win32 and relevant Win16-wrapper output under Windows 98SE.
3. Whether the agent should use Winsock 2.2 exclusively or negotiate/fallback to Winsock 1.1 for possible Windows 95 support.
4. Whether the host CLI remains PowerShell-only or gains a native executable after protocol stabilization.
5. Whether screenshot capture belongs in the main agent binary or a separate helper to reduce the core agent's GDI dependency.
6. How to detect desktop readiness reliably on Windows 98 without blocking early command execution.
7. Which Windows 98 DDK SetupX/Configuration Manager functions correctly reproduce display-device Have Disk association.
8. Whether agent startup should emit one COM1 marker or leave COM1 entirely to the driver under test.
9. Whether a private PCap/KM-TEST network is worth the added host setup after the SLiRP proof.
10. Whether routine test jobs should preserve every failed VHD or preserve only failures explicitly requested by policy because disk images are relatively large.
11. What license to apply to the future standalone repository.
12. Whether Windows 95 OSR2 and Windows Me compatibility should influence protocol v1 or remain later compatibility work.

---

## 20. Recommended initial decisions

Unless testing disproves them, begin with these choices:

- Guest binary: `V9XAGNT.EXE`.
- Guest install root: `C:\V9XREMOTE`.
- Host client: PowerShell.
- Default endpoint: `127.0.0.1:9869`.
- Guest listener: TCP port 9869.
- Network: existing NE2000 PnP plus 86Box SLiRP port forward.
- Protocol: 24-byte little-endian framed header with binary/TLV payloads.
- Text: ASCII-only protocol v1.
- Concurrency: one client, one active execution, one worker thread.
- Command shell: `COMMAND.COM`, never `cmd.exe`.
- Transfer integrity: CRC32 plus transactional `.PART` rename.
- Guest build: runtime-free Open Watcom, audited imports.
- Boot persistence: `RunServices`, with startup retry.
- Diagnostics: TCP control plus existing COM1 data plane.
- Recovery: host watchdog plus disposable VHD/profile.
- First driver association: manual until dedicated SetupX helper is proven.
- Repeated driver update: existing Velocity9x repair INF path.
- Screenshots: BMP, optional and invoked late.
- Automation results: explicit JSON plus stable process exit codes.

---

## 21. First practical end-to-end target

The first end-to-end success should be intentionally narrower than full automatic driver installation:

1. Install `V9XAGNT.EXE` once from the existing folder CD.
2. Boot Windows 98 and connect from `v9xctl.ps1` through SLiRP forwarding.
3. Upload the current Velocity9x active package into a new job directory.
4. Run a truly quiet `V9XSTAGE.EXE` and receive exit code 0.
5. Download its result/log data.
6. Reboot Windows through the agent.
7. Reconnect and prove boot-counter plus resume-token advancement.
8. Capture guest information and a BMP screenshot.
9. Shut down Windows through the agent.
10. Repeat the cycle twenty times without manual guest interaction.

Once this is stable, automate the already-installed-driver repair path. Only then begin the more hazardous first-device-association helper.

---

## 22. Definition of project success

The project is successful when an AI coding agent can make a Velocity9x source change, invoke one bounded host command, and receive a trustworthy result containing:

- whether the host build passed;
- whether the guest agent was reached;
- whether artifacts transferred intact;
- whether driver preflight passed;
- whether install/update staging succeeded;
- whether a real reboot occurred;
- which driver build reached mini-VDD and display-enable checkpoints;
- whether the requested mode became active;
- whether the GDI/pixel test passed;
- a screenshot when the desktop was available;
- the last serial checkpoint when it was not;
- whether host-level recovery was required;
- paths to all retained evidence.

The result must be explicit enough that the AI does not need to infer PASS from a message box, manually inspect the VM, or guess whether a TCP disconnect meant reboot, crash, or stale state.

---

## 23. References

Primary project context:

- Velocity9x repository: `<velocity9x-repo>`
- Velocity9x VM environment: `<velocity9x-repo>\docs\vm-environment.md`
- Velocity9x serial logging: `<velocity9x-repo>\docs\specifications\logging-protocol.md`
- Velocity9x active package: `<velocity9x-repo>\packaging\win98se`

External technical references to retain:

- 86Box networking and SLiRP port forwarding: <https://86box.readthedocs.io/en/latest/hardware/network.html>
- 86Box COM/LPT passthrough: <https://86box.readthedocs.io/en/v5.0/settings/ports.html>
- Microsoft `WSAStartup` documentation and Windows 98 Winsock 2.2 support: <https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsastartup>
- Microsoft INF installation overview: <https://learn.microsoft.com/en-us/windows/win32/setupapi/installing-from-an-inf-file>

Windows 98-specific device-install behavior must be validated against the installed Windows 98 DDK and the actual guest. Current Microsoft documentation often describes later operating systems and must not be treated as definitive evidence for SetupX behavior on Windows 98.

---

## 24. Immediate next action when implementation begins

Create the standalone repository, copy this plan into it, and implement only Milestone 0 and the handshake portion of Milestone 1. The first guest test should do nothing more dangerous than:

```text
start automatically -> listen -> HELLO -> PING -> INFO -> disconnect -> accept again
```

Do not combine initial networking proof with driver installation. Establish a dependable control channel first, then add process execution and file transfer behind host tests and bounded protocol parsing.
