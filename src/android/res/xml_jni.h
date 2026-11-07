/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_XML_JNI_H
#define SNX_ANDROID_XML_JNI_H
#include "android/res/axml.h"
#include <jni.h>
#include <stdbool.h>

/* android.content.res.XmlBlock's natives */
bool android_xml_register(JNIEnv* env);
/* the parser of a parse state handle that XmlBlock holds, NULL for 0 */
const AxmlParser* android_xml_parser(jlong state);
#endif
