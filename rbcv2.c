/*
 *	Platform features
 *	Z80A @ 8Mhz
 *	1MB ROM (max), 512K RAM
 *	16550A UART @1.8432Mhz at I/O 0x68
 *	DS1302 bitbanged RTC
 *	8255 for PPIDE etc
 *	Memory banking
 *	0x78-7B: RAM bank
 *	0x7C-7F: ROM bank (or set bit 7 to get RAM bank)
 *
 *	IRQ from serial only, or from ECB bus but not serial
 *	Optional PropIO v2 for I/O ports (keyboard/video/sd)
 *
 *	General stuff to tackle
 *	Interrupt jumper (ECB v 16x50)
 *	16x50 interrupts	(partly done)
 *	ECB and timer via UART interrupt hack (timer done)
 *	DS1302 burst mode for memory
 *	Do we care about DS1302 writes ?
 *	Whine/break on invalid PPIDE sequences to help debug code
 *	Memory jumpers (is it really 16/48 or 48/16 ?)
 *	Z80 CTC card (ECB)
 *	4UART needs connecting to something as does uart0 when in PropIO
 *	SCG would be fun but major work (does provide vblank though)
 *
 *	Fix usage!
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include "libz80/z80.h"
#include "ide.h"
#include "propio.h"
#include "rtc_bitbang.h"
#include "w5100.h"

#define HIRAM	63

static uint8_t ramrom[64][32768];	/* 512K ROM for now */
static uint8_t rombank;
static uint8_t rambank;

static uint8_t ide;
static struct ide_controller *ide0;
static struct propio *propio;
static uint8_t timerhack;
static uint8_t fast;
static uint8_t wiznet;

static Z80Context cpu_z80;

static nic_w5100_t *wiz;
static struct rtc *rtc;

static volatile int done;

#define TRACE_MEM	1
#define TRACE_IO	2
#define TRACE_ROM	4
#define TRACE_UNK	8
#define TRACE_RTC	16
#define TRACE_PPIDE	32
#define TRACE_PROP	64
#define TRACE_BANK	128
#define TRACE_UART	256

static int trace = 0;

static uint8_t mem_read(int unused, uint16_t addr)
{
    if (trace & TRACE_MEM)
        fprintf(stderr, "R %04X: ", addr);
    if (addr > 32767) {
        if (trace & TRACE_MEM)
            fprintf(stderr, "HR %04X<-%02X\n",
                addr & 0x7FFF, ramrom[HIRAM][addr & 0x7FFF]);
        return ramrom[HIRAM][addr & 0x7FFF];
    }
    if (rombank & 0x80) {
        if (trace & TRACE_MEM)
            fprintf(stderr, "LR%d %04X<-%02X\n",
                rambank & 0x1F, addr, ramrom[32 + (rambank & 0x1F)][addr]);
        return ramrom[32 + (rambank & 0x1F)][addr];
    }
    if (trace & TRACE_MEM)
        fprintf(stderr, "LF%d %04X<->%02X\n",
            rombank & 0x1F, addr, ramrom[rombank & 0x1F][addr]);
    return ramrom[rombank & 0x1F][addr];
}

static void mem_write(int unused, uint16_t addr, uint8_t val)
{
    if (trace & TRACE_MEM)
        fprintf(stderr, "W %04X: ", addr);
    if (addr > 32767) {
        if (trace & TRACE_MEM)
            fprintf(stderr, "HR %04X->%02X\n",addr, val);
        ramrom[HIRAM][addr & 0x7FFF] = val;
    }
    else if (rombank & 0x80) {
        if (trace & TRACE_MEM)
            fprintf(stderr, "LR%d %04X->%02X\n", (rambank & 0x1F), addr, val);
        ramrom[32 + (rambank & 0x1F)][addr] = val;
    } else if (trace & TRACE_MEM)
        fprintf(stderr, "LF%d %04X->ROM\n",
            (rombank & 0x1F), addr);
}

