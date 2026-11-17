/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Layout facts from Eden (src/hid_core/resources/shared_memory_format.h, src/hid_core/resources/ring_lifo.h,
   src/hid_core/hid_types.h, src/hid_core/resources/npad/npad_types.h,
   src/hid_core/resources/keyboard/keyboard_types.h, src/hid_core/resources/debug_pad/debug_pad_types.h,
   src/hid_core/resources/touch_screen/touch_types.h, src/hid_core/resources/mouse/mouse_types.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project */
/* HID's shared memory as the game reads it: HS_X is the size of structure X, HO_X_f the offset of its field f. Every
   value is guest-visible. tests/check_hid_layout.py compares them with Eden's declarations, and parses each
   #define line, so a value stays alone on its line. The checks at the end hold the values to each other */
#ifndef SNX_HID_LAYOUT_H
#define SNX_HID_LAYOUT_H

/* ---- the whole block (0x40000 bytes) and where each device's part starts */
#define HS_HidSharedMemory                         0x40000
#define HO_HidSharedMemory_debug_pad               0x0
#define HO_HidSharedMemory_touchscreen             0x400
#define HO_HidSharedMemory_mouse                   0x3400
#define HO_HidSharedMemory_keyboard                0x3800
#define HO_HidSharedMemory_home_button             0x4c00
#define HO_HidSharedMemory_capture_button          0x5000
#define HO_HidSharedMemory_unique_pad              0x5a00
#define HO_HidSharedMemory_npad                    0x9a00
#define HO_HidSharedMemory_gesture                 0x3ba00
#define HO_HidSharedMemory_console_six_axis_sensor 0x3c200
#define HO_HidSharedMemory_npad_condition          0x3e200

/* ---- the global Npad condition: the SDK's GetNpadJoyHoldType reads it before any Npad has a state */
#define HS_HidNpadCondition                 0x10
#define HO_HidNpadCondition_is_initialized  0x4
#define HO_HidNpadCondition_hold_type       0x8
#define HO_HidNpadCondition_is_valid        0xc

/* ---- a ring of samples: a header, then the entries, each a sampling number and a state */
#define HS_HidCommonLifoHeader              0x20
#define HO_HidCommonLifoHeader_unused       0x0
#define HO_HidCommonLifoHeader_buffer_count 0x8
#define HO_HidCommonLifoHeader_tail         0x10
#define HO_HidCommonLifoHeader_count        0x18

/* ---- one npad: an entry per Npad id at this stride, its state at the start. The state's 0x43f8 bytes count the six
   one-byte sensor properties after the four controller types, as Eden's layout does */
#define HS_HidNpadSharedMemoryEntry                                0x5000
#define HS_HidNpadInternalState                                    0x43f8
#define HO_HidNpadInternalState_style_set                          0x0
#define HO_HidNpadInternalState_joy_assignment_mode                0x4
#define HO_HidNpadInternalState_full_key_color                     0x8
#define HO_HidNpadInternalState_joy_color                          0x14
#define HO_HidNpadInternalState_full_key_lifo                      0x28
#define HO_HidNpadInternalState_handheld_lifo                      0x378
#define HO_HidNpadInternalState_joy_dual_lifo                      0x6c8
#define HO_HidNpadInternalState_joy_left_lifo                      0xa18
#define HO_HidNpadInternalState_joy_right_lifo                     0xd68
#define HO_HidNpadInternalState_palma_lifo                         0x10b8
#define HO_HidNpadInternalState_system_ext_lifo                    0x1408
#define HO_HidNpadInternalState_full_key_six_axis_sensor_lifo      0x1758
#define HO_HidNpadInternalState_handheld_six_axis_sensor_lifo      0x1e60
#define HO_HidNpadInternalState_joy_dual_left_six_axis_sensor_lifo 0x2568
#define HO_HidNpadInternalState_joy_dual_right_six_axis_sensor_lifo 0x2c70
#define HO_HidNpadInternalState_joy_left_six_axis_sensor_lifo       0x3378
#define HO_HidNpadInternalState_joy_right_six_axis_sensor_lifo      0x3a80
#define HO_HidNpadInternalState_device_type                         0x4188
#define HO_HidNpadInternalState_system_properties                   0x4190
#define HO_HidNpadInternalState_system_button_properties            0x4198
#define HO_HidNpadInternalState_battery_level                       0x419c
#define HO_HidNpadInternalState_applet_footer_ui_attribute          0x41a8
#define HO_HidNpadInternalState_applet_footer_ui_type               0x41ac
#define HO_HidNpadInternalState_gc_trigger_lifo                     0x4228
#define HO_HidNpadInternalState_lark_type_l_and_main                0x43e0

/* controller colors */
#define HS_HidNpadControllerColor   0x8
#define HS_HidNpadFullKeyColorState 0xc
#define HS_HidNpadJoyColorState     0x14

/* buttons and sticks */
#define HS_HidNpadCommonLifo                               0x350
#define HO_HidNpadCommonLifo_storage                       0x20
#define HS_HidNpadCommonStateAtomicStorage                 0x30
#define HO_HidNpadCommonStateAtomicStorage_sampling_number 0x0
#define HO_HidNpadCommonStateAtomicStorage_state           0x8
#define HS_HidNpadCommonState                              0x28
#define HO_HidNpadCommonState_sampling_number              0x0
#define HO_HidNpadCommonState_buttons                      0x8
#define HO_HidNpadCommonState_analog_stick_l               0x10
#define HO_HidNpadCommonState_analog_stick_r               0x18
#define HO_HidNpadCommonState_attributes                   0x20

/* motion */
#define HS_HidNpadSixAxisSensorLifo            0x708
#define HO_HidNpadSixAxisSensorLifo_storage    0x20
#define HS_HidSixAxisSensorStateAtomicStorage  0x68
#define HS_HidSixAxisSensorState               0x60
#define HO_HidSixAxisSensorState_acceleration  0x10
#define HO_HidSixAxisSensorState_direction     0x34
#define HO_HidSixAxisSensorState_attributes    0x58

/* GameCube controller triggers */
#define HO_HidNpadGcTriggerLifo_storage       0x20
#define HS_HidNpadGcTriggerStateAtomicStorage 0x18

/* ---- the touch screen */
#define HO_HidTouchScreenSharedMemoryFormat_lifo 0x0
#define HS_HidTouchScreenLifo                    0x2c38
#define HO_HidTouchScreenLifo_storage            0x20
#define HS_HidTouchScreenStateAtomicStorage      0x298
#define HS_HidTouchScreenState                   0x290
#define HO_HidTouchScreenState_count             0x8
#define HO_HidTouchScreenState_touches           0x10
#define HS_HidTouchState                         0x28
#define HO_HidTouchState_attribute               0x8
#define HO_HidTouchState_finger                  0xc
#define HO_HidTouchState_position_x              0x10
#define HO_HidTouchState_position_y              0x14
#define HO_HidTouchState_diameter_x              0x18
#define HO_HidTouchState_diameter_y              0x1c
#define HO_HidTouchState_rotation_angle          0x20

/* ---- keyboard, mouse and debug pad */
#define HO_HidKeyboardSharedMemoryFormat_lifo 0x0
#define HS_HidKeyboardState                   0x30
#define HO_HidMouseSharedMemoryFormat_lifo    0x0
#define HS_HidMouseState                      0x28
#define HS_HidDebugPadState                   0x20

/* ---- consistency, checked at compile time. A ring holds 17 entries, each an
   8-byte sampling number before its state; the npad's rings follow each other; ten npad entries end where the
   gesture block starts; sixteen touches fill a touch screen state */
#define HID_LAYOUT_ENTRIES 17
#define HID_LAYOUT_CHECK(name, cond) static_assert(cond, "hid layout: " #name)
HID_LAYOUT_CHECK(npad_storage, HS_HidNpadCommonStateAtomicStorage == 8 + HS_HidNpadCommonState);
HID_LAYOUT_CHECK(npad_lifo, HS_HidNpadCommonLifo ==
                                 HS_HidCommonLifoHeader + HID_LAYOUT_ENTRIES * HS_HidNpadCommonStateAtomicStorage);
HID_LAYOUT_CHECK(npad_lifo_storage, HO_HidNpadCommonLifo_storage == HS_HidCommonLifoHeader);
HID_LAYOUT_CHECK(sixaxis_storage, HS_HidSixAxisSensorStateAtomicStorage == 8 + HS_HidSixAxisSensorState);
HID_LAYOUT_CHECK(sixaxis_lifo, HS_HidNpadSixAxisSensorLifo ==
                                   HS_HidCommonLifoHeader + HID_LAYOUT_ENTRIES * HS_HidSixAxisSensorStateAtomicStorage);
HID_LAYOUT_CHECK(touch_storage, HS_HidTouchScreenStateAtomicStorage == 8 + HS_HidTouchScreenState);
HID_LAYOUT_CHECK(touch_lifo, HS_HidTouchScreenLifo ==
                                  HS_HidCommonLifoHeader + HID_LAYOUT_ENTRIES * HS_HidTouchScreenStateAtomicStorage);
HID_LAYOUT_CHECK(touches, HS_HidTouchScreenState == HO_HidTouchScreenState_touches + 16 * HS_HidTouchState);
HID_LAYOUT_CHECK(touch_room,
                 HO_HidSharedMemory_touchscreen + HS_HidTouchScreenLifo <= HO_HidSharedMemory_mouse);
HID_LAYOUT_CHECK(colors, HO_HidNpadInternalState_joy_color ==
                             HO_HidNpadInternalState_full_key_color + HS_HidNpadFullKeyColorState);
HID_LAYOUT_CHECK(first_lifo,
                 HO_HidNpadInternalState_full_key_lifo == HO_HidNpadInternalState_joy_color + HS_HidNpadJoyColorState);
HID_LAYOUT_CHECK(handheld, HO_HidNpadInternalState_handheld_lifo ==
                               HO_HidNpadInternalState_full_key_lifo + HS_HidNpadCommonLifo);
HID_LAYOUT_CHECK(joy_dual,
                 HO_HidNpadInternalState_joy_dual_lifo == HO_HidNpadInternalState_handheld_lifo + HS_HidNpadCommonLifo);
HID_LAYOUT_CHECK(joy_left,
                 HO_HidNpadInternalState_joy_left_lifo == HO_HidNpadInternalState_joy_dual_lifo + HS_HidNpadCommonLifo);
HID_LAYOUT_CHECK(joy_right, HO_HidNpadInternalState_joy_right_lifo ==
                                HO_HidNpadInternalState_joy_left_lifo + HS_HidNpadCommonLifo);
HID_LAYOUT_CHECK(palma,
                 HO_HidNpadInternalState_palma_lifo == HO_HidNpadInternalState_joy_right_lifo + HS_HidNpadCommonLifo);
HID_LAYOUT_CHECK(system_ext,
                 HO_HidNpadInternalState_system_ext_lifo == HO_HidNpadInternalState_palma_lifo + HS_HidNpadCommonLifo);
HID_LAYOUT_CHECK(full_key_sixaxis, HO_HidNpadInternalState_full_key_six_axis_sensor_lifo ==
                                       HO_HidNpadInternalState_system_ext_lifo + HS_HidNpadCommonLifo);
HID_LAYOUT_CHECK(handheld_sixaxis, HO_HidNpadInternalState_handheld_six_axis_sensor_lifo ==
                                       HO_HidNpadInternalState_full_key_six_axis_sensor_lifo +
                                           HS_HidNpadSixAxisSensorLifo);
HID_LAYOUT_CHECK(dual_left_sixaxis, HO_HidNpadInternalState_joy_dual_left_six_axis_sensor_lifo ==
                                        HO_HidNpadInternalState_handheld_six_axis_sensor_lifo +
                                            HS_HidNpadSixAxisSensorLifo);
HID_LAYOUT_CHECK(dual_right_sixaxis, HO_HidNpadInternalState_joy_dual_right_six_axis_sensor_lifo ==
                                         HO_HidNpadInternalState_joy_dual_left_six_axis_sensor_lifo +
                                             HS_HidNpadSixAxisSensorLifo);
HID_LAYOUT_CHECK(left_sixaxis, HO_HidNpadInternalState_joy_left_six_axis_sensor_lifo ==
                                   HO_HidNpadInternalState_joy_dual_right_six_axis_sensor_lifo +
                                       HS_HidNpadSixAxisSensorLifo);
HID_LAYOUT_CHECK(right_sixaxis, HO_HidNpadInternalState_joy_right_six_axis_sensor_lifo ==
                                    HO_HidNpadInternalState_joy_left_six_axis_sensor_lifo +
                                        HS_HidNpadSixAxisSensorLifo);
HID_LAYOUT_CHECK(device_type, HO_HidNpadInternalState_device_type ==
                                  HO_HidNpadInternalState_joy_right_six_axis_sensor_lifo + HS_HidNpadSixAxisSensorLifo);
HID_LAYOUT_CHECK(gc_trigger, HO_HidNpadInternalState_lark_type_l_and_main ==
                                 HO_HidNpadInternalState_gc_trigger_lifo + HO_HidNpadGcTriggerLifo_storage +
                                     HID_LAYOUT_ENTRIES * HS_HidNpadGcTriggerStateAtomicStorage);
HID_LAYOUT_CHECK(npad_size, HS_HidNpadInternalState == 0x43f8 &&
                                HS_HidNpadInternalState <= HS_HidNpadSharedMemoryEntry);
HID_LAYOUT_CHECK(npad_entries,
                 HO_HidSharedMemory_npad + 10 * HS_HidNpadSharedMemoryEntry == HO_HidSharedMemory_gesture);
HID_LAYOUT_CHECK(touch_offsets, HO_HidTouchState_attribute == 0x8 && HO_HidTouchState_finger == 0xc &&
                                    HO_HidTouchState_position_x == 0x10 && HO_HidTouchState_position_y == 0x14 &&
                                    HO_HidTouchState_diameter_x == 0x18 && HO_HidTouchState_diameter_y == 0x1c &&
                                    HO_HidTouchState_rotation_angle == 0x20);
HID_LAYOUT_CHECK(whole, HO_HidSharedMemory_console_six_axis_sensor < HS_HidSharedMemory);
HID_LAYOUT_CHECK(condition, HO_HidSharedMemory_npad_condition + HS_HidNpadCondition <= HS_HidSharedMemory);
HID_LAYOUT_CHECK(condition_fields, HO_HidNpadCondition_is_initialized == 4 && HO_HidNpadCondition_hold_type == 8 &&
                                       HO_HidNpadCondition_is_valid == 12);
#undef HID_LAYOUT_CHECK
#undef HID_LAYOUT_ENTRIES
#endif
