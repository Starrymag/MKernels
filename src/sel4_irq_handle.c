#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include <sel4/sel4.h>
#include <simple/simple.h>
#include <simple-default/simple-default.h>
#include <sel4platsupport/platsupport.h>

/* IRQ storm device layout */

#define STORM_IOBASE     0x560
#define STORM_IOSIZE     0x20

#define REG_CTRL         (STORM_IOBASE + 0x00)
#define REG_IRQ          (STORM_IOBASE + 0x01)
#define REG_BURST        (STORM_IOBASE + 0x02)
#define REG_STATUS       (STORM_IOBASE + 0x03)
#define REG_PERIOD_US    (STORM_IOBASE + 0x04)

#define REG_PULSES_LO    (STORM_IOBASE + 0x08)
#define REG_PULSES_HI    (STORM_IOBASE + 0x0C)
#define REG_TIMER_CB     (STORM_IOBASE + 0x10)
#define REG_CFG_WRITES   (STORM_IOBASE + 0x14)
#define REG_EN_TOGGLES   (STORM_IOBASE + 0x18)
#define REG_ACK          (STORM_IOBASE + 0x1C)

#define STORM_IRQ        5

#define CTRL_ENABLE      (1u << 0)
#define CTRL_LEVEL       (1u << 1)

#define STATUS_ENABLED   (1u << 0)
#define STATUS_ASSERT    (1u << 1)
#define STATUS_LEVEL     (1u << 2)

static simple_t simple;

static seL4_CPtr find_untyped_or_die(seL4_BootInfo *bi, uint8_t min_size_bits)
{
    for (seL4_CPtr ut = bi->untyped.start; ut < bi->untyped.end; ut++) {
        int idx = (int)(ut - bi->untyped.start);
        if (bi->untypedList[idx].isDevice) {
            continue;
        }
        if (bi->untypedList[idx].sizeBits >= min_size_bits) {
            return ut;
        }
    }
    printf("No suitable untyped found\n");
    seL4_DebugHalt();
    return 0;
}

typedef struct {
    seL4_CPtr cur;
    seL4_CPtr end;
} cslot_window_t;

static cslot_window_t reserve_cslot_window_from_end(seL4_BootInfo *bi, seL4_Word nslots)
{
    if (nslots == 0) {
        printf("reserve_cslot_window_from_end: nslots=0\n");
        seL4_DebugHalt();
    }

    seL4_Word avail = bi->empty.end - bi->empty.start;
    if (nslots > avail) {
        printf("Not enough empty CSlots\n");
        seL4_DebugHalt();
    }

    cslot_window_t w;
    w.end = bi->empty.end;
    w.cur = bi->empty.end - nslots;
    return w;
}

static seL4_CPtr cslot_alloc_or_die(cslot_window_t *w)
{
    if (w->cur >= w->end) {
        printf("CSlot window exhausted\n");
        seL4_DebugHalt();
    }
    return w->cur++;
}

/* x86 I/O helpers */

static inline uint8_t io_in8(seL4_X86_IOPort io, uint16_t port)
{
    seL4_X86_IOPort_In8_t r = seL4_X86_IOPort_In8(io, port);
    if (r.error) {
        printf("IOPort_In8 error=%ld port=0x%x\n", (long)r.error, port);
    }
    return (uint8_t)r.result;
}

static inline uint32_t io_in32(seL4_X86_IOPort io, uint16_t port)
{
    seL4_X86_IOPort_In32_t r = seL4_X86_IOPort_In32(io, port);
    if (r.error) {
        printf("IOPort_In32 error=%ld port=0x%x\n", (long)r.error, port);
    }
    return (uint32_t)r.result;
}

static inline void io_out8(seL4_X86_IOPort io, uint16_t port, uint8_t val)
{
    int err = seL4_X86_IOPort_Out8(io, port, val);
    if (err) {
        printf("IOPort_Out8 err=%d port=0x%x\n", err, port);
    }
}

static inline void io_out32(seL4_X86_IOPort io, uint16_t port, uint32_t val)
{
    int err = seL4_X86_IOPort_Out32(io, port, val);
    if (err) {
        printf("IOPort_Out32 err=%d port=0x%x\n", err, port);
    }
}

static inline uint64_t read_u64_lohi_stable(seL4_X86_IOPort io, uint16_t lo_port, uint16_t hi_port)
{
    uint32_t hi1, hi2, lo;
    do {
        hi1 = io_in32(io, hi_port);
        lo  = io_in32(io, lo_port);
        hi2 = io_in32(io, hi_port);
    } while (hi1 != hi2);
    return ((uint64_t)hi2 << 32) | lo;
}

static inline void print_cfg(seL4_X86_IOPort io)
{
    uint8_t ctrl = io_in8(io, REG_CTRL);
    uint8_t status = io_in8(io, REG_STATUS);
    uint32_t burst = io_in8(io, REG_BURST);
    uint32_t period = io_in32(io, REG_PERIOD_US);

    printf("cfg: ctrl=0x%02x status=0x%02x burst=%u period-us=%u\n",
           (unsigned)ctrl, (unsigned)status, (unsigned)burst, (unsigned)period);
}