unsigned int check_chario(void)
{
    fd_set i, o;
    struct timeval tv;
    unsigned int r = 0;

    FD_ZERO(&i);
    FD_SET(0, &i);
    FD_ZERO(&o);
    FD_SET(1, &o);
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    if (select(2, &i, NULL, NULL, &tv) == -1) {
	perror("select");
	exit(1);
    }
    if (FD_ISSET(0, &i))
	r |= 1;
    if (FD_ISSET(1, &o))
	r |= 2;
    return r;
}

unsigned int next_char(void)
{
    char c;
    if (read(0, &c, 1) != 1) {
	printf("(tty read without ready byte)\n");
	return 0xFF;
    }
    if (c == 0x0A)
        c = '\r';
    return c;
}

/*
 *	Emulate PPIDE. It's not a particularly good emulation of the actual
 *	port behaviour if misprogrammed but should be accurate for correct
 *	use of the device.
 */
static uint8_t pioreg[4];

static void pio_write(uint8_t addr, uint8_t val)
{
    /* Compute all the deltas */
    uint8_t changed = pioreg[addr] ^ val;
    uint8_t dhigh = val & changed;
    uint8_t dlow = ~val & changed;
    uint16_t d;

    switch(addr) {
        case 0:	/* Port A data */
        case 1:	/* Port B data */
            pioreg[addr] = val;
            if (trace & TRACE_PPIDE)
                fprintf(stderr, "Data now %04X\n", (((uint16_t)pioreg[1]) << 8) | pioreg[0]);
            break;
        case 2:	/* Port C - address/control lines */
            pioreg[addr] = val;
            if (!ide0)
                return;
            if (val & 0x80) {
                if (trace & TRACE_PPIDE)
                    fprintf(stderr, "ide in reset.\n");
                ide_reset_begin(ide0);
                return;
            }
            if ((trace & TRACE_PPIDE) && (dlow & 0x80))
                fprintf(stderr, "ide exits reset.\n");

            /* This register is effectively the bus to the IDE device
               bits 0-2 are A0-A2, bit 3 is CS0 bit 4 is CS1 bit 5 is W
               bit 6 is R bit 7 is reset */
            d = val & 0x07;
            /* Altstatus and friends */
            if (val & 0x10)
                d += 2;
            if (dlow & 0x20) {
                if (trace & TRACE_PPIDE)
                    fprintf(stderr, "write edge: %02X = %04X\n", d,
                        ((uint16_t)pioreg[1] << 8) | pioreg[0]);
                ide_write16(ide0, d, ((uint16_t)pioreg[1] << 8) | pioreg[0]);
            } else if (dhigh & 0x40) {
                /* Prime the data ports on the rising edge */
                if (trace & TRACE_PPIDE)
                    fprintf(stderr, "read edge: %02X = ", d);
                d = ide_read16(ide0, d);
                if (trace & TRACE_PPIDE)
                    fprintf(stderr, "%04X\n", d);
                pioreg[0] = d;
                pioreg[1] = d >> 8;
            }
            break;
        case 3: /* Control register */
            /* We could check the direction bits but we don't */
            pioreg[addr] = val;
            break;
    }
}

static uint8_t pio_read(uint8_t addr)
{
    if (trace & TRACE_PPIDE)
        fprintf(stderr, "ide read %d:%02X\n", addr, pioreg[addr]);
    return pioreg[addr];
}

/* UART: very mimimal for the moment */

struct uart16x50 {
    uint8_t ier;
    uint8_t iir;
    uint8_t fcr;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scratch;
    uint8_t ls;
    uint8_t ms;
    uint8_t dlab;
    uint8_t irq;
#define RXDA	1
#define TEMT	2
#define MODEM	8
    uint8_t irqline;
};

static struct uart16x50 uart[5];

static void uart_init(struct uart16x50 *uptr)
{
    uptr->dlab = 0;
}

