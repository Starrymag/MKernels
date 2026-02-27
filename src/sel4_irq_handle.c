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
#define STORM_IOSIZE     0x10

#define REG_CTRL         (STORM_IOBASE + 0x00)
#define REG_IRQ          (STORM_IOBASE + 0x01)
#define REG_BURST        (STORM_IOBASE + 0x02)
#define REG_STATUS       (STORM_IOBASE + 0x03)
#define REG_PERIOD_US    (STORM_IOBASE + 0x04)
#define REG_PULSES_LO    (STORM_IOBASE + 0x08)
#define REG_PULSES_HI    (STORM_IOBASE + 0x0C)

#define STORM_IRQ        5

static simple_t simple;

/* CSlot window allocator */

static seL4_CPtr next_free_slot;

static seL4_CPtr alloc_cslot_or_die(seL4_BootInfo *bi)
{
    if (next_free_slot == 0) {
        next_free_slot = bi->empty.start;
    }
    if (next_free_slot >= bi->empty.end) {
        printf("Out of empty CSlots\n");
        seL4_DebugHalt();
    }
    return next_free_slot++;
}

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

typedef struct {
    seL4_CPtr cur;
    seL4_CPtr end;
} cslot_window_t;

/* Reserve a block of CSlots from BootInfo empty range */

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

int main(void)
{
    seL4_BootInfo *bi = platsupport_get_bootinfo();
    assert(bi);
    simple_default_init_bootinfo(&simple, bi);

    printf("seL4 pc99: isa-irq-storm demo start\n");

    cslot_window_t win = reserve_cslot_window_from_end(bi, 32);

    seL4_CPtr ntfn_slot   = cslot_alloc_or_die(&win);
    seL4_CPtr irqh_slot   = cslot_alloc_or_die(&win);
    seL4_CPtr ioport_slot = cslot_alloc_or_die(&win);
    seL4_CPtr extra1      = cslot_alloc_or_die(&win);

    seL4_CPtr ut = find_untyped_or_die(bi, seL4_NotificationBits);

    printf("empty slots: [%lu .. %lu)\n",
           (unsigned long)bi->empty.start,
           (unsigned long)bi->empty.end);

    printf("using slots: ntfn=%lu irqh=%lu ioport=%lu\n",
           (unsigned long)ntfn_slot,
           (unsigned long)irqh_slot,
           (unsigned long)ioport_slot);

    /* Create Notification */

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

    /* Issue IOPort capability */

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

    /* Obtain IRQ handler via IOAPIC */

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

    printf("Bound IRQ %d to notification. Enabling device...\n", STORM_IRQ);

    /* Enable device */

    uint8_t irq_line = io_in8(io, REG_IRQ);
    printf("Storm device reports IRQ line: %u\n", (unsigned)irq_line);

    io_out8(io, REG_CTRL, 0x1);

    printf("Initial burst=%u status=0x%02x period-us=%u\n",
           (unsigned)io_in8(io, REG_BURST),
           (unsigned)io_in8(io, REG_STATUS),
           (unsigned)io_in32(io, REG_PERIOD_US));

    /* IRQ handling loop */

    while (1) {
        seL4_Word badge = 0;
        seL4_Wait(ntfn, &badge);

        uint32_t pulses_lo = io_in32(io, REG_PULSES_LO);
        uint32_t pulses_hi = io_in32(io, REG_PULSES_HI);
        uint64_t pulses = ((uint64_t)pulses_hi << 32) | pulses_lo;

        uint8_t status = io_in8(io, REG_STATUS);

        printf("IRQ storm: pulses=%llu status=0x%02x badge=0x%lx\n",
               (unsigned long long)pulses,
               (unsigned)status,
               (unsigned long)badge);

        err = seL4_IRQHandler_Ack(irq_handler);
        if (err) {
            printf("IRQHandler_Ack error: %d\n", (int)err);
        }
    }

    return 0;
}
