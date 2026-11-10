// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_ATOMIC_H
#define _LINUX_ATOMIC_H

#include <linux/types.h>

typedef struct { int counter; } atomic_t;
typedef struct { s64 counter; } atomic64_t;

#define ATOMIC_INIT(i) { (i) }

static inline int atomic_read(const atomic_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_SEQ_CST); }
static inline void atomic_set(atomic_t *v, int i) { __atomic_store_n(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_add(int i, atomic_t *v) { __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_sub(int i, atomic_t *v) { __atomic_fetch_sub(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_inc(atomic_t *v) { atomic_add(1, v); }
static inline void atomic_dec(atomic_t *v) { atomic_sub(1, v); }
static inline int atomic_add_return(int i, atomic_t *v) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int atomic_sub_return(int i, atomic_t *v) { return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int atomic_inc_return(atomic_t *v) { return atomic_add_return(1, v); }
static inline int atomic_dec_return(atomic_t *v) { return atomic_sub_return(1, v); }
static inline int atomic_dec_and_test(atomic_t *v) { return atomic_dec_return(v) == 0; }
static inline int atomic_cmpxchg(atomic_t *v, int old, int new_) {
    __atomic_compare_exchange_n(&v->counter, &old, new_, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return old;
}
static inline int atomic_xchg(atomic_t *v, int new_) { return __atomic_exchange_n(&v->counter, new_, __ATOMIC_SEQ_CST); }

static inline s64 atomic64_read(const atomic64_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_SEQ_CST); }
static inline void atomic64_set(atomic64_t *v, s64 i) { __atomic_store_n(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_inc_return(atomic64_t *v) { return __atomic_add_fetch(&v->counter, 1, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_add_return(s64 i, atomic64_t *v) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }

struct kref {
    atomic_t refcount;
};
static inline void kref_init(struct kref *k) { atomic_set(&k->refcount, 1); }
static inline void kref_get(struct kref *k) { atomic_inc(&k->refcount); }
// Returns 1 and calls release when the last reference goes away.
static inline int kref_put(struct kref *k, void (*release)(struct kref *)) {
    if (atomic_dec_and_test(&k->refcount)) { release(k); return 1; }
    return 0;
}

#endif
