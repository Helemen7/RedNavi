// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_LIST_H
#define _LINUX_LIST_H

#include <linux/kernel.h>

struct list_head {
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list) { list->next = list; list->prev = list; }

static inline void __list_add(struct list_head *entry, struct list_head *prev, struct list_head *next) {
    next->prev = entry;
    entry->next = next;
    entry->prev = prev;
    prev->next = entry;
}
static inline void list_add(struct list_head *entry, struct list_head *head) { __list_add(entry, head, head->next); }
static inline void list_add_tail(struct list_head *entry, struct list_head *head) { __list_add(entry, head->prev, head); }
static inline void list_del(struct list_head *entry) {
    entry->prev->next = entry->next;
    entry->next->prev = entry->prev;
    entry->next = entry->prev = NULL;
}
static inline void list_del_init(struct list_head *entry) { list_del(entry); INIT_LIST_HEAD(entry); }
static inline void list_move(struct list_head *entry, struct list_head *head) { list_del(entry); list_add(entry, head); }
static inline void list_move_tail(struct list_head *entry, struct list_head *head) { list_del(entry); list_add_tail(entry, head); }
static inline int list_empty(const struct list_head *head) { return head->next == head; }

#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)
#define list_next_entry(pos, member) list_entry((pos)->member.next, __typeof__(*(pos)), member)
#define list_for_each(pos, head) for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_entry(pos, head, member) \
    for (pos = list_first_entry(head, __typeof__(*pos), member); &pos->member != (head); pos = list_next_entry(pos, member))
#define list_for_each_entry_safe(pos, n, head, member) \
    for (pos = list_first_entry(head, __typeof__(*pos), member), n = list_next_entry(pos, member); \
         &pos->member != (head); pos = n, n = list_next_entry(n, member))

#endif