/* Compute the interrupt indicator register from what is pending */
static void uart_recalc_iir(struct uart16x50 *uptr)
{
    if (uptr->irq & RXDA)
        uptr->iir = 0x04;
    else if (uptr->irq & TEMT)
        uptr->iir = 0x02;
    else if (uptr->irq & MODEM)
        uptr->iir = 0x00;
    else {
        uptr->iir = 0x01;	/* No interrupt */
        uptr->irqline = 0;
        return;
    }
    /* Ok so we have an event, do we need to waggle the line */
    if (uptr->irqline)
        return;
    uptr->irqline = uptr->irq;
    Z80INT(&cpu_z80, 0xFF);	/* actually undefined */
    
}

/* Raise an interrupt source. Only has an effect if enabled in the ier */
static void uart_interrupt(struct uart16x50 *uptr, uint8_t n)
{
    if (uptr->irq & n)
        return;
    if (!(uptr->ier & n))
        return;
    uptr->irq |= n;
    uart_recalc_iir(uptr);
}

static void uart_clear_interrupt(struct uart16x50 *uptr, uint8_t n)
{
    if (!(uptr->irq & n))
        return;
    uptr->irq &= ~n;
    uart_recalc_iir(uptr);
}

static void uart_event(struct uart16x50 *uptr)
{
    uint8_t r = check_chario();
    uint8_t old = uptr->lsr;
    uint8_t dhigh;
    if (r & 1)
        uptr->lsr |= 0x01;	/* RX not empty */
    if (r & 2)
        uptr->lsr |= 0x60;	/* TX empty */
    dhigh = (old ^ uptr->lsr);
    dhigh &= uptr->lsr;		/* Changed high bits */
    if (dhigh & 1)
        uart_interrupt(uptr, RXDA);
    if (dhigh & 0x2)
        uart_interrupt(uptr, TEMT);
}

static void show_settings(struct uart16x50 *uptr)
{
    uint32_t baud;

    if (!(trace & TRACE_UART))
        return;

    baud = uptr->ls + (uptr->ms << 8);
    if (baud == 0)
        baud = 1843200;
    baud = 1843200 / baud;
    baud /= 16;
    fprintf(stderr, "[%d:%d",
            baud, (uptr->lcr &3) + 5);
    switch(uptr->lcr & 0x38) {
        case 0x00:
        case 0x10:
        case 0x20:
        case 0x30:
            fprintf(stderr, "N");
            break;
        case 0x08:
            fprintf(stderr, "O");
            break;
        case 0x18:
            fprintf(stderr, "E");
            break;
        case 0x28:
            fprintf(stderr, "M");
            break;
        case 0x38:
            fprintf(stderr, "S");
            break;
    }
    fprintf(stderr, "%d ",
            (uptr->lcr & 4) ? 2 : 1);

    if (uptr->lcr & 0x40)
        fprintf(stderr, "break ");
    if (uptr->lcr & 0x80)
        fprintf(stderr, "dlab ");
    if (uptr->mcr & 1)
        fprintf(stderr, "DTR ");
    if (uptr->mcr & 2)
        fprintf(stderr, "RTS ");
    if (uptr->mcr & 4)
        fprintf(stderr, "OUT1 ");
    if (uptr->mcr & 8)
        fprintf(stderr, "OUT2 ");
    if (uptr->mcr & 16)
        fprintf(stderr, "LOOP ");
    fprintf(stderr, "ier %02x]\n", uptr->ier);
}

