/*
 * ehci_msc_bug.c - minimal reproducer for the QEMU usb-storage CSW deadlock.
 *
 * Bug: when an EHCI bulk-only mass-storage READ(10) is issued so that
 *   - the data phase fits into ONE data qTD, and
 *   - the CSW bulk-IN qTD is chained behind the data qTD in the same
 *     asynchronous queue run (so QEMU's EHCI emulation pipelines it via
 *     ehci_fill_queue() while the SCSI read is still in flight),
 * then QEMU's usb-storage model never returns the CSW.  The CSW qTD stays
 * active forever; the guest can only time out.  With >= 2 data qTDs, or
 * with the CSW submitted as a separate queue run, the CSW arrives.
 *
 * Runs on bare metal (boot sector + this payload), no BIOS services needed
 * after boot (all USB I/O is done through our own EHCI driver).
 *
 * Build: see Makefile.  Run:  make run
 */

#include <stdint.h>

#define NULL ((void *)0)

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ------------------------------------------------------------------ */
/* console                                                            */
/* ------------------------------------------------------------------ */

#define COM1_BASE 0x3f8
#define VGA_TEXT_BUF 0xb8000

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint8_t vga_col = 0;

static void console_init(void)
{
    /* 115200 8N1 */
    outb(COM1_BASE + 1, 0x00);  /* disable interrupts */
    outb(COM1_BASE + 3, 0x80);   /* DLAB on */
    outb(COM1_BASE + 0, 0x01);   /* divisor 1 */
    outb(COM1_BASE + 1, 0x00);
    outb(COM1_BASE + 3, 0x03);   /* 8 bits, no parity, DLAB off */
    outb(COM1_BASE + 2, 0xc7);   /* FIFO */
    outb(COM1_BASE + 4, 0x03);   /* DTR + RTS */
}

static void putchar_(int c)
{
    uint16_t volatile *vga = (uint16_t volatile *)VGA_TEXT_BUF;

    if (c == '\n') {
        outb(COM1_BASE + 0, '\r');
        while (!(inb(COM1_BASE + 5) & 0x20)) { }
        outb(COM1_BASE + 0, '\n');
        vga_col = 0;
        return;
    }
    while (!(inb(COM1_BASE + 5) & 0x20)) { }
    outb(COM1_BASE + 0, (uint8_t)c);
    if (vga_col < 79) {
        vga[vga_col++] = (uint8_t)c | 0x0f00;
    }
}

static void puts_(const char *s)
{
    while (*s)
        putchar_(*s++);
}

static void putu(unsigned v)
{
    char b[12];
    int i = 0;

    if (!v) {
        putchar_('0');
        return;
    }
    while (v) {
        b[i++] = '0' + v % 10;
        v /= 10;
    }
    while (i)
        putchar_(b[--i]);
}

static void putd(int v)
{
    if (v < 0) {
        putchar_('-');
        putu((unsigned)(-(long)v));
    } else {
        putu((unsigned)v);
    }
}

static void putx(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 28; i >= 0; i -= 4)
        putchar_(hex[(v >> i) & 0xf]);
}

/* minimal printf: %s %u %d %x %p %% */
static void kprintf(const char *fmt, ...)
{
    __builtin_va_list ap;
    const char *p;

    __builtin_va_start(ap, fmt);
    for (p = fmt; *p; p++) {
        if (*p != '%') {
            putchar_(*p);
            continue;
        }
        p++;
        switch (*p) {
        case 's':
            puts_(__builtin_va_arg(ap, const char *));
            break;
        case 'u':
            putu(__builtin_va_arg(ap, unsigned));
            break;
        case 'd':
            putd(__builtin_va_arg(ap, int));
            break;
        case 'x':
            putx(__builtin_va_arg(ap, uint32_t));
            break;
        case 'p':
            puts_("0x");
            putx((uint32_t)(uintptr_t)__builtin_va_arg(ap, void *));
            break;
        case '%':
            putchar_('%');
            break;
        default:
            putchar_('%');
            putchar_(*p);
            break;
        }
    }
    __builtin_va_end(ap);
}

/* ------------------------------------------------------------------ */
/* PCI                                                                */
/* ------------------------------------------------------------------ */

/* configuration space access ports */
#define PCI_CONFIG_ADDR  0xcf8
#define PCI_CONFIG_DATA  0xcfc
#define PCI_CONFIG_ENABLE (1u << 31)

