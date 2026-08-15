// RP2040 implementation of run_split: core 0 rasterizes the top band while
// core 1 rasterizes the bottom band.
//
// Why this is safe with the 32blit pico backend on the PicoSystem: the SDK
// only claims core 1 when built with ENABLE_CORE1, which its CMake sets for
// scanvideo display boards alone. The PicoSystem uses the dbi display driver
// and runs audio from a hardware alarm on core 0, so core 1 is idle and ours.
//
// The handoff is a pair of volatile sequence counters in SRAM, polled, with
// no use of the inter core FIFO at all. That is deliberate twice over:
//
//   1. multicore_lockout_victim_init() installs an IRQ handler that owns the
//      core 1 FIFO and DISCARDS every word that is not the lockout magic. A
//      FIFO doorbell gets eaten by that handler and both cores deadlock on
//      the first frame. So the FIFO is simply never used here.
//   2. The idle loop must be flash safe (below), and a plain volatile SRAM
//      poll compiles to loads and branches with no calls, at any optimization
//      level. SDK FIFO helpers are static inline and can end up as flash
//      resident functions in a -O0 build, which would break that guarantee.
//
// There is also no multicore_lockout_victim_init() here at all: the 32blit
// storage driver only initiates a lockout when the SDK itself launched core 1
// (its own core1_started flag), which never happens in this configuration.
// Flash safety rests on the documented contract instead:
//
//   write_save disables XIP while it programs flash, and any core running
//   flash code at that moment reads garbage. The worker's idle loop is pinned
//   to RAM with __not_in_flash_func and touches only SRAM, so core 1 is
//   immune while parked there. run_split is the only time core 1 executes
//   flash code, and core 0 is busy rasterizing for all of it, so nothing can
//   possibly save mid frame. Games therefore save between frames, never
//   during rendering, and that is rule material in CLAUDE.md.
//
// Thread safety: the bands are disjoint rows of the framebuffer and the depth
// buffer, so the two cores never write the same memory and no locks exist.
// The counter updates are wrapped in __sync_synchronize, which is a real
// hardware barrier and not merely a compiler one: core 0 fills the job, then
// fences, then bumps the sequence; core 1 sees the sequence, fences, then
// reads the job. That is an acquire/release pair, so it is correct on the
// RP2350's Cortex-M33 as well as on the in-order M0+.

#include "pse/parallel.hpp"

#include "pico/multicore.h"
#include "pico/platform.h"

namespace pse {
namespace {

struct Job {
    Rasterizer* rasterizer;
    const FrameQueue* queue;
    SkyGradient sky;
    int row_begin;
    int row_end;
    int tri_begin;
    int tri_end;
    int gradient_begin;
    int gradient_end;
};

Job g_job;
volatile uint32_t g_job_seq = 0;    // bumped by core 0 to publish a job
volatile uint32_t g_done_seq = 0;   // set by core 1 when that job is finished
bool g_core1_launched = false;

// Ordinary flash resident code: only ever runs while core 0 is inside
// run_split, and nothing writes flash from there.
void worker_execute() {
    render_band(*g_job.rasterizer, *g_job.queue, g_job.sky,
                g_job.row_begin, g_job.row_end,
                g_job.tri_begin, g_job.tri_end,
                g_job.gradient_begin, g_job.gradient_end);
}

// The loop core 1 lives in between frames. RAM resident, and while waiting it
// executes nothing but SRAM loads and branches, so a flash write on core 0
// cannot hurt it.
void __not_in_flash_func(worker_entry)() {
    uint32_t seen = 0;
    while (true) {
        while (g_job_seq == seen) {
            // Plain poll on an uncached SRAM word. Nothing to yield to.
        }
        seen = g_job_seq;
        __sync_synchronize();

        worker_execute();

        __sync_synchronize();
        g_done_seq = seen;
    }
}

}  // namespace

void run_split(Rasterizer& rasterizer, const FrameQueue& queue,
               const SkyGradient& sky_top, const SkyGradient& sky_bottom) {
    const SplitPlan plan = plan_split(rasterizer, queue);

    if (!g_core1_launched) {
        multicore_launch_core1(worker_entry);
        g_core1_launched = true;
    }

    g_job.rasterizer = &rasterizer;
    g_job.queue = &queue;
    g_job.sky = sky_bottom;
    g_job.row_begin = plan.mid;
    g_job.row_end = plan.height;
    g_job.tri_begin = plan.bottom_tri_begin;
    g_job.tri_end = plan.bottom_tri_end;
    g_job.gradient_begin = plan.bottom_grad_begin;
    g_job.gradient_end = plan.height;

    // The barrier orders the job stores before the sequence bump that
    // publishes them.
    __sync_synchronize();
    const uint32_t seq = g_job_seq + 1;
    g_job_seq = seq;

    render_band(rasterizer, queue, sky_top, 0, plan.mid,
                plan.top_tri_begin, plan.top_tri_end, 0, plan.top_grad_end);

    while (g_done_seq != seq) {
        // Core 1 is finishing its band.
    }
    __sync_synchronize();
}

void run_split(Rasterizer& rasterizer, const FrameQueue& queue,
               const SkyGradient& sky) {
    run_split(rasterizer, queue, sky, sky);
}

}  // namespace pse
