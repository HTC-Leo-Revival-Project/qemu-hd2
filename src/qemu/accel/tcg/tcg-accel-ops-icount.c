/*
 * QEMU TCG Single Threaded vCPUs implementation using instruction counting
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2014 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "system/replay.h"
#include "exec/icount.h"
#include "qemu/main-loop.h"
#include "qemu/guest-random.h"
#include "hw/core/cpu.h"

#include "tcg-accel-ops.h"
#include "tcg-accel-ops-icount.h"
#include "tcg-accel-ops-rr.h"
#include "qemu/timer.h"

/*
 * Cost clock (icount_cost_active): a vCPU's budget is still counted in
 * instructions, so the time to the next deadline is turned into
 * instructions with the cost per instruction each vCPU has been running
 * at.  A slice is capped so a run of expensive instructions cannot
 * overshoot a timer by much.
 */
#define COST_CPUS       16
#define COST_SLICE_NS   (200 * SCALE_US)
#define COST_SLICE_MAX  2000000

bool icount_cost_pace;
#define COST_SLICE_MIN  2000
#define COST_SAMPLE     20000           /* instructions per ratio update */
#define COST_RATIO_MAX  64.0

static double cost_ratio[COST_CPUS];    /* cost units per instruction */
static uint64_t cost_slice0[COST_CPUS];
static uint64_t cost_acc_units[COST_CPUS], cost_acc_insns[COST_CPUS];
static int64_t cost_insns0[COST_CPUS];
static int64_t cost_pace_offset = INT64_MIN;

static int64_t icount_cost_limit(CPUState *cpu, int64_t deadline)
{
    int i = cpu->cpu_index % COST_CPUS;
    double r = cost_ratio[i] ? cost_ratio[i] : icount_cost_units_per_ns;
    int64_t n;

    deadline = MIN(deadline, COST_SLICE_NS);
    if (cpu->cpu_index == icount_cost_skip) {
        /* a core of its own on the real machine: its instruction rate */
        n = deadline * icount_cost_skip_ipns;
        return MAX(MIN(n, COST_SLICE_MAX), deadline ? 1 : 0);
    }
    n = deadline * (double)icount_cost_units_per_ns / r;
    return deadline ? MAX(MIN(n, COST_SLICE_MAX), COST_SLICE_MIN) : 0;
}

/* the slice ran: learn its cost per instruction, and keep to real time */
static void icount_cost_account(CPUState *cpu, int64_t executed)
{
    int i = cpu->cpu_index % COST_CPUS;
    uint64_t dc = icount_cost_cpu(cpu->cpu_index) - cost_slice0[i];

    /* learn over enough instructions, however short the slices */
    cost_acc_units[i] += dc;
    cost_acc_insns[i] += executed;
    if (cost_acc_insns[i] >= COST_SAMPLE) {
        double r = (double)cost_acc_units[i] / cost_acc_insns[i];

        r = MIN(MAX(r, 0.05), COST_RATIO_MAX);
        cost_ratio[i] = cost_ratio[i] ? 0.5 * cost_ratio[i] + 0.5 * r : r;
        cost_acc_units[i] = cost_acc_insns[i] = 0;
    }
    if (icount_cost_pace) {
        int64_t vt = icount_get();
        int64_t rt = get_clock();

        /* never run ahead of real time; never rush to make up for lag */
        if (cost_pace_offset == INT64_MIN || rt > vt + cost_pace_offset +
                                                    100 * SCALE_MS) {
            cost_pace_offset = rt - vt;
        } else if (vt + cost_pace_offset > rt + SCALE_MS) {
            g_usleep((vt + cost_pace_offset - rt) / SCALE_US);
        }
    }
}


