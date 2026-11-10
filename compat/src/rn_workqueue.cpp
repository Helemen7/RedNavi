// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Work items on top of thread_call. Linux runs work in a kernel thread that may sleep; a thread_call callback
// is a kernel thread too, so the same code can block.
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <linux/workqueue.h>

namespace {

void runWork(thread_call_param_t param0, thread_call_param_t) {
    work_struct *w = static_cast<work_struct *>(param0);
    __atomic_store_n(&w->running, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&w->pending, 0, __ATOMIC_SEQ_CST);     // cleared before the function runs, as in Linux
    if (w->func) w->func(w);
    __atomic_store_n(&w->running, 0, __ATOMIC_SEQ_CST);
}

thread_call_t callFor(work_struct *w) {
    if (!w->impl) w->impl = thread_call_allocate(runWork, w);
    return static_cast<thread_call_t>(w->impl);
}

bool enqueue(work_struct *w, unsigned long delayMs) {
    thread_call_t c = callFor(w);
    if (!c) return false;
    if (__atomic_exchange_n(&w->pending, 1, __ATOMIC_SEQ_CST)) return false;     // already queued
    if (delayMs == 0) {
        thread_call_enter(c);
    } else {
        uint64_t deadline = 0;
        clock_interval_to_deadline(static_cast<uint32_t>(delayMs), 1000000, &deadline);     // 1 ms = 1,000,000 ns
        thread_call_enter_delayed(c, deadline);
    }
    return true;
}

bool cancel(work_struct *w) {
    bool wasPending = false;
    if (w->impl) {
        wasPending = thread_call_cancel_wait(static_cast<thread_call_t>(w->impl));
        if (wasPending) __atomic_store_n(&w->pending, 0, __ATOMIC_SEQ_CST);
    }
    while (__atomic_load_n(&w->running, __ATOMIC_SEQ_CST)) rn_yield();
    return wasPending;
}

}

extern "C" {

void rn_work_init(work_struct *work, work_func_t func) {
    work->func = func;
    work->impl = nullptr;
    work->pending = 0;
    work->running = 0;
}

void rn_work_destroy(work_struct *work) {
    cancel(work);
    if (work->impl) { thread_call_free(static_cast<thread_call_t>(work->impl)); work->impl = nullptr; }
}

bool schedule_work(work_struct *work) { return enqueue(work, 0); }
bool queue_work(workqueue_struct *, work_struct *work) { return enqueue(work, 0); }
bool schedule_delayed_work(delayed_work *dwork, unsigned long delay) { return enqueue(&dwork->work, delay); }
bool queue_delayed_work(workqueue_struct *, delayed_work *dwork, unsigned long delay) { return enqueue(&dwork->work, delay); }
bool cancel_work_sync(work_struct *work) { return cancel(work); }
bool cancel_delayed_work(delayed_work *dwork) { return cancel(&dwork->work); }
bool cancel_delayed_work_sync(delayed_work *dwork) { return cancel(&dwork->work); }

bool flush_work(work_struct *work) {
    bool waited = false;
    while (__atomic_load_n(&work->pending, __ATOMIC_SEQ_CST) || __atomic_load_n(&work->running, __ATOMIC_SEQ_CST)) {
        waited = true;
        rn_yield();
    }
    return waited;
}

workqueue_struct *alloc_workqueue(const char *, unsigned int, int, ...) {
    static workqueue_struct shared;
    return &shared;
}
void destroy_workqueue(workqueue_struct *) { }
void flush_workqueue(workqueue_struct *) { }

}
