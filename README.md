# QEMU usb-storage CSW deadlock — minimal bare-metal reproducer

## What this is

A ~700-line freestanding C program (plus a 100-line boot sector) that boots
as a USB stick under QEMU, takes over the emulated EHCI controller from
SeaBIOS, and issues SCSI READ(10) commands to QEMU's `usb-storage` device
through hand-built queue heads (QHs) and queue transfer descriptors (qTDs).

It demonstrates that **QEMU's `usb-storage` device model never returns the
CSW** (command status wrapper) when a bulk-IN data phase that fits into a
*single* data qTD has its CSW qTD chained behind it in the *same*
asynchronous queue run. The CSW qTD stays active forever; only a guest
timeout can end it.

## Build and run

Requirements: `gcc` (i386 backend), binutils (`elf_i386`), `nasm`,
`qemu-system-x86_64`.

```
make        # builds poc.img (boot sector + payload, padded to 1 MiB)
make run    # boots poc.img as a USB stick on an emulated EHCI controller
```

No guest OS, no libraries, no BIOS disk services after boot: all USB I/O
goes through the EHCI driver in `ehci_msc_bug.c`.

## What it does

1. Takes over EHCI (PCI scan, `HCRESET`, builds a 3-QH async ring:
   bulk-OUT EP2 / bulk-IN EP1 / control EP0), resets the port and
   enumerates the `usb-storage` device (`SET_ADDRESS`, `GET_DESCRIPTOR`,
   `SET_CONFIGURATION`) — all with plain chained qTDs.
2. Test [1]: `READ(10)` of **1 block** (512 bytes → **one** data qTD),
   with the 13-byte CSW qTD **chained behind the data qTD** on the bulk-IN
   queue head — one CBW qTD, one data qTD, one CSW qTD, all submitted in a
   single queue run, exactly how a straightforward EHCI driver chains them.
   → **FAIL: the CSW qTD stays `Active` with 13 bytes remaining; no CSW
   ever arrives** (2 s timeout in the PoC).
3. Recovers with a port reset + re-enumeration, then test [2]: `READ(10)`
   of **41 blocks** (20992 bytes → **two** data qTDs, 20480 + 512),
   CSW chained the same way. → **PASS, CSW received, status 0.**
   (One qTD moves at most 5 × 4096 = 20480 bytes = 40 blocks, hence 41.)
4. Test [3]: 1 block again, but the data chain **terminates** and the CSW
   qTD is submitted as its **own queue run** after the data qTDs retired
   (the way a normal host stack submits a separate CSW URB).
   → **PASS, CSW received, status 0.**

Expected serial output (abridged):

```
[1] READ(10) 1 block, data in ONE qTD, CSW qTD chained in the same queue run:
  CBW qTD... retired (tbytes 0)
  data qTD 0... retired (tbytes 0)
  CSW qTD... still ACTIVE after 2000 ms (tbytes 13)
    -> FAIL: no CSW, CSW qTD wedged (bug reproduced)
[2] same, ... 41 blocks ..., data split over TWO qTDs, CSW chained:
  ...
  CSW: tag 00000002 residue 00000000 status 00000000
    -> PASS: CSW received
[3] same, 1 block, ... CSW submitted as its own queue run:
  ...
    -> PASS: CSW received
summary: [1] HANG (bug)  [2] pass  [3] pass
bug reproduced on this QEMU
```

Reproduced with QEMU 10.2.2 (Fedora `qemu-10.2.2-1.fc44`).

## Bug analysis

QEMU's EHCI emulation does not execute a qTD chain one qTD at a time: when
the data-phase packet goes async (disk read in flight),
`ehci_fill_queue()` (`hw/usb/hcd-ehci.c`) submits **every** chained qTD
behind it to the device stack up front — including the CSW bulk-IN. The
bulk endpoint is not pipelined, so the CSW packet waits in the USB core's
endpoint queue (`usb_queue_one()`, `hw/usb/core.c`).

When the disk read finishes with a single data qTD, that packet is parked
*inside* the device as `s->packet`. Copying the data into it completes the
SCSI request while `s->packet != NULL`, so `usb_msd_command_complete()`
(`hw/usb/dev-storage.c`) runs its "pending packet" path: it sets
`mode = CSW` and completes the data packet *from inside its own body*.
That completion drains the endpoint queue, which re-submits the queued CSW
packet to the device *right now* — but `s->req` is only cleared to `NULL`
at the tail of `usb_msd_command_complete()`, i.e. *after* the re-submitted
CSW packet has been seen. The device therefore takes the "still in flight"
branch and parks the CSW packet again (`USB_RET_ASYNC`) — with no event
left that could ever complete it, because the SCSI request is already
done. Deadlock: the guest's CSW bulk-IN never completes.

With two data qTDs (or a separately submitted CSW), the SCSI request
completes while no packet is parked in the device (`s->packet == NULL`),
`usb_msd_command_complete()` takes its clean path (`s->req = NULL` before
any CSW packet is processed), and the CSW is delivered normally. Control
transfers and short reads (INQUIRY, READ CAPACITY) are unaffected because
their data is available synchronously — no packet is ever parked.

This only triggers against QEMU's device model: real EHCI hardware
executes the chain in order and issues the CSW bulk-IN only after the data
phase retires, so the race cannot occur there. The defect is the ordering
inside `usb_msd_command_complete()`: it completes the pending data packet
before clearing `s->req`, letting the endpoint-queue drain re-submit the
CSW packet too early.

## Files

- `boot.S` — boot sector: INT 13h extended read of the payload to
  linear 0x20000, A20, switch to 32-bit protected mode, call `cmain`.
- `ehci_msc_bug.c` — the whole PoC: serial/VGA console + mini-printf,
  PCI scan, EHCI takeover and QH/qTD engine, USB enumeration, bulk-only
  transport, and the three tests.
- `linker.ld`, `Makefile` — flat-binary layout at 0x20000; image build
  and `make run` QEMU invocation.
