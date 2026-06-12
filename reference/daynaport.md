# DaynaPort SCSI/Link — Protocol Reference

Findings for writing a Linux kernel network driver for the DaynaPort SCSI/Link
Ethernet adapter, as emulated by **BlueSCSI V2**, **ZuluSCSI**, and **PiSCSI**
(formerly RaSCSI Reloaded).

The device is a real-world SCSI Ethernet bridge from the early 1990s (Dayna
Communications; also rebadged as the Farallon EtherMac). Because there was never
a Linux driver, we are writing one from scratch against the documented command
set. Everything below is the *device-side* protocol the initiator (our driver)
must speak — it is defined by the hardware, not by any particular emulator, so it
applies equally to a real DaynaPort, BlueSCSI, ZuluSCSI, and PiSCSI.

## Sources

- **SLINKCMD.TXT** (Dayna's own command reference):
  `reference/SLINKCMD.TXT`
- **BlueSCSI V2 firmware** — *our primary target hardware*:
  https://github.com/BlueSCSI/BlueSCSI-v2
  (`lib/SCSI2SD/src/firmware/network.{c,h}`, `inquiry.c`, `src/BlueSCSI_config.h`)
- **ZuluSCSI firmware** — BlueSCSI V2 is a fork of ZuluSCSI; both carry the same
  SCSI2SD DaynaPORT code, so ZuluSCSI is wire-compatible and behaves identically:
  https://github.com/ZuluSCSI/ZuluSCSI-firmware
- **PiSCSI emulation** — https://github.com/PiSCSI/piscsi
  (`cpp/devices/scsi_daynaport.{h,cpp}`, `cpp/devices/ctapdriver.cpp`,
  `cpp/test/scsi_daynaport_test.cpp`)
- **PiSCSI wiki**: https://github.com/PiSCSI/piscsi/wiki/Dayna-Port-SCSI-Link
- David Kuder's Tiny SCSI Emulator: https://hackaday.io/project/18974-tiny-scsi-emulator

> **One protocol, several emulators.** BlueSCSI, ZuluSCSI, and PiSCSI all implement
> the same SLINKCMD.TXT command set and are wire-compatible. BlueSCSI and ZuluSCSI
> share the SCSI2SD firmware lineage (BlueSCSI V2 is a fork of ZuluSCSI), so they
> behave identically; PiSCSI differs in a few details (firmware string, short-frame
> padding, multi-packet batching, MAC source). Where they diverge, **BlueSCSI
> behaviour is authoritative for this project** and is called out inline with a
> `BlueSCSI:` / `PiSCSI:` tag, plus a dedicated section §10. The BlueSCSI V2 DaynaPORT also bridges to **Wi-Fi**, not wired Ethernet —
> see §10 for the operational implications (you must join a network first).

---

## 1. Device identity & SCSI characteristics

| Property | Value |
|----------|-------|
| SCSI device type | **0x03 — Processor device** (NOT a disk/`0x00`) |
| SCSI compliance level | SCSI-2 |
| SCSI ID | **No default — user-assigned.** On BlueSCSI the DaynaPORT image must be given an explicit ID in `bluescsi.ini`; there is no canonical ID to assume. |
| LUN | 0 |
| Block / transfer model | packet-at-a-time via READ(6)/WRITE(6) — NOT block storage |

Because it enumerates as a **Processor device (type 3)**, the standard Linux disk
upper-layer (`sd`) will *not* claim it. Our driver must recognise it by INQUIRY
type 0x03 + vendor/product strings (below) and bind a `net_device` to it.

**Discovery — the driver must find the device, not assume an ID.** Since the ID is
user-configured and may be anything (or absent), the driver must **not** hardcode a
target. Instead it binds via the SCSI mid-layer's normal scan: the mid-layer
INQUIRYs every target/LUN on the bus, and our upper-level driver's detect/attach
hook claims any device whose INQUIRY matches `type == 0x03 (Processor)` + vendor
`"Dayna"` + product `"SCSI/Link"`. This naturally answers both *whether* a DaynaPort
is present and *where* it sits — and supports more than one if the user has several.
This is exactly the model the NetBSD `dse` driver uses (`scsipi_inqmatch` against a
`dse_patterns[]` table). On Linux 2.0.35 the equivalent is a
SCSI high-level driver with a `detect` callback that walks the detected
`Scsi_Device` list and matches `->type`/`->vendor`/`->model`.

---

## 2. INQUIRY (opcode 0x12)

Standard SCSI INQUIRY. The DaynaPort returns a response the Mac driver expects to
be **37 bytes** long (note: `additional length` = 0x20, so total = 5 + 0x20 = 37).
PiSCSI deliberately bumps the additional-length byte by 1 and appends one
vendor-specific pad byte to hit exactly 37.

Response layout:

| Byte | Value | Meaning |
|------|-------|---------|
| 0 | `0x03` | Peripheral device type = Processor |
| 1 | `0x00` | RMB=0 (not removable) |
| 2 | `0x02` | SCSI-2 |
| 3 | `0x02` | Response data format (SCSI-2) |
| 4 | `0x20` | Additional length (32) → total length 37 |
| 5–7 | `0x00` | reserved |
| 8–15 | `"Dayna   "` | Vendor ID (8 bytes, space-padded) |
| 16–31 | `"SCSI/Link       "` | Product ID (16 bytes, space-padded) |
| 32–35 | `"1.4a"` / `"2.0f"` | Revision (4 bytes) — see below |
| 36 | `0x00` | vendor-specific pad (PiSCSI only) |

**Identification string for the driver:** vendor `"Dayna"`, product `"SCSI/Link"`.
Match on **type==0x03 + vendor "Dayna" + product "SCSI/Link"**. Do **not** be
strict about the revision string, the ANSI version byte, or the exact additional
length — these differ between emulators:

- **BlueSCSI:** revision `"2.0f"`; reports ANSI version byte (byte 2) = `0x01`;
  additional length `0x1f` (no +1/pad). (`src/BlueSCSI_config.h`:
  `DRIVEINFO_NETWORK {"Dayna","SCSI/Link","2.0f",""}`.)
- **PiSCSI:** revision `"1.4a"`; ANSI version byte `0x02` (SCSI-2); additional
  length bumped to `0x20` + 1 pad byte to total exactly 37 bytes (the Mac driver
  wants 37).

Firmware versions in the wild: **1.4a** and **2.0f**. Some commands behave
differently (see Set Interface Mode §4.5). BlueSCSI reports as **2.0f**, which
means broadcast RX is always on and Set Interface Mode is a no-op.

---

## 3. Opcode summary

The DaynaPort overloads several standard SCSI-6 opcodes with custom semantics.
All commands use 6-byte CDBs.

| Opcode | Name | Direction | Purpose |
|--------|------|-----------|---------|
| `0x00` | TEST UNIT READY | none | Always returns GOOD |
| `0x03` | REQUEST SENSE | in | Standard sense data |
| `0x08` | READ(6) | in | Read one received Ethernet packet |
| `0x09` | RETRIEVE STATS | in | MAC address + 3 error counters (18 bytes) |
| `0x0A` | WRITE(6) | out | Transmit one Ethernet packet |
| `0x0C` | SET INTERFACE MODE | out/none | Broadcast mode (BlueSCSI: ignored) |
| `0x0D` | SET MULTICAST ADDRESS | out | Program multicast filter |
| `0x0E` | ENABLE/DISABLE INTERFACE | none | Bring link up/down |
| `0x12` | INQUIRY | in | Standard inquiry (see §2) |
| `0x1A` | MODE SENSE(6) | — | BlueSCSI: accepted as no-op |
| `0x40` | SET MAC ADDRESS | out | BlueSCSI: standalone opcode, ignored |
| `0x80` | SET MODE | none | BlueSCSI: standalone opcode, ignored |
| `0x1C` | WI-FI CONTROL | in/out | **BlueSCSI extension** (sub-cmd in CDB[1]) — see §10 |

> **Opcode-mapping discrepancy (0x0C vs 0x40/0x80):** SLINKCMD/PiSCSI fold "Set
> MAC" (`0x40`) and "Set Mode" (`0x80`) into command `0x0C` as values of the
> *control byte* CDB[5]. BlueSCSI instead implements `0x40` and `0x80` as
> standalone opcodes (and ignores `0x0C` entirely). Our driver never needs to set
> the MAC (we read it via `0x09`), so this divergence is harmless — just don't
> rely on `0x0C` doing anything on BlueSCSI.

The control byte (CDB byte 5) is frequently used as a *sub-command / format*
selector rather than a normal SCSI control byte — pay close attention per command.

---

## 4. Command details

### 4.1 TEST UNIT READY — `0x00`

```
00 00 00 00 00 00
```
Always succeeds (GOOD status). Use as a liveness probe.

### 4.2 READ(6) — `0x08`  (receive a packet)

```
CDB:  08 00 00 LL LL XX
            \____/  \_ control/flag byte: must be 0xC0 or 0x80
              \_ LLLL = max transfer length (big-endian), byte 3 hi, byte 4 lo
```

- `LLLL` is a **16-bit big-endian** max transfer length: `CDB[3]<<8 | CDB[4]`.
- **Control byte (CDB[5]) — the `0x40` bit selects single- vs multi-packet:**
  - `0x80` (bit 6 *clear*): **polled mode** (old SCSI Manager / virtual memory
    present). Device returns **one packet per READ(6)** to minimise SCSI bus hold
    time so the VM pager can use the bus between transactions.
  - `0xC0` (bit 6 *set*): **blind mode** (new SCSI Manager, no VM). Device may
    pack **multiple packets into a single READ(6)** response (see batching below).
  - **PiSCSI:** rejects any control byte other than `0xC0`/`0x80` with CHECK
    CONDITION / ILLEGAL REQUEST. **BlueSCSI:** only tests bit `0x40`; other values
    don't error. For safety our driver should always send `0xC0` or `0x80`.
- **Special case — `LLLL == 1`:** startup "root sector" probe. **BlueSCSI** returns
  CHECK CONDITION; **PiSCSI** returns a zero-length/status response. Our driver
  never issues this.

**Multi-packet batching (BlueSCSI, control byte `0xC0`):** a single READ(6)
DATA-IN can contain several back-to-back `[6-byte header][frame]` units. Each
non-final header carries flag byte `0x10` ("more in this transfer"); the final
unit carries flag `0x00`. The device stops batching when: the queue empties, the
next packet wouldn't fit in the host's requested `LLLL`, or it has already batched
~2×(max packet) to avoid hogging the bus. **The driver's RX parser must therefore
loop through the returned buffer, consuming `header → length → frame` repeatedly,
not assume exactly one frame per READ.** In polled mode (`0x80`) you always get at
most one frame.

**Returned data — a 6-byte header followed by the frame:**

```
 LL LL  NN NN NN NN  <ethernet frame bytes...>  CC CC CC CC
 \___/  \_________/                             \_________/
  len     flags                                    CRC (FCS)
```

| Field | Bytes | Endianness | Meaning |
|-------|-------|-----------|---------|
| length | 2 | **big-endian** | size of the frame *including* its 4 trailing CRC bytes, but EXCLUDING the length field and the flag field |
| flags | 4 | (see below) | packet-availability flag |
| frame | length−4 | — | the Ethernet frame (dest MAC first) |
| CRC | 4 | — | Ethernet FCS appended by the device |

**Flag field values:**

| Value | Meaning |
|-------|---------|
| `0x00000000` | this is the last packet (no more queued) |
| `0x00000010` | more packets currently available in device memory (code uses `0x10`) |
| `0xFFFFFFFF` | a packet was dropped; in this case the length field is reported as `0x4000`. Persists until a disable/enable cycle. |

**No-data response:** when there is nothing to receive, the device returns just
the 6-byte header with `length = 0` and `flags = 0x00000000` (i.e. 6 bytes total).

> **How the real NetBSD `dse` driver parses this (authoritative):** it **ignores
> the 4-byte flag field entirely** and walks the returned buffer purely by the
> 2-byte length: read len → skip the 6-byte header → if `len == 0` stop →
> else consume `len` bytes (frame incl. FCS) → repeat until the buffer is
> exhausted or a zero-length record terminates it. The `0x10`/`0xFFFFFFFF` flags
> are informational only. Our Linux RX parser should do the same length-walk +
> zero-terminator; don't depend on the flag bits.

> **BlueSCSI reality — `0x10` is intra-transfer only, NOT a "ring backed up"
> signal.** In BlueSCSI's `network.c` the flag is written as `done ? 0x00 : 0x10`,
> where `done` becomes true on the *last record written in this transfer*. So
> `0x10` means "another `[header][frame]` follows **in this same DATA-IN**," and
> the final record is **always** `0x00` — even when BlueSCSI stopped early because
> it hit the `LLLL` size cap or its ~2×(max packet) batch cap with packets **still
> queued** in the 20-deep ring. The host therefore can *never* learn from the flag
> that the ring is non-empty, so a flag-driven "re-poll immediately" fast-drain is
> effectively dead on this hardware (it always reads `0x00` at end-of-transfer).
> The flag table's "available in device memory" wording is the spec/PiSCSI intent,
> not what BlueSCSI's code does. This is one reason RX throughput can't be pushed
> from the initiator side.

**Important framing quirks (short-frame padding differs by emulator):**

- **BlueSCSI:** pads runt frames to a **60-byte payload, then appends a valid
  4-byte FCS → 64 bytes** (the real Ethernet minimum). Because the CRC is computed
  *after* padding, the FCS is **correct**. (`scsiNetworkEnqueue` in `network.c`.)
- **PiSCSI:** reports any frame `< 128` bytes with `length = 128` and a
  **deliberately broken FCS** (pad happens after CRC). No known driver checks the
  FCS, so this is tolerated. (Originated as 64-byte min for the Atari driver;
  NetBSD needs ≥128.)
- **Takeaway:** our driver should not depend on a valid FCS and should size its RX
  buffer for up to 128-byte minimum frames. Either way, strip/ignore the trailing
  4 FCS bytes.
- The **CRC/FCS is appended by the device**, standard Ethernet CRC-32 (reflected,
  poly `0xEDB88320`, init `0xFFFFFFFF`, final XOR `0xFFFFFFFF`). BlueSCSI uses a
  table-driven implementation (`crc32_tab` in `network.c`); same result. Linux's
  net stack strips the FCS on RX, so drop the last 4 bytes (or trust length−4)
  before `netif_rx()`.
- `ETH_FRAME_LEN = 1514`; BlueSCSI buffers per packet are `NETWORK_PACKET_MAX_SIZE
  = 1520`, and the max single SCSI transfer unit is `DAYNAPORT_SCSI_PACKET_MAX =
  1524` (1514 frame + 4 FCS + 6-byte header). Inbound ring is 20 packets deep
  (`NETWORK_PACKET_QUEUE_SIZE`); the device drops frames when the ring is full.

**Read-polling behaviour:** there is no interrupt line. The device is polled.
PiSCSI internally retries up to `MAX_READ_RETRIES = 50` reads from its tap before
giving up and returning "no data". Our driver will need a poll loop / timer (the
classic approach for SCSI-Ethernet on these old kernels) — see §6.

> **Send-delay quirk:** The Mac driver issues the READ as effectively *two* reads:
> first the 6-byte size/flags header, then the body. PiSCSI sets a "send delay"
> equal to the 6-byte header (`DAYNAPORT_READ_HEADER_SZ = 6`) — i.e. the device
> inserts a brief pause after delivering the header. Our initiator just reads the
> whole response; the SCSI layer handles phase changes. Noted in case timing
> matters on real hardware.

### 4.3 RETRIEVE STATS — `0x09`

```
09 00 00 00 12 00      (0x12 = 18 = number of bytes requested)
```
Returns **18 bytes**:

| Bytes | Meaning |
|-------|---------|
| 0–5 | current hardware MAC address (6 bytes) |
| 6–9 | counter #1 — frame alignment errors (4 bytes, **little-endian**) |
| 10–13 | counter #2 — CRC errors (little-endian) |
| 14–17 | counter #3 — frames lost (little-endian) |

Counters are typically zero. This is how the driver learns the MAC address to put
in `dev->dev_addr`. **Read the MAC from this command at init, don't assume.**

- **BlueSCSI:** returns the board's real Wi-Fi MAC (`boardCfg.wifiMACAddress`),
  unique per board; the 3 counters are always returned as zero.
- **PiSCSI:** hard-codes `00:80:19:10:98:e3` (a Dayna OUI) unless overridden.

### 4.4 WRITE(6) — `0x0A`  (transmit a packet)

```
CDB:  0a 00 00 LL LL XX
            \____/  \_ data-format byte (CDB[5]): 0x00 or 0x80
              \_ LLLL = length (big-endian)
```

Two output formats selected by CDB[5]:

- **Format `0x00`:** `LLLL` = packet length, and the DATA-OUT payload is the raw
  Ethernet frame image, exactly `LLLL` bytes.
- **Format `0x80`:** `LLLL` = packet length + 8. The DATA-OUT payload is:
  ```
  PP PP 00 00  <frame bytes...>  00 00 00 00
  \___/                          \_________/
  real len (big-endian)          trailing pad
  ```
  i.e. a 4-byte header (`PPPP` = real length big-endian, then `00 00`), the frame,
  then a 4-byte trailing pad. The device transmits the `PPPP`-byte frame.

Either format works; **`0x00` is the simpler choice for our driver** — just put
the frame length in the CDB and send the raw frame as DATA-OUT. We do **not**
append an FCS on TX; the device/network handles it.

**BlueSCSI multi-packet write:** in format `0x80`, BlueSCSI runs a *loop*: it reads
a 4-byte sub-header (`PPPP 00`), and if `PPPP != 0` reads and transmits that
packet, repeating until a **zero-length sub-header** terminates the batch. So the
PiSCSI "length+8 with trailing `00 00 00 00`" layout is just the single-packet
case (the trailing zeros are the terminating sub-header). Format `0x00` always
sends exactly one packet (`len = LLLL`) and returns. Frames larger than
`NETWORK_PACKET_MAX_SIZE` (1520) are clamped. **For our driver, format `0x00`,
one frame per WRITE(6), is simplest and fully supported.**

`WRITE6` allocates a large buffer on the device side (PiSCSI uses 16 MB) and sets
block count appropriately; from the initiator side it's a single SCSI WRITE(6).

### 4.5 SET INTERFACE MODE / SET MAC ADDRESS — `0x0C`

CDB[5] selects the operation:

**Set Interface Mode** — `CDB[5] = 0x80`:
```
0c 00 00 00 FF 80      (FF = 0x04 or 0x08)
```
- `FF = 0x04`: enable receiving broadcast messages.
- `FF = 0x08`: function unknown.
- No data transferred.
- Accepted by both firmware 1.4a and 2.0f, but on **2.0f it is a no-op** (always
  receives broadcast). On 1.4a, once set, broadcast stays on until interface
  disabled.
- PiSCSI treats `0x80` as not-implemented / no-op and just returns GOOD status.

**Set MAC Address** — `CDB[5] = 0x40`:
```
0c 00 00 00 FF 40
```
- DATA-OUT phase, 6 bytes = the new MAC address. Overrides the built-in MAC.
- Intended for debug/test. Disabling the interface resets MAC to built-in.

Any other CDB[5] → CHECK CONDITION / ILLEGAL REQUEST (invalid command op code).

> **BlueSCSI:** ignores `0x0C` entirely (no-op, no data phase) — it reports as
> firmware 2.0f, so broadcast RX is always on. It implements MAC-setting as the
> standalone opcode `0x40` (also ignored). Our driver can simply **skip `0x0C`**.

### 4.6 SET MULTICAST ADDRESS — `0x0D`

```
0d 00 00 00 NN XX      (NN = CDB[4] = length of multicast data)
```
- Length is taken from `CDB[4]`. If 0 → ILLEGAL REQUEST (invalid field in CDB).
- DATA-OUT phase delivers the multicast address table.
- PiSCSI accepts and currently ignores the contents (no real multicast filtering;
  RX filtering is effectively promiscuous in the emulator). Safe to program from
  the driver; behaviour on real hardware is the documented multicast filter.

### 4.7 ENABLE / DISABLE INTERFACE — `0x0E`

```
0e 00 00 00 00 XX      (XX = 0x80 enable, 0x00 disable)
```
- `CDB[5] & 0x80` → **enable** (link up). Otherwise → **disable** (link down).
- No data transferred.
- **CRITICAL TIMING:** after issuing ENABLE, the initiator must **wait ~0.5 s
  before sending any further command** to the device. ✅ Confirmed by the NetBSD
  `dse` driver, which sends `0E .. 80` once at attach and immediately does
  `kpause(hz/2)` = 500 ms before any other command. (Host-side wait.)
- **Enable once, at attach/probe** — not on every interface-up. The NetBSD driver
  issues ENABLE a single time during attach and leaves the adapter enabled;
  `ifconfig up`/`down` only starts/stops the RX poll loop. We can adopt the same
  model (simpler than enable/disable per open/close).
- On enable, the device flushes any pending/queued packets. **BlueSCSI:** also
  zeroes its entire inbound ring buffer on enable, and sets `scsiNetworkEnabled`.
  While disabled, BlueSCSI's `scsiNetworkEnqueue` drops all inbound frames — so
  nothing is buffered until you enable.

### 4.8 REQUEST SENSE — `0x03`

Standard SCSI request sense. Returned after a CHECK CONDITION to retrieve the
sense key / ASC. Sense keys we will encounter:

- `ILLEGAL REQUEST (0x05)` with ASC `invalid field in CDB` — bad READ control
  byte, zero-length multicast, bad WRITE length.
- `ILLEGAL REQUEST (0x05)` with ASC `invalid command operation code` — bad
  Set-Interface sub-command.
- `ABORTED COMMAND (0x0B)` — enable/disable of the underlying link failed.

---

## 5. Frame format & CRC

- Frames are standard Ethernet II: 6-byte dest MAC, 6-byte src MAC, 2-byte
  ethertype, payload. Max `ETH_FRAME_LEN = 1514` (no FCS).
- **On RX (READ 0x08):** device appends a 4-byte FCS (CRC-32) after the frame, and
  the reported length *includes* it. Linux normally strips FCS — slice off the
  last 4 bytes before `netif_rx()`, or trust the length minus 4.
- **On TX (WRITE 0x0A):** send the bare frame with no FCS; do not pad (device pads
  short frames itself).
- CRC-32 spec (if we ever need to compute/verify): reflected, polynomial
  `0xEDB88320`, initial value `0xFFFFFFFF`, final XOR `0xFFFFFFFF`. Reference
  implementations: BlueSCSI/ZuluSCSI `lib/SCSI2SD/src/firmware/network.c` (`crc32`,
  table-driven) and PiSCSI `cpp/devices/ctapdriver.cpp` (`CTapDriver::Crc32`,
  bit-wise). We almost certainly never need to compute it ourselves.

---

## 6. Initialization & operation sequence

Recommended driver lifecycle, derived from the command semantics:

**Probe / attach (driver load):**
1. INQUIRY (`0x12`) → verify type 0x03 + vendor "Dayna" / product "SCSI/Link".
2. TEST UNIT READY (`0x00`) → confirm responsive.
3. RETRIEVE STATS (`0x09`, 18 bytes) → read the MAC address into `dev->dev_addr`.
4. Register the `net_device`.

**Interface up (`dev->open`):**
1. ENABLE INTERFACE (`0x0E`, CDB[5]=0x80).
2. **Sleep ~0.5 s** before the next command to the device.
3. (Optional / PiSCSI-only) SET INTERFACE MODE (`0x0C`/0x80, FF=0x04) to allow
   broadcast. **Skip on BlueSCSI** (no-op; 2.0f already receives broadcast).
4. (Optional) SET MULTICAST (`0x0D`) per `dev->mc_list`.
5. Start the RX poll mechanism (timer or kernel thread): periodically issue
   READ(6) with control byte `0xC0` (multi-packet) or `0x80` (one at a time).
   **Parse the returned buffer as a sequence of `[2-byte len][4-byte flags]
   [frame]` units**, advancing by `6 + len` each time; continue while the flag
   byte is `0x10`, stop at flag `0x00` or `len == 0`. Hand each frame to
   `netif_rx()` after stripping the 4-byte FCS.

**Transmit (`dev->hard_start_xmit`):**
- WRITE(6) (`0x0A`) format `0x00`: CDB length = frame length, DATA-OUT = raw frame.

**Interface down (`dev->stop`):**
- Stop the poll timer, then DISABLE INTERFACE (`0x0E`, CDB[5]=0x00).

**Statistics (`dev->get_stats`):**
- RETRIEVE STATS (`0x09`) → map the 3 little-endian counters to
  `rx_frame_errors` (alignment), `rx_crc_errors`, `rx_missed_errors` (frames lost).

---

## 7. Key design constraints for the Linux 2.0.35 driver

- **It is a hybrid driver:** a SCSI initiator-side consumer that presents a
  `net_device`. It must issue *arbitrary/vendor* SCSI CDBs (0x08/0x09/0x0A/0x0C/
  0x0D/0x0E), not just block reads — so it sits above the SCSI mid-layer and
  submits commands directly (à la `sg`/`st`), not via the `sd` block path.
- **No interrupts:** the SCSI target gives no async RX notification. RX is by
  polling. Choose a kernel timer or a low-priority poll; balance latency vs. SCSI
  bus load. (Real Mac driver polled; 50-retry burst per poll is the PiSCSI model.)
- **Honour the 0.5 s post-ENABLE delay** or early commands may be lost.
- **Processor device type (0x03)** means we won't collide with `sd`; we bind by
  type+inquiry strings.
- **MTU:** 1500; max frame 1514 (no FCS on our side).
- Be conservative with transfer lengths — keep READ max length within the frame +
  header size (`1514 + 6 + 4`).

---

## 8. Resolved against BlueSCSI source + open questions

**Resolved by reading the BlueSCSI/ZuluSCSI `lib/SCSI2SD/src/firmware/network.c`:**

- ✅ BlueSCSI returns a **real, unique MAC** (the board's Wi-Fi MAC) via RETRIEVE
  STATS; counters are zero.
- ✅ BlueSCSI only tests bit `0x40` of the READ control byte; it does **not**
  reject other values (PiSCSI does). Send `0xC0`/`0x80` regardless.
- ✅ BlueSCSI pads runt frames to **64 bytes (60 + valid FCS)**, not 128.
- ✅ BlueSCSI reports firmware **`2.0f`** → broadcast always on, `0x0C` is a no-op.
- ✅ BlueSCSI **ignores** multicast contents (`0x0D`) — effectively promiscuous.
- ✅ BlueSCSI supports **multi-packet** READ (`0xC0`) and WRITE (`0x80`) batching.

**Still worth confirming on real hardware / on the wire:**

- [ ] Real post-ENABLE settle time (the 0.5 s is host-driver convention; BlueSCSI
      itself doesn't block, but real timing on the bus may matter).
- [ ] Whether BlueSCSI ever sets the `0xFFFFFFFF` / length-`0x4000` dropped-packet
      flag (the source uses simple ring-drop and reports flag `0x10`/`0x00` only —
      so we likely never see `0xFFFFFFFF`, but verify).
- [ ] Whether the Wi-Fi link must be joined before ENABLE returns useful RX (see §10).

---

## 9. Quick CDB cheat-sheet

```
TEST UNIT READY     00 00 00 00 00 00
REQUEST SENSE       03 00 00 00 LL 00
READ packet         08 00 00 HH LL C0      ; HHLL = 16-bit max len; C0=multi, 80=single
RETRIEVE STATS      09 00 00 00 12 00      ; returns 18 bytes (MAC + 3 counters)
WRITE packet        0a 00 00 HH LL 00      ; HHLL = frame len, raw frame in DATA-OUT
SET IFACE MODE      0c 00 00 00 04 80      ; allow broadcast (PiSCSI); BlueSCSI ignores
SET MULTICAST       0d 00 00 00 NN 00      ; NN = length of mcast data in DATA-OUT
ENABLE INTERFACE    0e 00 00 00 00 80      ; then wait ~0.5s
DISABLE INTERFACE   0e 00 00 00 00 00
INQUIRY             12 00 00 00 fc 00      ; allocation length (>= response size)
SET MAC (BlueSCSI)  40 00 00 00 00 00      ; standalone opcode, 6 bytes DATA-OUT, ignored
```

---

## 10. BlueSCSI V2 specifics (our target hardware)

The BlueSCSI V2 "DaynaPORT" is a **Pico-W-based Wi-Fi** bridge that presents the
DaynaPort SCSI/Link protocol above to the host, but the actual network egress is
**802.11 Wi-Fi**, not wired Ethernet. Implications:

- **The board must be joined to a Wi-Fi network** for traffic to flow. This is
  normally configured in `bluescsi.ini` on the SD card (SSID/key + the device line
  declaring the DaynaPORT image and SCSI ID). There is no DHCP/IP on the BlueSCSI
  itself — it's an L2 bridge; the *Linux host* owns the IP stack over our driver.
- BlueSCSI builds the network feature only when compiled with `BLUESCSI_NETWORK`;
  device config type is `S2S_CFG_NETWORK`, inquiry type `0x03` (processor).

**Wi-Fi control extension — opcode `0x1C`** (`SCSI_NETWORK_WIFI_CMD`, sub-command
in CDB[1]). Not part of the DaynaPort spec; BlueSCSI-only. Our basic Ethernet
driver does **not** need these, but they let a host driver manage Wi-Fi at runtime:

| CDB[1] | Name | Meaning |
|--------|------|---------|
| `0x01` | SCAN | start a Wi-Fi scan (DATA-IN: 1 status byte) |
| `0x02` | COMPLETE | poll scan completion (DATA-IN: 1 byte) |
| `0x03` | SCAN_RESULTS | return network list (2-byte len + entries) |
| `0x04` | INFO | current SSID/BSSID/RSSI/channel |
| `0x05` | JOIN | join SSID/key (DATA-OUT: `wifi_join_request`) |

Scan-result / join structures: `struct wifi_network_entry` (64-byte ssid, 6-byte
bssid, rssi, channel, flags) and `struct wifi_join_request` (64-byte ssid, 64-byte
key, channel) — see `network.h`. Same opcode is shared with the Amiga Wi-Fi
variant.

**Constants that bound our driver (from `network.h`):**

| Constant | Value | Use |
|----------|-------|-----|
| `NETWORK_PACKET_MAX_SIZE` | 1520 | max bytes per queued packet |
| `DAYNAPORT_SCSI_PACKET_MAX` | 1524 | max single SCSI packet (frame+FCS+6 hdr) |
| `NETWORK_PACKET_QUEUE_SIZE` | 20 | inbound ring depth (frames dropped when full) |
| short-frame pad target | 60 (+4 FCS = 64) | runt padding |

**Practical takeaways for the Linux driver:**

- Treat it as a plain DaynaPort: INQUIRY-match `Dayna`/`SCSI/Link`, type 0x03;
  use READ `0x08`/WRITE `0x0A`/STATS `0x09`/ENABLE `0x0E`. Skip `0x0C`.
- Parse multi-packet READ responses (don't assume one frame per READ).
- Poll briskly enough that the 20-deep ring doesn't overflow under load, but not so
  hard that you saturate the SCSI bus — a timer at a few hundred Hz, draining the
  ring per poll, is the model BlueSCSI expects.
- Wi-Fi association is a *board configuration* concern (`bluescsi.ini`), not
  something our v1 driver must manage over SCSI.