int main(void)
{
    seL4_BootInfo *bi = platsupport_get_bootinfo();
    assert(bi);
    simple_default_init_bootinfo(&simple, bi);

    printf("seL4 pc99: isa-irq-storm demo start (new device, no DebugRunTime)\n");

    cslot_window_t win = reserve_cslot_window_from_end(bi, 32);

    seL4_CPtr ntfn_slot   = cslot_alloc_or_die(&win);
    seL4_CPtr irqh_slot   = cslot_alloc_or_die(&win);
    seL4_CPtr ioport_slot = cslot_alloc_or_die(&win);
    (void)cslot_alloc_or_die(&win);

    seL4_CPtr ut = find_untyped_or_die(bi, seL4_NotificationBits);

    seL4_Error err = seL4_Untyped_Retype(
        ut,
        seL4_NotificationObject,
        0,
        seL4_CapInitThreadCNode,
        0,
        0,
        ntfn_slot,
        1
    );
    assert(err == 0);
    seL4_CPtr ntfn = ntfn_slot;

    err = seL4_X86_IOPortControl_Issue(
        seL4_CapIOPortControl,
        STORM_IOBASE,
        (uint16_t)(STORM_IOBASE + STORM_IOSIZE - 1),
        seL4_CapInitThreadCNode,
        ioport_slot,
        seL4_WordBits
    );
    assert(err == 0);
    seL4_X86_IOPort io = (seL4_X86_IOPort)ioport_slot;

    err = seL4_IRQControl_GetIOAPIC(
        seL4_CapIRQControl,
        seL4_CapInitThreadCNode,
        irqh_slot,
        seL4_WordBits,
        0,
        STORM_IRQ,
        1,
        1,
        STORM_IRQ
    );
    assert(err == 0);
    seL4_CPtr irq_handler = irqh_slot;

    err = seL4_IRQHandler_SetNotification(irq_handler, ntfn);
    assert(err == 0);
    err = seL4_IRQHandler_Ack(irq_handler);
    assert(err == 0);

    printf("Device reports IRQ line: %u\n", (unsigned)io_in8(io, REG_IRQ));
    print_cfg(io);

    /* Ensure enabled (do not change LEVEL bit set by QEMU unless you want to) */
    uint8_t ctrl = io_in8(io, REG_CTRL);
    if (!(ctrl & CTRL_ENABLE)) {
        ctrl |= CTRL_ENABLE;
        io_out8(io, REG_CTRL, ctrl);
    }

    /* Reporting cadence: every N handled notifications */
    const uint64_t report_every_handled = 1ULL << 16; /* 65536 */
    uint64_t handled = 0;

    uint64_t last_pulses = read_u64_lohi_stable(io, REG_PULSES_LO, REG_PULSES_HI);
    uint32_t last_timer_cb = io_in32(io, REG_TIMER_CB);
    uint32_t last_cfg_writes = io_in32(io, REG_CFG_WRITES);
    uint32_t last_en_toggles = io_in32(io, REG_EN_TOGGLES);

    seL4_Word last_badge = 0;
    uint8_t last_status = io_in8(io, REG_STATUS);

    while (1) {
        seL4_Word badge = 0;
        seL4_Wait(ntfn, &badge);
        handled++;
        last_badge = badge;

        /* Minimal per-IRQ work */
        uint8_t status = io_in8(io, REG_STATUS);
        last_status = status;

        /* If device is in LEVEL mode and currently asserted, ACK it */
        if ((status & STATUS_LEVEL) && (status & STATUS_ASSERT)) {
            io_out32(io, REG_ACK, 1);
        }

        err = seL4_IRQHandler_Ack(irq_handler);
        if (err) {
            printf("IRQHandler_Ack error: %d\n", (int)err);
        }

        if ((handled & (report_every_handled - 1)) == 0) {
            uint64_t pulses = read_u64_lohi_stable(io, REG_PULSES_LO, REG_PULSES_HI);
            uint32_t timer_cb = io_in32(io, REG_TIMER_CB);
            uint32_t cfg_writes = io_in32(io, REG_CFG_WRITES);
            uint32_t en_toggles = io_in32(io, REG_EN_TOGGLES);

            uint64_t dpulses = pulses - last_pulses;
            uint32_t dtimer_cb = timer_cb - last_timer_cb;
            uint32_t dcfg = cfg_writes - last_cfg_writes;
            uint32_t dtog = en_toggles - last_en_toggles;

            uint8_t cur_ctrl = io_in8(io, REG_CTRL);
            uint32_t cur_burst = io_in8(io, REG_BURST);
            uint32_t cur_period = io_in32(io, REG_PERIOD_US);

            printf("storm: handled=%llu (+%llu) dpulses=%llu dtimer_cb=%u dcfg=%u dtog=%u ctrl=0x%02x status=0x%02x badge=0x%lx burst=%u period-us=%u total_pulses=%llu\n",
                   (unsigned long long)handled,
                   (unsigned long long)report_every_handled,
                   (unsigned long long)dpulses,
                   (unsigned)dtimer_cb,
                   (unsigned)dcfg,
                   (unsigned)dtog,
                   (unsigned)cur_ctrl,
                   (unsigned)last_status,
                   (unsigned long)last_badge,
                   (unsigned)cur_burst,
                   (unsigned)cur_period,
                   (unsigned long long)pulses);

            last_pulses = pulses;
            last_timer_cb = timer_cb;
            last_cfg_writes = cfg_writes;
            last_en_toggles = en_toggles;
        }
    }

    return 0;
}