static void uart_write(struct uart16x50 *uptr, uint8_t addr, uint8_t val)
{
    switch(addr) {
    case 0:	/* If dlab = 0, then write else LS*/
        if (uptr->dlab == 0) {
            if (uptr == &uart[0]) {
                putchar(val);
                fflush(stdout);
            }
            uart_clear_interrupt(uptr, TEMT);
            uart_interrupt(uptr, TEMT);
        } else {
            uptr->ls = val;
            show_settings(uptr);
        }
        break;
    case 1:	/* If dlab = 0, then IER */
        if (uptr->dlab) {
            uptr->ms= val;
            show_settings(uptr);
        }
        else
            uptr->ier = val;
        break;
    case 2:	/* FCR */
        uptr->fcr = val & 0x9F;
        break;
    case 3:	/* LCR */
        uptr->lcr = val;
        uptr->dlab = (uptr->lcr & 0x80);
        show_settings(uptr);
        break;
    case 4:	/* MCR */
        uptr->mcr = val & 0x3F;
        show_settings(uptr);
        break;
    case 5:	/* LSR (r/o) */
        break;
    case 6:	/* MSR (r/o) */
        break;
    case 7:	/* Scratch */
        uptr->scratch = val;
        break;
    }
}

static uint8_t uart_read(struct uart16x50 *uptr, uint8_t addr)
{
    uint8_t r;

    switch(addr) {
    case 0:
        /* receive buffer */
        if (!propio && uptr == &uart[0] && uptr->dlab == 0) {
            uart_clear_interrupt(uptr, RXDA);
            return next_char();
        }
        break;
    case 1:
        /* IER */
        return uptr->ier;
    case 2:
        /* IIR */
        return uptr->iir;
    case 3:
        /* LCR */
        return uptr->lcr;
    case 4:
        /* mcr */
        return uptr->mcr;
    case 5:
        /* lsr */
        if (!propio) {
            r = check_chario();
            uptr->lsr = 0;
            if (!propio && (r & 1))
                 uptr->lsr |= 0x01;	/* Data ready */
            if (r & 2)
                 uptr->lsr |= 0x60;	/* TX empty | holding empty */
            /* Reading the LSR causes these bits to clear */
            r = uptr->lsr;
            uptr->lsr &= 0xF0;
            return r;
        }
        return 0x60;
    case 6:
        /* msr */
        r = uptr->msr;
        /* Reading clears the delta bits */
        uptr->msr &= 0xF0;
        uart_clear_interrupt(uptr, MODEM);
        return r;
    case 7:
        return uptr->scratch;
    }
    return 0xFF;
}

/* Clock timer hack. The (signal level) DSR line on the jumpers is connected
   to a slow clock generator */
static void timer_pulse(void)
{
    struct uart16x50 *uptr = uart;
    if (timerhack) {
        uptr->msr ^= 0x20;	/* DSR toggles */
        uptr->msr |= 0x02;	/* DSR delta */
        uart_interrupt(uptr, MODEM);
    }
}


static int ramf;
static int ramf_fd;
static const char *ramf_path = "ramf.disk";
static uint8_t *ramf_addr;
static uint8_t ramf_port[2][2];
static uint16_t ramf_count[2];

static void ramf_init(void)
{
    ramf_fd = open(ramf_path, O_RDWR|O_CREAT, 0600);
    if(ramf_fd == -1) {
        perror(ramf_path);
        ramf = 0;
        return;
    }
    ramf_addr = mmap(NULL, 8192 * 1024, PROT_READ|PROT_WRITE,
        MAP_SHARED, ramf_fd, 0L);
    if (ramf_addr == MAP_FAILED) {
        perror("mmap");
        close(ramf_fd);
        ramf = 0;
        return;
    }
}

static uint8_t *ramaddr(uint8_t high)
{
    uint32_t offset = high ? 4096 * 1024 : 0;
    offset += (ramf_port[high][0] & 0x1F) << 17;
    offset += ramf_port[high][1] << 9;
    offset += ramf_count[high]++;
    return ramf_addr + offset;
}

static void ramf_write(uint8_t addr, uint8_t val)
{
    uint8_t high = (addr & 4) ? 1 : 0;
    fprintf(stderr, "RAMF write %d = %d\n", addr, val);
    addr &= 3;
    if (addr == 0)
        *ramaddr(high) = val;
    else if (addr == 3)
        return;
    else {
        ramf_port[high][addr & 1] = val;
        ramf_count[high] = 0;
    }
}