/* configuration space register offsets */
#define PCI_COMMAND      0x04
#define PCI_CLASS        0x08
#define PCI_HEADER_TYPE  0x0c
#define PCI_BAR0         0x10

/* PCI_COMMAND bits */
#define PCI_CMD_MEM     (1u << 1)   /* memory space decode */
#define PCI_CMD_MASTER  (1u << 2)   /* bus master DMA */

/* address bits of a 32-bit memory BAR (low 4 bits are flags) */
#define PCI_BAR_MEM_ADDR_MASK  (~0xfu)

/* class code for an EHCI controller */
#define PCI_CLASS_SERIAL_BUS  0x0cu
#define PCI_SUBCLASS_USB      0x03u
#define PCI_PROGIF_EHCI       0x20u

/* header type multifunction bit */
#define PCI_HEADER_MULTIFN  0x80u

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    outl(PCI_CONFIG_ADDR, PCI_CONFIG_ENABLE | ((uint32_t)bus << 16) |
                          ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | off);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                        uint32_t val)
{
    outl(PCI_CONFIG_ADDR, PCI_CONFIG_ENABLE | ((uint32_t)bus << 16) |
                          ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | off);
    outl(PCI_CONFIG_DATA, val);
}

/* ------------------------------------------------------------------ */
/* EHCI registers                                                     */
/* ------------------------------------------------------------------ */

/* EHCI operational registers, offsets from op_base (spec section 2.3) */
#define EHCI_USBCMD         0x00
#define EHCI_USBSTS         0x04
#define EHCI_FRINDEX        0x0c
#define EHCI_PERIODICBASE   0x14
#define EHCI_ASYNCLISTADDR  0x18
#define EHCI_CONFIGFLAG     0x40
#define EHCI_PORTSC_BASE    0x44

/* USBCMD bits */
#define USBCMD_RUNSTOP  (1u << 0)
#define USBCMD_HCRESET  (1u << 1)
#define USBCMD_ASE      (1u << 5)
#define USBCMD_IAAD     (1u << 6)

/* USBSTS bits */
#define USBSTS_IAA       (1u << 5)
#define USBSTS_RWC_MASK  0x3fu   /* write-1-to-clear status bits */

static uint32_t op_base;        /* operational register block */
static int      ehci_port = -1; /* port the mass storage device sits on */

static uint32_t reg_read(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(op_base + off);
}

static void reg_write(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)(op_base + off) = val;
}

#define UFRAMES_PER_MS  8u   /* FRINDEX counts 125 us microframes */

static uint32_t frindex(void)
{
    return reg_read(EHCI_FRINDEX);   /* wraps, unsigned delta is safe */
}

static void delay_ms(unsigned ms)
{
    uint32_t start = frindex();

    while (frindex() - start < ms * UFRAMES_PER_MS) { }
}

/* ------------------------------------------------------------------ */
/* EHCI structures                                                    */
/* ------------------------------------------------------------------ */

/* qTD token bits */
#define QTD_ACTIVE        (1u << 7)
#define QTD_HALT          (1u << 6)
#define QTD_DTOGGLE       (1u << 31)
#define QTD_CERR          (3u << 10)
#define QTD_TBYTES_SHIFT  16
#define QTD_TBYTES(t)     (((t) >> QTD_TBYTES_SHIFT) & 0x7fffu)

#define PID_OUT    0u
#define PID_IN     1u
#define PID_SETUP  2u

/* link pointer helpers: bit 0 = terminate, type QH = 1 << 1 */
#define LINK_TERM       1u
#define LINK_QH(p)      ((((uint32_t)(uintptr_t)(p)) & ~0x1fu) | (1u << 1))
#define LINK_QTD(p)     (((uint32_t)(uintptr_t)(p)) & ~0x1fu)

/* QH indices / roles: asynchronous ring is qh[0] -> qh[1] -> qh[2] -> qh[0] */
#define QH_BULK_OUT 0   /* EP 2, OUT, CBW, head of reclamation (H bit) */
#define QH_BULK_IN  1   /* EP 1, IN, data + CSW */
#define QH_CONTROL  2   /* EP 0, control */

#define DEV_ADDR 1       /* USB device address we assign */

