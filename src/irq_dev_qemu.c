*
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
#define IRQ_STORM_REG_PULSES_LO  0x08
#define IRQ_STORM_REG_PULSES_HI  0x0c
#define IRQ_STORM_REG_TIMER_CB   0x10
#define IRQ_STORM_REG_CFG_WRITES 0x14
#define IRQ_STORM_REG_EN_TOGGLES 0x18
#define IRQ_STORM_REG_ACK        0x1c

#define IRQ_STORM_CTRL_ENABLE    BIT(0)
#define IRQ_STORM_CTRL_LEVEL     BIT(1)

#define IRQ_STORM_STATUS_ENABLED BIT(0)
#define IRQ_STORM_STATUS_ASSERT  BIT(1)
#define IRQ_STORM_STATUS_LEVEL   BIT(2)

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
    bool level_triggered;

    uint8_t control;
    bool irq_asserted;
    int64_t next_deadline_ns;
    uint64_t pulses_emitted;
    uint64_t timer_cb_count;
    uint64_t config_writes;
    uint64_t enable_toggle_count;
};

static uint64_t irq_storm_period_ns(ISAIrqStormState *s)
{
    return MAX(1U, s->period_us) * SCALE_US;
}

static void irq_storm_irq_deassert(ISAIrqStormState *s)
{
    if (s->irq_asserted) {
        qemu_irq_lower(s->irq);
        s->irq_asserted = false;
    }
}

