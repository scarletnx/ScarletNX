/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A write log in the GPU's memory whose clear is recorded (writelog.c vk_wlog_ready) is the one the next dispatches
   in the mirror take (vk_wlog_get_gpu), and no log is made for them. One whose clear is not recorded is not given
   again, and a log of another count of parts is another log. The clear log that no dispatch took goes back to the
   pool after 600 presents, and after 60 with the device's memory short.

   Short is a budget of one byte, on any host. To spare is the host's to give: the use the trim reads is the driver's
   count of the heap, which on a driver that shares the system's memory (lavapipe) is every process's. That half is
   checked only where the count is under three quarters of the budget before the trim and after it (the trim takes
   the short wait at four fifths), and says the numbers where it is not */
static int check_wlog_again(void) {
    vk_lock();
    vk_finish();
    const u32 present = g_vk_present;
    const u64 again = vk_wlog_again();
    u64 logs0, logs, bytes, in_use, windows;
    VkWriteLog* a = vk_wlog_get_gpu(2);
    VkWriteLog* b = vk_wlog_get_gpu(2); /* a's clear is not recorded: another log */
    CHECK(a && b && a != b && vk_wlog_on_gpu(a) && vk_wlog_on_gpu(b) && vk_wlog_again() == again);
    vk_wlog_stats(&logs0, &bytes, &in_use, &windows);
    vk_wlog_clear(vk_cmd(), a);
    vk_wlog_ready(a);
    VkWriteLog* c = vk_wlog_get_gpu(2);
    VkWriteLog* d = vk_wlog_get_gpu(2);
    VkWriteLog* e = vk_wlog_get_gpu(3); /* three parts: a log of its own */
    vk_wlog_stats(&logs, &bytes, &in_use, &windows);
    int failed = c != a || d != a || !e || e == a || vk_wlog_again() != again + 2 || logs != logs0 + 1;
    vk_wlog_unref(c);
    vk_wlog_unref(d);
    vk_wlog_unref(a);
    vk_wlog_unref(b);
    vk_wlog_unref(e);
    vk_finish(); /* what was recorded with them is done: the pool may let them go */
    /* idle for 61 presents with memory to spare: still the one given */
    u64 usage, budget, usage_after, budget_after;
    vk_mem_use(&usage, &budget);
    g_vk_present += 61;
    vk_wlog_trim();
    vk_mem_use(&usage_after, &budget_after);
    const int spare = usage < budget / 4 * 3 && usage_after < budget_after / 4 * 3;
    c = vk_wlog_get_gpu(2);
    CHECK(c);
    if (spare)
        failed |= c != a || vk_wlog_again() != again + 3;
    else
        printf("write logs: memory to spare is not checked here: %llu of %llu MiB budgeted in use before the trim and "
               "%llu of %llu after it, where the check needs under three quarters\n",
               (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20),
               (unsigned long long)(usage_after >> 20), (unsigned long long)(budget_after >> 20));
    /* c is the clear log from here on, whatever that trim did with the one before: cleared, and given again */
    vk_wlog_clear(vk_cmd(), c);
    vk_wlog_ready(c);
    vk_wlog_unref(c);
    vk_finish();
    d = vk_wlog_get_gpu(2);
    const u64 given = vk_wlog_again();
    failed |= d != c;
    vk_wlog_unref(d);
    vk_finish();
    /* and with memory short it went back to the pool: the next log is not a clear one given again */
    const u64 cap = vk_mem_budget_cap(1);
    g_vk_present += 61;
    vk_wlog_trim();
    vk_mem_budget_cap(cap);
    e = vk_wlog_get_gpu(2);
    failed |= !e || vk_wlog_again() != given;
    vk_wlog_unref(e);
    g_vk_present = present;
    vk_finish();
    vk_unlock();
    CHECK(!failed);
    printf("write logs: the log a dispatch in the mirror cleared is the next dispatches', a log not cleared yet or of "
           "another size is not; idle, it goes back to the pool after 60 presents with memory short%s\n",
           spare ? ", and stays with memory to spare" : "");
    return 0;
}