/*
 * EHCI queue structures.  They must be 32-byte aligned in memory.
 * The alignment on the TYPE makes the compiler round the array stride
 * up, so every element in a pool stays 32-byte aligned.
 * The extra stride also absorbs QEMU's oversized queue head accesses
 * (it reads/writes the 64-bit-addressing variant of the structs, which
 * is larger than the 32-bit layout).
 */
struct ehci_qtd {
    uint32_t next;
    uint32_t altnext;
    uint32_t token;
    uint32_t buf[5];
} __attribute__((aligned(64)));    /* 32 bytes used, 32-byte aligned */

struct ehci_qh {
    uint32_t link;           /* horizontal link */
    uint32_t epchar;
    uint32_t epcap;
    uint32_t current_qtd;
    /* transfer overlay: */
    uint32_t next_qtd;
    uint32_t altnext_qtd;
    uint32_t token;
    uint32_t buf[5];
} __attribute__((aligned(128)));   /* 48 bytes used, 32-byte aligned */

/* QH endpoint characteristics (epchar) fields */
#define QH_DEVADDR_SHIFT  0
#define QH_EP_SHIFT       8
#define QH_EPS_SHIFT      12
#define QH_EPS_HIGH       2u
#define QH_DTC            (1u << 14)   /* take data toggle from each qTD */
#define QH_H              (1u << 15)   /* head of reclamation list */
#define QH_MPLEN_SHIFT    16

/* QH endpoint capabilities (epcap): mult field, 1 transaction/µframe */
#define QH_MULT_ONE  (1u << 30)

/* USB device address: 7 bits */
#define USB_ADDR_MASK  0x7fu

/* usb-storage bulk endpoints (fixed by its interface descriptor) */
#define BULK_OUT_EP  2u
#define BULK_IN_EP   1u
#define BULK_MAXPKT  512u
#define CTRL_MAXPKT  64u

#define PAGE_SIZE  4096u
#define PAGE_MASK  (PAGE_SIZE - 1)

/* hardware-shared pools: all accesses go through volatile helpers;
 * explicit initializers keep them in .data: a flat binary has no BSS */
static struct ehci_qh  qhs[3]  = { 0 };
static struct ehci_qtd qtds[8] = { 0 };
static uint8_t buf_misc[PAGE_SIZE]    __attribute__((aligned(PAGE_SIZE))) = { 0 };
static uint8_t buf_data[6][PAGE_SIZE] __attribute__((aligned(PAGE_SIZE))) = { 0 };

/* offsets inside buf_misc */
#define MISC_CBW    0
#define MISC_CSW    32
#define MISC_SETUP  64
#define MISC_DESC   128

static inline uint32_t rd32(volatile const void *p)
{
    return *(volatile const uint32_t *)p;
}

static inline void wr32(volatile void *p, uint32_t v)
{
    *(volatile uint32_t *)p = v;
}

static void memset32(volatile void *p, uint32_t v, unsigned n)
{
    volatile uint32_t *d = (volatile uint32_t *)p;

    while (n--)
        *d++ = v;
}

/*
 * Fill in one qTD.  Buffers must live in physically contiguous pages when
 * a transfer spans pages (true for this PoC: buf_data[][PAGE_SIZE]).
 * No data toggle: bulk QHs manage the toggle themselves (QH DTC = 0).
 */
static void qtd_init(struct ehci_qtd *q, uint32_t pid, const void *data,
                     unsigned len, int dt)
{
    uintptr_t addr = (uintptr_t)data;
    unsigned off = addr & PAGE_MASK;
    unsigned npages = (off + len + PAGE_MASK) / PAGE_SIZE;
    unsigned i;

    memset32(q, 0, sizeof(*q) / 4);
    wr32(&q->next, LINK_TERM);
    wr32(&q->altnext, LINK_TERM);       /* short packet detect enabled */
    wr32(&q->token, QTD_ACTIVE | QTD_CERR | (pid << 8) |
                    ((uint32_t)len << QTD_TBYTES_SHIFT) |
                    (dt ? QTD_DTOGGLE : 0));
    for (i = 0; i < npages && i < 5; i++)
        wr32(&q->buf[i], ((addr & ~PAGE_MASK) + i * PAGE_SIZE) | (i ? 0 : off));
}

static void qtd_link(struct ehci_qtd *from, const struct ehci_qtd *to)
{
    wr32(&from->next, LINK_QTD(to));
}

