/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_ANNOTATIONS_H
#define SNX_ANDROID_ANNOTATIONS_H
#include "android/dex/classfile.h"
#include "android/dex/dex.h"
#include <stdint.h>

/* A class's annotations_directory_item, and the JVM attributes that its annotations become. The runtime-visible
   annotations become RuntimeVisibleAnnotations (and the parameters' RuntimeVisibleParameterAnnotations), so that
   reflection finds them. Of the system annotations, dalvik.annotation.Signature becomes Signature, and
   AnnotationDefault becomes each method's AnnotationDefault. ART reads the others (InnerClass, EnclosingMethod,
   Throws, MemberClasses) for reflection that R8's output seldom uses, and this translation drops them */
typedef struct {
    const Dex* dex;
    uint32_t class_set; /* the class's annotation_set_item, 0 for none */
    uint32_t fields, methods, parameters;
    uint32_t lists; /* the offset of the field_annotation list, the method and parameter lists after it */
} DexAnnotations;

/* the directory at off (0 for a class without one: all empty) */
void dex_annotations(const Dex* dex, uint32_t off, DexAnnotations* out);
/* the annotation_set_item of a field or a method, the annotation_set_ref_list of a method's parameters; 0 for none */
uint32_t dex_field_annotations(const DexAnnotations* a, uint32_t field_idx);
uint32_t dex_method_annotations(const DexAnnotations* a, uint32_t method_idx);
uint32_t dex_parameter_annotations(const DexAnnotations* a, uint32_t method_idx);

/* the attributes of an annotation set into out, as a class's, a field's or a method's: the count of them */
unsigned cf_annotation_attributes(CfPool* pool, CfBuffer* out, const Dex* dex, uint32_t set_off);
/* RuntimeVisibleParameterAnnotations of a method with params parameters: 0 or 1 attributes */
unsigned cf_parameter_attributes(CfPool* pool, CfBuffer* out, const Dex* dex, uint32_t list_off, unsigned params);
/* the AnnotationDefault of method name, from its annotation class's set: 0 or 1 attributes */
unsigned cf_annotation_default(CfPool* pool, CfBuffer* out, const Dex* dex, uint32_t class_set, const char* name);
#endif