static uint8_t ramf_read(uint8_t addr)
{
    uint8_t high = (addr & 4) ? 1 : 0;
    fprintf(stderr, "RAMF read %d\n", addr);
    addr &= 3;
    if (addr == 0)
        return *ramaddr(high);
    if (addr == 3)
        return 0;	/* or 1 for write protected */
    return ramf_port[high][addr];
}

static uint8_t io_read(int unused, uint16_t addr)
{
    if (trace & TRACE_IO)
        fprintf(stderr, "read %02x\n", addr);
    addr &= 0xFF;
    if (addr >= 0x28 && addr <= 0x2C && wiznet)
        return nic_w5100_read(wiz, addr & 3);
    if (addr >= 0x60 && addr <= 0x67) 	/* Aliased */
        return pio_read(addr & 3);
    if (addr >= 0x68 && addr < 0x70)
        return uart_read(&uart[0], addr & 7);
    if (addr >= 0x70 && addr <= 0x77)
        return rtc_read(rtc);
    if (ramf && (addr >= 0xA0 && addr <= 0xA7))
        return ramf_read(addr & 7);
    if (propio && (addr >= 0xA8 && addr <= 0xAF))
        return propio_read(propio, addr);
    if (addr >= 0xC0 && addr <= 0xDF)
        return uart_read(&uart[((addr - 0xC0) >> 3) + 1], addr & 7);
    if (trace & TRACE_UNK)
        fprintf(stderr, "Unknown read from port %04X\n", addr);
    return 0xFF;
}

static void io_write(int unused, uint16_t addr, uint8_t val)
{
    if (trace & TRACE_IO)
        fprintf(stderr, "write %02x <- %02x\n", addr & 0xFF, val);
    addr &= 0xFF;
    if (addr >= 0x28 && addr <= 0x2C && wiznet)
        nic_w5100_write(wiz, addr & 3, val);
    else if (addr >= 0x60 && addr <= 0x67)	/* Aliased */
        pio_write(addr & 3, val);
    else if (addr >= 0x68 && addr < 0x70)
        uart_write(&uart[0], addr & 7, val);
    else if (addr >= 0x70 && addr <= 0x77)
        rtc_write(rtc, val);
    else if (addr >= 0x78 && addr <= 0x79) {
        if (trace & TRACE_BANK)
            fprintf(stderr, "RAM bank to %02X\n", val);
        rambank = val;
    } else if (addr >= 0x7C && addr <= 0x7F) {
        if (trace & TRACE_BANK) {
            fprintf(stderr, "ROM bank to %02X\n", val);
            if (val & 0x80)
                fprintf(stderr, "Using RAM bank %d\n", rambank & 0x1F);
        }
        rombank = val;
    }
    else if (ramf && addr >=0xA0 && addr <=0xA7)
        ramf_write(addr & 0x07, val);
    else if (propio && addr >= 0xA8 && addr <= 0xAF)
        propio_write(propio, addr & 0x03, val);
    else if (addr >= 0xC0 && addr <= 0xDF)
        uart_write(&uart[((addr - 0xC0) >> 3) + 1], addr & 7, val);
    else if (addr == 0xFD) {
        printf("trace set to %d\n", val);
        trace = val;
    }
    else if (trace & TRACE_UNK)
        fprintf(stderr, "Unknown write to port %02X of %02X\n",
            addr & 0xFF, val);
}

static struct termios saved_term, term;

static void cleanup(int sig)
{
    tcsetattr(0, TCSADRAIN, &saved_term);
    exit(1);
}

static void exit_cleanup(void)
{
    tcsetattr(0, TCSADRAIN, &saved_term);
}

