/*
 * icount - Instruction Counter API
 * CPU timers state API
 *
 * Copyright 2020 SUSE LLC
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef EXEC_ICOUNT_H
#define EXEC_ICOUNT_H

/**
 * ICountMode: icount enablement state:
 *
 * @ICOUNT_DISABLED: Disabled - Do not count executed instructions.
 * @ICOUNT_PRECISE: Enabled - Fixed conversion of insn to ns via "shift" option
 * @ICOUNT_ADAPTATIVE: Enabled - Runtime adaptive algorithm to compute shift
 */
typedef enum {
    ICOUNT_DISABLED = 0,
    ICOUNT_PRECISE,
    ICOUNT_ADAPTATIVE,
} ICountMode;

#ifdef CONFIG_TCG
extern ICountMode use_icount;
/*
 * With icount and several vCPUs on one thread: the most instructions a
 * vCPU runs before the next one gets its turn (0: no limit).  Boards
 * whose CPUs are hardware threads of one core set this small so that
 * they interleave the way the hardware does.
 */
extern int64_t icount_rr_slice;
#define icount_enabled() (use_icount)
#else
#define icount_enabled() ICOUNT_DISABLED
#endif

/* Protect the CONFIG_USER_ONLY test vs poisoning. */
#if defined(COMPILING_PER_TARGET) || defined(COMPILING_SYSTEM_VS_USER)
# ifdef CONFIG_USER_ONLY
#  undef  icount_enabled
#  define icount_enabled() ICOUNT_DISABLED
# endif
#endif

/*
 * Update the icount with the executed instructions. Called by
 * cpus-tcg vCPU thread so the main-loop can see time has moved forward.
 */
void icount_update(CPUState *cpu);

/* get raw icount value */
int64_t icount_get_raw(void);

/* return the virtual CPU time in ns, based on the instruction counter. */
int64_t icount_get(void);
/*
 * convert an instruction counter value to ns, based on the icount shift.
 * This shift is set as a fixed value with the icount "shift" option
 * (precise mode), or it is constantly approximated and corrected at
 * runtime in adaptive mode.
 */
int64_t icount_to_ns(int64_t icount);

/**
 * icount_configure: configure the icount options, including "shift"
 * @opts: Options to parse
 * @errp: pointer to a NULL-initialized error object
 *
 * Return: true on success, else false setting @errp with error
 */
bool icount_configure(QemuOpts *opts, Error **errp);

/*
 * Cost clock: a plugin that models instruction timing keeps per-vCPU cost
 * counters (in 1/units_per_ns ns) and virtual time follows the main core's
 * costs (plus idle time) instead of the instruction count.  See
 * icount_cost_skip for the other core.  icount_cost_cpu is NULL when no
 * plugin does this.
 */
extern uint64_t (*icount_cost_cpu)(int cpu_index);
extern unsigned icount_cost_units_per_ns;
/*
 * Whether virtual time follows the costs now; a request is applied between
 * slices, without a jump in virtual time.
 */
extern bool icount_cost_active;
void icount_cost_request(bool on);
void icount_cost_sync(CPUState *cpu);
/* a vCPU starts and ends a slice of execution */
void icount_cost_slice_start(CPUState *cpu);
void icount_cost_slice_end(CPUState *cpu);
/*
 * A vCPU that runs in parallel on the real machine (cost clock): it gets
 * icount_cost_skip_ipns instructions per ns of virtual time, and moves time
 * only while the main core sleeps.
 */
extern int icount_cost_skip;
extern double icount_cost_skip_ipns;
/* instructions a vCPU has executed so far */
int64_t icount_cpu_insns(int cpu_index);
/* keep the cost clock from running ahead of real time */
extern bool icount_cost_pace;

/* used by tcg vcpu thread to calc icount budget */
int64_t icount_round(int64_t count);

/* if the CPUs are idle, start accounting real time to virtual clock. */
void icount_start_warp_timer(void);
void icount_account_warp_timer(void);
void icount_notify_exit(void);

#endif /* EXEC_ICOUNT_H */
