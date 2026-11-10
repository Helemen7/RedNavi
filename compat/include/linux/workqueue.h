// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_WORKQUEUE_H
#define _LINUX_WORKQUEUE_H

#include <linux/types.h>
#include <linux/jiffies.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);

// Each work item owns a thread_call, allocated on first use. Call rn_work_destroy before freeing a work item
// that was ever queued (Linux has no such call; its work items cost nothing when idle).
struct work_struct {
    work_func_t func;
    void *impl;
    volatile int pending;
    volatile int running;
};

struct delayed_work {
    struct work_struct work;
};

struct workqueue_struct {
    int unused;
};

#define INIT_WORK(w, f) rn_work_init((w), (f))
#define INIT_DELAYED_WORK(dw, f) rn_work_init(&(dw)->work, (f))
#define to_delayed_work(w) ((struct delayed_work *)(w))

void rn_work_init(struct work_struct *work, work_func_t func);
void rn_work_destroy(struct work_struct *work);

bool schedule_work(struct work_struct *work);
bool queue_work(struct workqueue_struct *wq, struct work_struct *work);
bool schedule_delayed_work(struct delayed_work *dwork, unsigned long delay);
bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay);
bool cancel_work_sync(struct work_struct *work);
bool cancel_delayed_work(struct delayed_work *dwork);
bool cancel_delayed_work_sync(struct delayed_work *dwork);
bool flush_work(struct work_struct *work);

// All workqueues share the system thread_call pool; the queue object only exists so ported code compiles.
struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...);
#define create_singlethread_workqueue(name) alloc_workqueue("%s", 0, 1, name)
#define create_workqueue(name) alloc_workqueue("%s", 0, 1, name)
void destroy_workqueue(struct workqueue_struct *wq);
void flush_workqueue(struct workqueue_struct *wq);

#ifdef __cplusplus
}
#endif

#endif
