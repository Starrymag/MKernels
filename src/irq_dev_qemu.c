/*
 * QEMU ISA interrupt storm generator.
 *
 * This device is intended for stress-testing interrupt handling in guests.
 */

#include "qemu/osdep.h"
#include "hw/isa/isa.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qom/object.h"

#define TYPE_ISA_IRQ_STORM_DEVICE "isa-irq-storm"
OBJECT_DECLARE_SIMPLE_TYPE(ISAIrqStormState, ISA_IRQ_STORM_DEVICE)

#define IRQ_STORM_REG_CTRL       0x00
#define IRQ_STORM_REG_IRQ        0x01
#define IRQ_STORM_REG_BURST      0x02
#define IRQ_STORM_REG_STATUS     0x03
#define IRQ_STORM_REG_PERIOD_US  0x04
#define IRQ_STORM_REG_PULSES     0x08

#define IRQ_STORM_CTRL_ENABLE    BIT(0)
#define IRQ_STORM_MAX_BURST      100000U

struct ISAIrqStormState {
    ISADevice parent_obj;

    MemoryRegion io;
    QEMUTimer *timer;
    qemu_irq irq;

    uint32_t iobase;
    uint32_t iosize;
    uint32_t isairq;
    uint32_t burst;
    uint32_t period_us;
    bool start_enabled;

    uint8_t control;
    uint64_t pulses_emitted;
};

static void irq_storm_schedule_next(ISAIrqStormState *s)
{
    int64_t now;
    uint64_t period_ns;

    if (!(s->control & IRQ_STORM_CTRL_ENABLE)) {
        return;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    period_ns = MAX(1U, s->period_us) * SCALE_US;
    timer_mod(s->timer, now + period_ns);
}

static void irq_storm_timer_cb(void *opaque)
{
    ISAIrqStormState *s = opaque;
    uint32_t i;
    uint32_t pulses;

    if (!(s->control & IRQ_STORM_CTRL_ENABLE)) {
        return;
    }

    pulses = MIN(MAX(1U, s->burst), IRQ_STORM_MAX_BURST);
    for (i = 0; i < pulses; i++) {
        qemu_irq_pulse(s->irq);
    }

    s->pulses_emitted += pulses;
    irq_storm_schedule_next(s);
}

static uint64_t irq_storm_read(void *opaque, hwaddr addr, unsigned size)
{
    ISAIrqStormState *s = opaque;
    (void)size;

    switch (addr) {
    case IRQ_STORM_REG_CTRL:
        return s->control;
    case IRQ_STORM_REG_IRQ:
        return s->isairq;
    case IRQ_STORM_REG_BURST:
        return s->burst;
    case IRQ_STORM_REG_STATUS:
        return !!(s->control & IRQ_STORM_CTRL_ENABLE);
    case IRQ_STORM_REG_PERIOD_US:
        return s->period_us;
    case IRQ_STORM_REG_PULSES:
        return (uint32_t)s->pulses_emitted;
    default:
        return 0;
    }
}

static void irq_storm_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    ISAIrqStormState *s = opaque;
    bool was_enabled = !!(s->control & IRQ_STORM_CTRL_ENABLE);
    (void)size;

    switch (addr) {
    case IRQ_STORM_REG_CTRL:
        s->control = val & IRQ_STORM_CTRL_ENABLE;
        if (s->control & IRQ_STORM_CTRL_ENABLE) {
            if (!was_enabled) {
                irq_storm_schedule_next(s);
            }
        } else {
            timer_del(s->timer);
        }
        break;
    case IRQ_STORM_REG_BURST:
        s->burst = val;
        break;
    case IRQ_STORM_REG_PERIOD_US:
        s->period_us = val;
        if (s->control & IRQ_STORM_CTRL_ENABLE) {
            irq_storm_schedule_next(s);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps irq_storm_ops = {
    .read = irq_storm_read,
    .write = irq_storm_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void irq_storm_realize(DeviceState *dev, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(dev);
    ISAIrqStormState *s = ISA_IRQ_STORM_DEVICE(dev);

    if (s->isairq > 15) {
        error_setg(errp, "isa-irq-storm: irq must be in range [0..15]");
        return;
    }
    if (s->iosize < 0x0c) {
        error_setg(errp, "isa-irq-storm: iosize must be at least 0x0c");
        return;
    }

    s->irq = isa_get_irq(isadev, s->isairq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, irq_storm_timer_cb, s);

    memory_region_init_io(&s->io, OBJECT(dev), &irq_storm_ops, s,
                          TYPE_ISA_IRQ_STORM_DEVICE, s->iosize);
    memory_region_add_subregion(isa_address_space_io(isadev), s->iobase, &s->io);

    if (s->start_enabled) {
        s->control |= IRQ_STORM_CTRL_ENABLE;
        irq_storm_schedule_next(s);
    }
}

static void irq_storm_unrealize(DeviceState *dev)
{
    ISAIrqStormState *s = ISA_IRQ_STORM_DEVICE(dev);

    if (s->timer) {
        timer_del(s->timer);
        timer_free(s->timer);
        s->timer = NULL;
    }
}

static const Property irq_storm_properties[] = {
    DEFINE_PROP_UINT32("iobase", ISAIrqStormState, iobase, 0x560),
    DEFINE_PROP_UINT32("iosize", ISAIrqStormState, iosize, 0x10),
    DEFINE_PROP_UINT32("irq", ISAIrqStormState, isairq, 5),
    DEFINE_PROP_UINT32("burst", ISAIrqStormState, burst, 128),
    DEFINE_PROP_UINT32("period-us", ISAIrqStormState, period_us, 100),
    DEFINE_PROP_BOOL("start-enabled", ISAIrqStormState, start_enabled, true),
};

static void irq_storm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = irq_storm_realize;
    dc->unrealize = irq_storm_unrealize;
    device_class_set_props(dc, irq_storm_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo irq_storm_info = {
    .name          = TYPE_ISA_IRQ_STORM_DEVICE,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(ISAIrqStormState),
    .class_init    = irq_storm_class_init,
};

static void irq_storm_register_types(void)
{
    type_register_static(&irq_storm_info);
}

type_init(irq_storm_register_types)
