/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vk_gpu_profile and vk_checkpoints in a console's renderer, the Switch's settings, on its submission ring (ring.h):
   the GPU's time in each part of a submission (Vulkan timestamps), the whole submission one part named after the
   renderer's work unless it names others (snx_vk_ring_pass), summed per name and said every 10 seconds; and, where the
   device has VK_NV_device_diagnostic_checkpoints, a checkpoint at each part, the ones the queue reached said when the
   device is lost */
#ifndef SNX_COMMON_VK_MARKS_H
#define SNX_COMMON_VK_MARKS_H
#include "common/gpu/vk/device.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct SnxVkMarks SnxVkMarks;
/* in a renderer's choose: VK_NV_device_diagnostic_checkpoints enabled where checkpoints (vk_checkpoints) asks and the
   device has it; true where it was */
bool snx_vk_marks_choose(SnxVkChoice* c, int checkpoints);
/* After the device: the marks of a ring of slots on the queue family, the profile where profile (vk_gpu_profile) asks
   and the family has timestamps, checkpoints where the device was made with them (checkpoints_made); who before each
   line, work the name of a submission's first part. NULL where neither is on; what was asked and cannot be said on
   stderr */
SnxVkMarks* snx_vk_marks_new(VkPhysicalDevice phys, VkDevice device, uint32_t family, int profile, int checkpoints,
                             bool checkpoints_made, const char* who, const char* work);
void snx_vk_marks_free(SnxVkMarks* m, VkDevice device);
/* the ring's: a slot's recording begun on cb, a part named name begun, the recording about to end, the slot's
   submission done (its times summed, said every 10 seconds), and the device lost on queue (the checkpoints it reached
   said) */
void snx_vk_marks_begin(SnxVkMarks* m, VkCommandBuffer cb, unsigned slot);
void snx_vk_marks_pass(SnxVkMarks* m, VkCommandBuffer cb, unsigned slot, const char* name);
void snx_vk_marks_end(SnxVkMarks* m, VkCommandBuffer cb, unsigned slot);
void snx_vk_marks_retire(SnxVkMarks* m, VkDevice device, unsigned slot, uint64_t now_ns);
void snx_vk_marks_lost(SnxVkMarks* m, VkQueue queue);
/* the profile's sums as its line says them, for its test: "frame 1.25 ms" each name's mean a submission, the
   submissions and the seconds they span */
void snx_vk_marks_line(const SnxVkMarks* m, double seconds, char* out, size_t size);
#endif