static void usage(void)
{
    fprintf(stderr, "rcbv2: [-r rompath] [-i idepath] [-t] [-p] [-s sdcardpath] [-d tracemask] [-R]\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
    static struct timespec tc;
    int opt;
    int fd;
    char *rompath = "sbc.rom";
    char *ppath = NULL;
    char *idepath[2] = { NULL, NULL };
    int i;
    unsigned int prop = 0;

    while((opt = getopt(argc, argv, "r:i:s:ptd:fRw")) != -1) {
        switch(opt) {
            case 'r':
                rompath = optarg;
                break;
                break;
            case 'i':
                if (ide == 2)
                    fprintf(stderr, "sbcv2: only two disks per controller.\n");
                else
                    idepath[ide++] = optarg;
                break;
            case 's':
                ppath = optarg;
            case 'p':
                prop = 1;
                break;
            case 't':
                timerhack = 1;
                break;
            case 'd':
                trace = atoi(optarg);
                break;
            case 'f':
                fast = 1;
                break;
            case 'R':
                ramf = 1;
                break;
            case 'w':
                wiznet = 1;
                break;
            default:
                usage();
        }
    }
    if (optind < argc)
        usage();

    fd = open(rompath, O_RDONLY);
    if (fd == -1) {
        perror(rompath);
        exit(EXIT_FAILURE);
    }
    for (i = 0; i < 16; i++) {
        if (read(fd, ramrom[i], 32768) != 32768) {
            fprintf(stderr, "sbcv2: banked rom image should be 512K.\n");
            exit(EXIT_FAILURE);
        }
    }
    close(fd);

    if (ide) {
        ide0 = ide_allocate("cf");
        if (ide0) {
            fd = open(idepath[0], O_RDWR);
            if (fd == -1) {
                perror(idepath[0]);
                ide = 0;
            } else if (ide_attach(ide0, 0, fd) == 0) {
                ide = 1;
                ide_reset_begin(ide0);
            }
            if (idepath[1]) {
                fd = open(idepath[1], O_RDWR);
                if (fd == -1)
                    perror(idepath[1]);
                ide_attach(ide0, 1, fd);
            }
        } else
            ide = 0;
    }

    rtc = rtc_create();
    rtc_trace(rtc, trace & TRACE_RTC);

    if (prop) {
        propio = propio_create(ppath);
        propio_set_input(propio, 1);
        propio_trace(propio, trace & TRACE_PROP);
    }

    ramf_init();

    if (ramf)
        ramf_init();

    uart_init(&uart[0]);
    uart_init(&uart[1]);
    uart_init(&uart[2]);
    uart_init(&uart[3]);
    uart_init(&uart[4]);

    if (wiznet) {
        wiz = nic_w5100_alloc();
        nic_w5100_reset(wiz);
    }

    /* No real need for interrupt accuracy so just go with the timer. If we
       ever do the UART as timer hack it'll need addressing! */
    tc.tv_sec = 0;
    tc.tv_nsec = 100000000L;

    if (tcgetattr(0, &term) == 0) {
	saved_term = term;
	atexit(exit_cleanup);
	signal(SIGINT, cleanup);
	signal(SIGQUIT, cleanup);
	signal(SIGPIPE, cleanup);
	term.c_lflag &= ~(ICANON|ECHO);
	term.c_cc[VMIN] = 1;
	term.c_cc[VTIME] = 0;
	term.c_cc[VINTR] = 0;
	term.c_cc[VSUSP] = 0;
	term.c_cc[VSTOP] = 0;
	tcsetattr(0, TCSADRAIN, &term);
    }

    Z80RESET(&cpu_z80);
    cpu_z80.ioRead = io_read;
    cpu_z80.ioWrite = io_write;
    cpu_z80.memRead = mem_read;
    cpu_z80.memWrite = mem_write;

    /* This is the wrong way to do it but it's easier for the moment. We
       should track how much real time has occurred and try to keep cycle
       matched with that. The scheme here works fine except when the host
       is loaded though */

    /* 4MHz Z80 - 4,000,000 tstates / second */
    while (!done) {
        Z80ExecuteTStates(&cpu_z80, 400000);
	/* Do 100ms of I/O and delays */
	if (!fast)
	    nanosleep(&tc, NULL);
	uart_event(uart);
	timer_pulse();
        if (wiznet)
            w5100_process(wiz);
    }
    exit(0);
}