static void irq_storm_schedule_from_now(ISAIrqStormState *s)
{
    int64_t now;

    if (!(s->control & IRQ_STORM_CTRL_ENABLE)) {
        return;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->next_deadline_ns = now + irq_storm_period_ns(s);
    timer_mod(s->timer, s->next_deadline_ns);
}

static void irq_storm_schedule_next(ISAIrqStormState *s)
{
    int64_t now;
    uint64_t period_ns;

    if (!(s->control & IRQ_STORM_CTRL_ENABLE)) {
        return;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    period_ns = irq_storm_period_ns(s);
    s->next_deadline_ns += period_ns;
    if (s->next_deadline_ns <= now) {
        uint64_t missed = ((uint64_t)(now - s->next_deadline_ns) / period_ns) + 1;
        s->next_deadline_ns += missed * period_ns;
    }
    timer_mod(s->timer, s->next_deadline_ns);
}

static void irq_storm_timer_cb(void *opaque)
{
    ISAIrqStormState *s = opaque;
    uint32_t i;
    uint32_t pulses;

    if (!(s->control & IRQ_STORM_CTRL_ENABLE)) {
        return;
    }

    s->timer_cb_count++;
    if (s->control & IRQ_STORM_CTRL_LEVEL) {
        if (!s->irq_asserted) {
            qemu_irq_raise(s->irq);
            s->irq_asserted = true;
            s->pulses_emitted++;
        }
    } else {
        pulses = MIN(MAX(1U, s->burst), IRQ_STORM_MAX_BURST);
        for (i = 0; i < pulses; i++) {
            qemu_irq_pulse(s->irq);
        }
        s->pulses_emitted += pulses;
    }
    irq_storm_schedule_next(s);
}

static uint64_t irq_storm_read(void *opaque, hwaddr addr, unsigned size)
{
    ISAIrqStormState *s = opaque;
    uint8_t status = 0;
    (void)size;

    switch (addr) {
    case IRQ_STORM_REG_CTRL:
        return s->control;
    case IRQ_STORM_REG_IRQ:
        return s->isairq;
    case IRQ_STORM_REG_BURST:
        return s->burst;
    case IRQ_STORM_REG_STATUS:
        if (s->control & IRQ_STORM_CTRL_ENABLE) {
            status |= IRQ_STORM_STATUS_ENABLED;
        }
        if (s->irq_asserted) {
            status |= IRQ_STORM_STATUS_ASSERT;
        }
        if (s->control & IRQ_STORM_CTRL_LEVEL) {
            status |= IRQ_STORM_STATUS_LEVEL;
        }
        return status;
    case IRQ_STORM_REG_PERIOD_US:
        return s->period_us;
    case IRQ_STORM_REG_PULSES_LO:
        return (uint32_t)s->pulses_emitted;
    case IRQ_STORM_REG_PULSES_HI:
        return (uint32_t)(s->pulses_emitted >> 32);
    case IRQ_STORM_REG_TIMER_CB:
        return (uint32_t)s->timer_cb_count;
    case IRQ_STORM_REG_CFG_WRITES:
        return (uint32_t)s->config_writes;
    case IRQ_STORM_REG_EN_TOGGLES:
        return (uint32_t)s->enable_toggle_count;
    default:
        return 0;
    }
}

static void irq_storm_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    ISAIrqStormState *s = opaque;
    uint8_t old_control = s->control;
    uint8_t new_control;
    bool was_level = !!(old_control & IRQ_STORM_CTRL_LEVEL);
    bool is_level;
    bool was_enabled = !!(old_control & IRQ_STORM_CTRL_ENABLE);
    bool is_enabled;
    (void)size;

    switch (addr) {
    case IRQ_STORM_REG_CTRL:
        new_control = val & (IRQ_STORM_CTRL_ENABLE | IRQ_STORM_CTRL_LEVEL);
        if (new_control != old_control) {
            s->config_writes++;
            s->control = new_control;
        }

        is_level = !!(s->control & IRQ_STORM_CTRL_LEVEL);
        if (was_level && !is_level) {
            irq_storm_irq_deassert(s);
        }

        is_enabled = !!(s->control & IRQ_STORM_CTRL_ENABLE);
        if (is_enabled != was_enabled) {
            s->enable_toggle_count++;
        }

        if (is_enabled) {
            if (!was_enabled) {
                irq_storm_schedule_from_now(s);
            }
        } else {
            timer_del(s->timer);
            irq_storm_irq_deassert(s);
        }
        break;
    case IRQ_STORM_REG_BURST:
        if ((uint32_t)val != s->burst) {
            s->burst = val;
            s->config_writes++;
        }
        break;
    case IRQ_STORM_REG_PERIOD_US:
        if ((uint32_t)val != s->period_us) {
            s->period_us = val;
            s->config_writes++;
            if (s->control & IRQ_STORM_CTRL_ENABLE) {
                irq_storm_schedule_from_now(s);
            }
        }
        break;
    case IRQ_STORM_REG_ACK:
        if (val) {
            irq_storm_irq_deassert(s);
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
    if (s->iosize < 0x20) {
        error_setg(errp, "isa-irq-storm: iosize must be at least 0x20");
        return;
    }

    s->irq = isa_get_irq(isadev, s->isairq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, irq_storm_timer_cb, s);

    memory_region_init_io(&s->io, OBJECT(dev), &irq_storm_ops, s,
                          TYPE_ISA_IRQ_STORM_DEVICE, s->iosize);
    memory_region_add_subregion(isa_address_space_io(isadev), s->iobase, &s->io);

    if (s->level_triggered) {
        s->control |= IRQ_STORM_CTRL_LEVEL;
    }
    if (s->start_enabled) {
        s->control |= IRQ_STORM_CTRL_ENABLE;
        irq_storm_schedule_from_now(s);
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
    DEFINE_PROP_UINT32("iosize", ISAIrqStormState, iosize, 0x20),
    DEFINE_PROP_UINT32("irq", ISAIrqStormState, isairq, 5),
    DEFINE_PROP_UINT32("burst", ISAIrqStormState, burst, 128),
    DEFINE_PROP_UINT32("period-us", ISAIrqStormState, period_us, 100),
    DEFINE_PROP_BOOL("start-enabled", ISAIrqStormState, start_enabled, true),
    DEFINE_PROP_BOOL("level-triggered", ISAIrqStormState, level_triggered,
                     false),
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