static int64_t icount_get_limit(void)
{
    int64_t deadline;

    if (replay_mode != REPLAY_MODE_PLAY) {
        /*
         * Include all the timers, because they may need an attention.
         * Too long CPU execution may create unnecessary delay in UI.
         */
        deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                              QEMU_TIMER_ATTR_ALL);
        /* Check realtime timers, because they help with input processing */
        deadline = qemu_soonest_timeout(deadline,
                qemu_clock_deadline_ns_all(QEMU_CLOCK_REALTIME,
                                           QEMU_TIMER_ATTR_ALL));

        /*
         * Maintain prior (possibly buggy) behaviour where if no deadline
         * was set (as there is no QEMU_CLOCK_VIRTUAL timer) or it is more than
         * INT32_MAX nanoseconds ahead, we still use INT32_MAX
         * nanoseconds.
         */
        if ((deadline < 0) || (deadline > INT32_MAX)) {
            deadline = INT32_MAX;
        }

        return icount_round(deadline);
    } else {
        return replay_get_instructions();
    }
}

static void icount_notify_aio_contexts(void)
{
    /* Wake up other AioContexts.  */
    qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
}

void icount_handle_deadline(void)
{
    assert(qemu_in_vcpu_thread());
    int64_t deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                                  QEMU_TIMER_ATTR_ALL);

    /*
     * Instructions, interrupts, and exceptions are processed in cpu-exec.
     * Don't interrupt cpu thread, when these events are waiting
     * (i.e., there is no checkpoint)
     */
    if (deadline == 0) {
        icount_notify_aio_contexts();
    }
}

/* Distribute the budget evenly across all CPUs */
int64_t icount_percpu_budget(int cpu_count)
{
    int64_t limit = icount_get_limit();
    int64_t timeslice = limit / cpu_count;

    if (timeslice == 0) {
        timeslice = limit;
    }
    if (icount_rr_slice && timeslice > icount_rr_slice) {
        timeslice = icount_rr_slice;
    }

    return timeslice;
}

void icount_prepare_for_run(CPUState *cpu, int64_t cpu_budget)
{
    int insns_left;

    /*
     * These should always be cleared by icount_process_data after
     * each vCPU execution. However u16.high can be raised
     * asynchronously by cpu_exit/cpu_interrupt/tcg_handle_interrupt
     */
    g_assert(cpu->neg.icount_decr.u16.low == 0);
    g_assert(cpu->icount_extra == 0);

    replay_mutex_lock();

    icount_cost_sync(cpu);
    cpu->icount_budget = MIN(icount_get_limit(), cpu_budget);
    if (icount_cost_active) {
        icount_cost_slice_start(cpu);
        cost_slice0[cpu->cpu_index % COST_CPUS] =
            icount_cost_cpu(cpu->cpu_index);
        cost_insns0[cpu->cpu_index % COST_CPUS] =
            icount_cpu_insns(cpu->cpu_index);
        cpu->icount_budget = icount_cost_limit(cpu,
                                               icount_to_ns(cpu->icount_budget));
    }
    insns_left = MIN(0xffff, cpu->icount_budget);
    cpu->neg.icount_decr.u16.low = insns_left;
    cpu->icount_extra = cpu->icount_budget - insns_left;

    if (cpu->icount_budget == 0) {
        /*
         * We're called without the BQL, so must take it while
         * we're calling timer handlers.
         */
        bql_lock();
        icount_notify_aio_contexts();
        bql_unlock();
    }
}

void icount_process_data(CPUState *cpu)
{
    /* Account for executed instructions */
    icount_update(cpu);
    if (icount_cost_active) {
        icount_cost_slice_end(cpu);
        /* reading the clock in the slice accounted some of them already */
        icount_cost_account(cpu, icount_cpu_insns(cpu->cpu_index) -
                                 cost_insns0[cpu->cpu_index % COST_CPUS]);
    }

    /* Reset the counters */
    cpu->neg.icount_decr.u16.low = 0;
    cpu->icount_extra = 0;
    cpu->icount_budget = 0;

    replay_account_executed_instructions();

    replay_mutex_unlock();
}

void icount_handle_interrupt(CPUState *cpu, int mask)
{
    int old_mask = cpu->interrupt_request;

    tcg_handle_interrupt(cpu, mask);
    if (qemu_cpu_is_self(cpu) &&
        !cpu->neg.can_do_io
        && (mask & ~old_mask) != 0) {
        cpu_abort(cpu, "Raised interrupt while not in I/O function");
    }
}