/* wait until qTD is no longer active: 0 = done, -1 = halted, 1 = timeout */
static int qtd_wait(struct ehci_qtd *q, unsigned timeout_ms)
{
    uint32_t start = frindex();

    for (;;) {
        uint32_t t = rd32(&q->token);

        if (!(t & QTD_ACTIVE))
            return (t & QTD_HALT) ? -1 : 0;
        if (frindex() - start >= timeout_ms * 8)
            return 1;
    }
}

/* arm a QH with a chain of qTDs (overlay inactive, HC fetches first qTD) */
static void qh_arm(struct ehci_qh *qh, const struct ehci_qtd *first)
{
    wr32(&qh->current_qtd, 0);
    wr32(&qh->next_qtd, LINK_QTD(first));
    wr32(&qh->altnext_qtd, LINK_TERM);
    wr32(&qh->token, 0);
    memset32(qh->buf, 0, 5);
}

/* mark a QH idle: no qTDs to execute, HC skips it horizontally */
static void qh_idle(struct ehci_qh *qh)
{
    wr32(&qh->next_qtd, LINK_TERM);
    wr32(&qh->altnext_qtd, LINK_TERM);
    wr32(&qh->token, 0);
    memset32(qh->buf, 0, 5);
}

static void qh_set_addr(struct ehci_qh *qh, unsigned addr)
{
    uint32_t v = rd32(&qh->epchar);

    v = (v & ~USB_ADDR_MASK) | (addr & USB_ADDR_MASK);
    wr32(&qh->epchar, v);
}

/* interrupt-on-async-advance doorbell: HC advances past the async list */
static void async_doorbell(void)
{
    uint32_t start = frindex();

    if (reg_read(EHCI_USBSTS) & USBSTS_IAA)
        reg_write(EHCI_USBSTS, USBSTS_IAA);   /* ack a still-set IAA */
    reg_write(EHCI_USBCMD, reg_read(EHCI_USBCMD) | USBCMD_IAAD);
    while (!(reg_read(EHCI_USBSTS) & USBSTS_IAA)) {
        if (frindex() - start > 100 * UFRAMES_PER_MS)
            break;                            /* best effort */
    }
    reg_write(EHCI_USBSTS, USBSTS_IAA);       /* acknowledge */
}

/* ------------------------------------------------------------------ */
/* USB                                                                 */
/* ------------------------------------------------------------------ */

