/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A write log in the GPU's memory whose clear is recorded (writelog.c vk_wlog_ready) is the one the next dispatches
   in the mirror take (vk_wlog_get_gpu), and no log is made for them. One whose clear is not recorded is not given
   again, and a log of another count of parts is another log. The clear log that no dispatch took goes back to the
   pool after 600 presents, and after 60 with the device's memory short */
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
    g_vk_present += 61;
    vk_wlog_trim();
    c = vk_wlog_get_gpu(2);
    failed |= c != a || vk_wlog_again() != again + 3;
    vk_wlog_unref(c);
    vk_finish();
    /* and with memory short it went back to the pool: the next log is not a clear one given again */
    const u64 cap = vk_mem_budget_cap(1);
    g_vk_present += 61;
    vk_wlog_trim();
    vk_mem_budget_cap(cap);
    c = vk_wlog_get_gpu(2);
    failed |= !c || vk_wlog_again() != again + 3;
    vk_wlog_unref(c);
    g_vk_present = present;
    vk_finish();
    vk_unlock();
    CHECK(!failed);
    puts("write logs: the log a dispatch in the mirror cleared is the next dispatches', a log not cleared yet or of "
         "another size is not; idle, it goes back to the pool after 60 presents with memory short");
    return 0;
}
