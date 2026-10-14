/*
 * QEMU Plugin API - System specific implementations
 *
 * This provides the APIs that have a specific system implementation
 * or are only relevant to system-mode.
 *
 * Copyright (C) 2017, Emilio G. Cota <cota@braap.org>
 * Copyright (C) 2019-2025, Linaro
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "migration/blocker.h"
#include "hw/core/boards.h"
#include "qemu/plugin-memory.h"
#include "qemu/plugin.h"
#include "exec/icount.h"
#include "system/cpu-timers.h"

/*
 * In system mode we cannot trace the binary being executed so the
 * helpers all return NULL/0.
 */
const char *qemu_plugin_path_to_binary(void)
{
    return NULL;
}

uint64_t qemu_plugin_start_code(void)
{
    return 0;
}

uint64_t qemu_plugin_end_code(void)
{
    return 0;
}

uint64_t qemu_plugin_entry_code(void)
{
    return 0;
}

/*
 * Virtual Memory queries
 */

static __thread struct qemu_plugin_hwaddr hwaddr_info;

struct qemu_plugin_hwaddr *qemu_plugin_get_hwaddr(qemu_plugin_meminfo_t info,
                                                  uint64_t vaddr)
{
    CPUState *cpu = current_cpu;
    unsigned int mmu_idx = get_mmuidx(info);
    enum qemu_plugin_mem_rw rw = get_plugin_meminfo_rw(info);
    hwaddr_info.is_store = (rw & QEMU_PLUGIN_MEM_W) != 0;

    assert(mmu_idx < NB_MMU_MODES);

    if (!tlb_plugin_lookup(cpu, vaddr, mmu_idx,
                           hwaddr_info.is_store, &hwaddr_info)) {
        error_report("invalid use of qemu_plugin_get_hwaddr");
        return NULL;
    }

    return &hwaddr_info;
}

bool qemu_plugin_hwaddr_is_io(const struct qemu_plugin_hwaddr *haddr)
{
    return haddr->is_io;
}

uint64_t qemu_plugin_hwaddr_phys_addr(const struct qemu_plugin_hwaddr *haddr)
{
    if (haddr) {
        return haddr->phys_addr;
    }
    return 0;
}

uint8_t qemu_plugin_hwaddr_memattr(const struct qemu_plugin_hwaddr *haddr)
{
    return haddr && !haddr->is_io ? haddr->memattr : 0;
}

/* DMA snooping (qemu_plugin_register_dma_snoop) */
bool (*plugin_dma_snoop)(uint64_t addr, void *buf, uint64_t len,
                         bool is_write);

void qemu_plugin_register_dma_snoop(qemu_plugin_dma_snoop_cb_t cb)
{
    plugin_dma_snoop = cb;
}

const char *qemu_plugin_hwaddr_device_name(const struct qemu_plugin_hwaddr *h)
{
    if (h && h->is_io) {
        MemoryRegion *mr = h->mr;
        if (!mr->name) {
            unsigned maddr = (uintptr_t)mr;
            g_autofree char *temp = g_strdup_printf("anon%08x", maddr);
            return g_intern_string(temp);
        } else {
            return g_intern_string(mr->name);
        }
    } else {
        return g_intern_static_string("RAM");
    }
}

/*
 * Time control
 */
static bool has_control;
static Error *migration_blocker;

const void *qemu_plugin_request_time_control(void)
{
    if (!has_control) {
        has_control = true;
        error_setg(&migration_blocker,
                   "TCG plugin time control does not support migration");
        migrate_add_blocker(&migration_blocker, &error_fatal);
        return &has_control;
    }
    return NULL;
}

static void advance_virtual_time__async(CPUState *cpu, run_on_cpu_data data)
{
    int64_t new_time = data.host_ulong;
    qemu_clock_advance_virtual_time(new_time);
}

static qemu_plugin_u64 cost_counter;

static uint64_t cost_cpu(int cpu_index)
{
    /* a vCPU the plugins have not seen yet has no costs */
    return cpu_index < qemu_plugin_num_vcpus() ?
           qemu_plugin_u64_get(cost_counter, cpu_index) : 0;
}

bool qemu_plugin_icount_cost(qemu_plugin_u64 counter, unsigned units_per_ns,
                             bool pace, int skip_vcpu, unsigned skip_mips)
{
    if (!icount_enabled() || icount_cost_cpu || !units_per_ns) {
        return false;
    }
    cost_counter = counter;
    icount_cost_skip = skip_vcpu;
    icount_cost_skip_ipns = MAX(skip_mips, 1) / 1000.0;
    icount_cost_units_per_ns = units_per_ns;
    icount_cost_pace = pace;
    icount_cost_cpu = cost_cpu;
    icount_cost_request(true);
    return true;
}

void qemu_plugin_icount_cost_enable(bool on)
{
    icount_cost_request(on);
}

void qemu_plugin_update_ns(const void *handle, int64_t new_time)
{
    if (handle == &has_control) {
        /* Need to execute out of cpu_exec, so bql can be locked. */
        async_run_on_cpu(current_cpu,
                         advance_virtual_time__async,
                         RUN_ON_CPU_HOST_ULONG(new_time));
    }
}
