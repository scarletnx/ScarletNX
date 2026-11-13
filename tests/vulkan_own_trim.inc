/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The kept results within the budget (owned.c vk_own_trim). With the device's memory short, a result that nothing
   wrote or read for 120 presents lands in guest memory, as the CPU's touch would land it: its pages are the CPU's
   again, with the result's bytes where it was and memory's around it. With memory to spare it stays, and so does one
   published lately, which lands once it has been left alone as long.

   Short is a budget of one byte, on any host. To spare is the host's to give: the use the trim reads is the driver's
   count of the heap, which on a driver that shares the system's memory (lavapipe) is every process's. That half is
   checked only where the count is under three quarters of the budget before the trim and after it (the trim acts
   at four fifths), and says the numbers where it is not */
static int own_trim_landed(u64 cpu, u8 value) {
    CHECK(own_lookup(cpu + 0x100, 256, NULL, 0) == 0);
    for (u32 i = 0; i < 0x1000; i++) CHECK(GP(cpu)[i] == (i >= 0x100 && i < 0x200 ? value : 0x11));
    return 0;
}
static int check_own_trim(void) {
    const u64 cpu = HEAP_BASE + 0x940000, gva = 0xae00000;
    if (!own_traps()) return 0;
    CHECK(mem_map(cpu, 0x3000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva, cpu, 0x3000);
    vk_lock();
    vk_finish();
    const u32 present = g_vk_present;
    VkOwnRes* r[3];
    memset(GP(cpu), 0x11, 0x3000);
    for (u32 i = 0; i < 3; i++) {
        CHECK((r[i] = vk_own_res_new(256, VK_OWN_BY_COPY)) != NULL);
        CHECK(!owned_test_fill(r[i]->buf, 0, 256, (u8)(0x41 + i)));
    }
    /* a page each: a landing takes whole pages. vk_own_trim looks every fourth present */
    g_vk_present = (present + 8) & ~3u;
    vk_own_publish_owned(cpu + 0x100, 256, r[0], 0);
    vk_own_publish_owned(cpu + 0x2100, 256, r[2], 0);
    g_vk_present += 124;
    vk_own_publish_owned(cpu + 0x1100, 256, r[1], 0); /* this one lately */
    vk_finish();                                      /* their writes are done: none is still being recorded */
    for (u32 i = 0; i < 3; i++) CHECK(own_lookup(cpu + i * 0x1000 + 0x100, 256, NULL, 0) == 1);
    u64 usage, budget, usage_after, budget_after;
    vk_mem_use(&usage, &budget);
    vk_own_trim(); /* memory to spare: nothing lands */
    vk_mem_use(&usage_after, &budget_after);
    const int spare = usage < budget / 4 * 3 && usage_after < budget_after / 4 * 3;
    if (spare)
        for (u32 i = 0; i < 3; i++) CHECK(own_lookup(cpu + i * 0x1000 + 0x100, 256, NULL, 0) == 1);
    else
        printf("own trim: memory to spare is not checked here: %llu of %llu MiB budgeted in use before the trim and "
               "%llu of %llu after it, where the check needs under three quarters\n",
               (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20),
               (unsigned long long)(usage_after >> 20), (unsigned long long)(budget_after >> 20));
    const u64 cap = vk_mem_budget_cap(1); /* a budget of one byte: short */
    /* a call lands the 16 left alone longest, and other checks' results may be among them */
    for (u32 i = 0; i < 64 && (own_lookup(cpu + 0x100, 256, NULL, 0) || own_lookup(cpu + 0x2100, 256, NULL, 0)); i++)
        vk_own_trim();
    int failed = own_trim_landed(cpu, 0x41) || own_trim_landed(cpu + 0x2000, 0x43) ||
                 own_lookup(cpu + 0x1100, 256, NULL, 0) != 1;
    if (!failed) {
        g_vk_present += 124;
        for (u32 i = 0; i < 64 && own_lookup(cpu + 0x1100, 256, NULL, 0); i++) vk_own_trim();
        failed = own_trim_landed(cpu + 0x1000, 0x42);
    }
    vk_mem_budget_cap(cap);
    g_vk_present = present;
    for (u32 i = 0; i < 3; i++) vk_own_res_unref(r[i]);
    vk_finish();
    vk_unlock();
    CHECK(!failed);
    printf("own trim: with memory short the kept results left alone for 120 presents land in guest memory, the one "
           "published lately after them%s\n",
           spare ? "; with memory to spare none does" : "");
    return 0;
}