struct usb_setup {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

#define USB_REQ_SET_ADDRESS        5
#define USB_REQ_GET_DESCRIPTOR     6
#define USB_REQ_SET_CONFIGURATION  9

/* setup packet bmRequestType: direction bit */
#define USB_DIR_IN  0x80u

/* USB descriptor types */
#define USB_DT_DEVICE  1u

/* USB device descriptor: length and field offsets */
#define USB_DEVDESC_LEN        18
#define USB_DEVDESC_IDVENDOR   8
#define USB_DEVDESC_IDPRODUCT  10

/*
 * One control transfer on EP0 as a single qTD chain:
 * setup (+ data) + status.  The control QH has DTC = 1, so each qTD
 * carries its own data toggle (setup DATA0, data/status DATA1).
 * usb-storage handles EP0 synchronously, so pipelining is safe here.
 */
static int control_xfer(const struct usb_setup *s, void *data, unsigned len)
{
    struct ehci_qtd *last;
    int ret;

    async_doorbell();
    qtd_init(&qtds[0], PID_SETUP, s, sizeof(*s), 0);
    if (len) {
        qtd_init(&qtds[1], (s->bmRequestType & USB_DIR_IN) ? PID_IN : PID_OUT,
                 data, len, 1);
        qtd_link(&qtds[0], &qtds[1]);
        qtd_init(&qtds[2], (s->bmRequestType & USB_DIR_IN) ? PID_OUT : PID_IN,
                 NULL, 0, 1);
        qtd_link(&qtds[1], &qtds[2]);
        last = &qtds[2];
    } else {
        qtd_init(&qtds[1], (s->bmRequestType & USB_DIR_IN) ? PID_OUT : PID_IN,
                 NULL, 0, 1);
        qtd_link(&qtds[0], &qtds[1]);
        last = &qtds[1];
    }
    qh_arm(&qhs[QH_CONTROL], &qtds[0]);
    ret = qtd_wait(last, 2000);
    if (ret) {
        kprintf("control_xfer %s: setup token %x data/status token %x\n",
                ret == 1 ? "TIMEOUT" : "HALTED",
                rd32(&qtds[0].token), rd32(&last->token));
        return -1;
    }
    return 0;
}

static int usb_enum(void)
{
    struct usb_setup *s = (struct usb_setup *)&buf_misc[MISC_SETUP];
    uint8_t *desc = &buf_misc[MISC_DESC];
    unsigned vid, pid;

    qh_set_addr(&qhs[QH_CONTROL], 0);         /* device is at address 0 */

    /* SET_ADDRESS(1) */
    memset32(s, 0, sizeof(*s) / 4);
    s->bRequest = USB_REQ_SET_ADDRESS;
    s->wValue = DEV_ADDR;
    if (control_xfer(s, NULL, 0))
        return -1;
    delay_ms(2);
    qh_set_addr(&qhs[QH_CONTROL], DEV_ADDR);

    /* GET_DESCRIPTOR(Device) */
    memset32(s, 0, sizeof(*s) / 4);
    s->bmRequestType = USB_DIR_IN;
    s->bRequest = USB_REQ_GET_DESCRIPTOR;
    s->wValue = USB_DT_DEVICE << 8;            /* type Device, index 0 */
    s->wLength = USB_DEVDESC_LEN;
    if (control_xfer(s, desc, USB_DEVDESC_LEN))
        return -1;
    vid = (unsigned)desc[USB_DEVDESC_IDVENDOR] |
          ((unsigned)desc[USB_DEVDESC_IDVENDOR + 1] << 8);
    pid = (unsigned)desc[USB_DEVDESC_IDPRODUCT] |
          ((unsigned)desc[USB_DEVDESC_IDPRODUCT + 1] << 8);
    kprintf("device %x:%x at address %u\n", vid, pid, DEV_ADDR);

    /* SET_CONFIGURATION(1) */
    memset32(s, 0, sizeof(*s) / 4);
    s->bRequest = USB_REQ_SET_CONFIGURATION;
    s->wValue = 1;
    if (control_xfer(s, NULL, 0))
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* USB mass storage, bulk-only transport                               */
/* ------------------------------------------------------------------ */

/* bulk-only transport signatures */
#define MS_CBW_SIG  0x43425355u   /* 'USBC' */
#define MS_CSW_SIG  0x53425355u   /* 'USBS' */

struct ms_cbw {
    uint32_t sig;         /* 'USBC' */
    uint32_t tag;
    uint32_t data_len;    /* dCBWDataTransferLength */
    uint8_t  flags;       /* CBWFLAGS_DATA_IN: data-in, else data-out */
    uint8_t  lun;
    uint8_t  cb_len;      /* bCBWCBLength */
    uint8_t  cb[16];
} __attribute__((packed));

struct ms_csw {
    uint32_t sig;         /* 'USBS' */
    uint32_t tag;
    uint32_t residue;     /* dCSWDataResidue */
    uint8_t  status;      /* bCSWStatus */
} __attribute__((packed));

#define MAX_BYTES_PER_QTD (5 * PAGE_SIZE)  /* one qTD spans 5 buffer pages */

/* CBW flags: direction of the data phase */
#define CBWFLAGS_DATA_IN  0x80u

/* SCSI opcode */
#define SCSI_READ10  0x28u

/* READ(10) command descriptor block: transfer length in blocks,
 * big endian, at this offset within CBWCB (LBA 0 needs no stores) */
#define READ10_XFERLEN  7

/*
 * SCSI READ(10) of nblocks blocks at LBA 0.
 *
 * layout:
 *   - CBW:  qtds[0], 31 bytes OUT on EP2 (qh[QH_BULK_OUT])
 *   - data: qtds[1..n], up to 20480 bytes per qTD, IN on EP1
 *           (qh[QH_BULK_IN]); 41 blocks need 2 data qTDs
 *   - CSW:  qtds[n+1], 13 bytes IN on EP1
 *
 * chain_csw != 0: the CSW qTD is chained behind the last data qTD, i.e.
 * submitted in the same asynchronous queue run (EHCI prefetches it while
 * the data phase is still executing).  chain_csw == 0: the data chain
 * terminates, and the CSW qTD is only submitted after every data qTD has
 * retired (the way a normal host stack submits a separate CSW URB).
 */
static int msc_read(unsigned tag, unsigned nblocks, int chain_csw)
{
    volatile struct ms_cbw *cbw = (struct ms_cbw *)&buf_misc[MISC_CBW];
    volatile struct ms_csw *csw = (struct ms_csw *)&buf_misc[MISC_CSW];
    unsigned nbytes = nblocks * 512;
    unsigned n_data = (nbytes + MAX_BYTES_PER_QTD - 1) / MAX_BYTES_PER_QTD;
    unsigned done = 0, i;
    struct ehci_qtd *cq;
    int ret;

    /* build CBW: READ(10), LBA 0, big endian fields */
    memset32((void *)cbw, 0, sizeof(*cbw) / 4 + 1);
    wr32(&cbw->sig, MS_CBW_SIG);
    wr32(&cbw->tag, tag);
    wr32(&cbw->data_len, nbytes);
    cbw->flags = CBWFLAGS_DATA_IN;
    cbw->cb_len = 12;                          /* bCBWCBLength */
    cbw->cb[0] = SCSI_READ10;
    cbw->cb[READ10_XFERLEN] = (uint8_t)(nblocks >> 8);
    cbw->cb[READ10_XFERLEN + 1] = (uint8_t)nblocks;

    async_doorbell();
    qh_idle(&qhs[QH_CONTROL]);

    /* CBW qTD on the bulk-OUT queue head */
    qtd_init(&qtds[0], PID_OUT, (const void *)cbw, sizeof(struct ms_cbw), 0);

    /* data qTDs on the bulk-IN queue head */
    for (i = 0; i < n_data; i++) {
        unsigned chunk = nbytes - done;

        if (chunk > MAX_BYTES_PER_QTD)
            chunk = MAX_BYTES_PER_QTD;
        qtd_init(&qtds[1 + i], PID_IN, &buf_data[done / PAGE_SIZE], chunk, 0);
        if (i)
            qtd_link(&qtds[i], &qtds[1 + i]); /* qtds[1+i-1] -> qtds[1+i] */
        done += chunk;
    }
    cq = &qtds[1 + n_data];                   /* CSW qTD */
    qtd_init(cq, PID_IN, (const void *)csw, sizeof(struct ms_csw), 0);
    if (chain_csw)
        qtd_link(&qtds[n_data], cq);           /* qtds[1+n-1] -> csw */

    /* CBW phase */
    qh_arm(&qhs[QH_BULK_OUT], &qtds[0]);
    /* data phase (+ CSW if chained) */
    qh_arm(&qhs[QH_BULK_IN], &qtds[1]);

    kprintf("  CBW qTD...");
    ret = qtd_wait(&qtds[0], 1000);
    kprintf(" %s (tbytes %u)\n", ret ? "TIMEOUT/HALT" : "retired",
            QTD_TBYTES(rd32(&qtds[0].token)));

    for (i = 0; i < n_data; i++) {
        kprintf("  data qTD %u...", i);
        ret = qtd_wait(&qtds[1 + i], 1000);
        kprintf(" %s (tbytes %u)\n", ret ? "TIMEOUT/HALT" : "retired",
                QTD_TBYTES(rd32(&qtds[1 + i].token)));
        if (ret)
            return -1;
    }

    if (!chain_csw) {
        /* submit the CSW only now, as its own queue run */
        async_doorbell();
        qh_arm(&qhs[QH_BULK_IN], cq);
    }

    kprintf("  CSW qTD...");
    ret = qtd_wait(cq, 2000);
    if (ret) {
        kprintf(" %s after 2000 ms (tbytes %u)\n",
                ret == 1 ? "still ACTIVE" : "HALTED",
                QTD_TBYTES(rd32(&cq->token)));
        return -1;
    }
    kprintf(" retired (tbytes %u)\n", QTD_TBYTES(rd32(&cq->token)));

    if (rd32(&csw->sig) != MS_CSW_SIG) {
        kprintf("  bad CSW signature %x\n", rd32(&csw->sig));
        return -1;
    }
    kprintf("  CSW: tag %x residue %x status %x\n",
            rd32(&csw->tag), rd32(&csw->residue), csw->status);
    if (rd32(&csw->tag) != tag || csw->status != 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* EHCI bring-up                                                      */
/* ------------------------------------------------------------------ */

#define PORTSC_CONNECT (1u << 0)
#define PORTSC_CSC     (1u << 1)
#define PORTSC_PED     (1u << 2)
#define PORTSC_PEC     (1u << 3)
#define PORTSC_OCC     (1u << 5)
#define PORTSC_PRESET  (1u << 8)

static uint32_t portsc_addr(int port)
{
    return EHCI_PORTSC_BASE + 4 * (unsigned)port;
}

/* acknowledge port change bits; must preserve PED: QEMU's PORTSC write
 * path applies "*portsc &= val | ~PED", so a bare change-ack write would
 * clear the port-enable bit and detach the port from the schedule */
static void portsc_ack(int port)
{
    uint32_t p = portsc_addr(port);

    reg_write(p, (reg_read(p) & PORTSC_PED) |
                 PORTSC_CSC | PORTSC_PEC | PORTSC_OCC);
}

/* reset the port: device returns to address 0, state machine to CBW mode */
static int port_reset(int port)
{
    uint32_t p = portsc_addr(port);
    uint32_t v;

    kprintf("port %d reset: portsc %x -> ", port, reg_read(p));
    portsc_ack(port);
    reg_write(p, reg_read(p) | PORTSC_PRESET);
    delay_ms(60);
    reg_write(p, reg_read(p) & ~PORTSC_PRESET);
    delay_ms(20);
    v = reg_read(p);
    kprintf("%x\n", v);
    portsc_ack(port);
    return (v & PORTSC_CONNECT) && (v & PORTSC_PED) ? 0 : -1;
}

static int ehci_init(void)
{
    unsigned bus, dev, fn;
    uint32_t bar, classc, pcicmd, usbcmd;
    uint32_t mmio;
    int port, nports, found = 0;

    /* find the EHCI controller */
    for (bus = 0; bus < 256 && !found; bus++) {
        for (dev = 0; dev < 32 && !found; dev++) {
            for (fn = 0; fn < 8; fn++) {
                classc = pci_read32(bus, dev, fn, PCI_CLASS) >> 8;
                if (classc == (PCI_CLASS_SERIAL_BUS << 16 |
                               PCI_SUBCLASS_USB << 8 | PCI_PROGIF_EHCI)) {
                    bar = pci_read32(bus, dev, fn, PCI_BAR0);
                    if (!(bar & 1)) {           /* must be a memory BAR */
                        /* enable memory decode + bus master DMA */
                        pcicmd = pci_read32(bus, dev, fn, PCI_COMMAND);
                        pci_write32(bus, dev, fn, PCI_COMMAND,
                                    pcicmd | PCI_CMD_MEM | PCI_CMD_MASTER);
                        mmio = bar & PCI_BAR_MEM_ADDR_MASK;
                        found = 1;
                        break;
                    }
                }
                if (!(pci_read32(bus, dev, fn, PCI_HEADER_TYPE) >> 16 &
                      PCI_HEADER_MULTIFN))
                    break;                      /* no functions 1..7 */
            }
        }
    }
    if (!found) {
        kprintf("no EHCI controller found\n");
        return -1;
    }

    op_base = mmio + *(volatile uint8_t *)(uintptr_t)mmio; /* + CAPLENGTH */
    nports = (int)((*(volatile uint32_t *)(uintptr_t)(mmio + 4)) & 0xf);

    kprintf("takeover: usbcmd %x usbsts %x asynclistaddr %x periodicbase %x\n",
            reg_read(EHCI_USBCMD), reg_read(EHCI_USBSTS),
            reg_read(EHCI_ASYNCLISTADDR), reg_read(EHCI_PERIODICBASE));

    /* take over with a full host controller reset: clears every trace of
     * firmware state, including QEMU's internal schedule state machine */
    reg_write(EHCI_USBCMD, USBCMD_HCRESET);
    while (reg_read(EHCI_USBCMD) & USBCMD_HCRESET) { } /* self-clears */
    reg_write(EHCI_USBSTS, USBSTS_RWC_MASK); /* clear pending status */
    reg_write(EHCI_CONFIGFLAG, 1);           /* CONFIGFLAG: EHCI owns */

    kprintf("hc reset done: usbcmd %x usbsts %x\n",
            reg_read(EHCI_USBCMD), reg_read(EHCI_USBSTS));

    /* asynchronous ring: qh[0] (H) -> qh[1] -> qh[2] -> qh[0] */
    memset32(qhs, 0, sizeof(qhs) / 4);

    wr32(&qhs[0].epchar, DEV_ADDR |
         (BULK_OUT_EP << QH_EP_SHIFT) | (QH_EPS_HIGH << QH_EPS_SHIFT) |
         (BULK_MAXPKT << QH_MPLEN_SHIFT) | QH_H);
    wr32(&qhs[1].epchar, DEV_ADDR |
         (BULK_IN_EP << QH_EP_SHIFT) | (QH_EPS_HIGH << QH_EPS_SHIFT) |
         (BULK_MAXPKT << QH_MPLEN_SHIFT));
    wr32(&qhs[2].epchar, (QH_EPS_HIGH << QH_EPS_SHIFT) |
         (CTRL_MAXPKT << QH_MPLEN_SHIFT) | QH_DTC);
    wr32(&qhs[0].epcap, QH_MULT_ONE);
    wr32(&qhs[1].epcap, QH_MULT_ONE);
    wr32(&qhs[2].epcap, QH_MULT_ONE);
    wr32(&qhs[0].link, LINK_QH(&qhs[1]));
    wr32(&qhs[1].link, LINK_QH(&qhs[2]));
    wr32(&qhs[2].link, LINK_QH(&qhs[0]));
    qh_idle(&qhs[0]);
    qh_idle(&qhs[1]);
    qh_idle(&qhs[2]);

    reg_write(EHCI_ASYNCLISTADDR, LINK_QH(&qhs[0]));

    /* run + enable the asynchronous schedule; from here on QEMU's frame
     * timer keeps running and FRINDEX advances, so delays work */
    usbcmd = reg_read(EHCI_USBCMD);
    kprintf("ring built, asynclistaddr %x; enabling async...\n",
            reg_read(EHCI_ASYNCLISTADDR));
    reg_write(EHCI_USBCMD, usbcmd | USBCMD_RUNSTOP | USBCMD_ASE);
    kprintf("async on: usbcmd %x usbsts %x\n",
            reg_read(EHCI_USBCMD), reg_read(EHCI_USBSTS));
    delay_ms(10);
    kprintf("delay ok: usbsts %x frindex %x\n",
            reg_read(EHCI_USBSTS), reg_read(EHCI_FRINDEX));

    /* find the attached device */
    for (port = 0; port < nports; port++) {
        if (reg_read(portsc_addr(port)) & PORTSC_CONNECT) {
            ehci_port = port;
            break;
        }
    }
    if (ehci_port < 0) {
        kprintf("no device attached\n");
        return -1;
    }
    kprintf("EHCI op base %p, device on port %d\n",
            (void *)(uintptr_t)op_base, ehci_port);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

static int recover(void)
{
    if (port_reset(ehci_port))
        return -1;
    return usb_enum();
}

void cmain(unsigned boot_drive)
{
    int r1, r2, r3;

    console_init();
    kprintf("QEMU usb-storage CSW deadlock PoC (boot drive 0x%x)\n\n",
            boot_drive);

    if (ehci_init())
        goto fail;
    if (port_reset(ehci_port) || usb_enum())
        goto fail;

    kprintf("\n[1] READ(10) 1 block, data in ONE qTD, CSW qTD chained "
            "in the same queue run:\n");
    r1 = msc_read(1, 1, 1);
    kprintf("    -> %s\n\n", r1 ? "FAIL: no CSW, CSW qTD wedged "
                                "(bug reproduced)" : "unexpected pass");

    kprintf("[2] same, after port reset + re-enumeration, 41 blocks "
            "(20992 bytes), data split over TWO qTDs, CSW chained:\n");
    if (recover())
        goto fail;
    r2 = msc_read(2, 41, 1);
    kprintf("    -> %s\n\n", r2 ? "FAIL" : "PASS: CSW received");

    kprintf("[3] same, 1 block, data qTD terminates the queue run, "
            "CSW submitted as its own queue run:\n");
    r3 = msc_read(3, 1, 0);
    kprintf("    -> %s\n\n", r3 ? "FAIL" : "PASS: CSW received");

    kprintf("summary: [1] %s  [2] %s  [3] %s\n",
            r1 ? "HANG (bug)" : "pass",
            r2 ? "FAIL" : "pass",
            r3 ? "FAIL" : "pass");
    kprintf("bug %s on this QEMU\n",
            (r1 && !r2 && !r3) ? "reproduced" : "not reproduced");
    return;

fail:
    kprintf("init failed\n");
}
