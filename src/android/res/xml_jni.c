/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* android.content.res.XmlBlock's natives: a compiled XML document and its parsers, over axml.c */
#include "android/res/xml_jni.h"
#include "android/res/axml.h"
#include "android/runtime/jvm.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    AxmlDocument doc;
    size_t size;
    unsigned char data[]; /* the document's bytes, the block's own copy */
} Block;
typedef struct {
    AxmlParser parser;
} State;

static Block* block_of(jlong handle) {
    return (Block*)(intptr_t)handle;
}
static AxmlParser* parser_of(jlong handle) {
    return &((State*)(intptr_t)handle)->parser;
}
const AxmlParser* android_xml_parser(jlong state) {
    return state ? parser_of(state) : NULL;
}
static jint string_index(uint32_t index) {
    return index == AXML_NONE ? -1 : (jint)index;
}

static jlong JNICALL create(JNIEnv* env, jclass unused, jbyteArray data) {
    (void)unused;
    jsize size = (*env)->GetArrayLength(env, data);
    Block* block = malloc(sizeof *block + (size_t)size + 1);
    if (!block) return 0;
    block->size = (size_t)size;
    (*env)->GetByteArrayRegion(env, data, 0, size, (jbyte*)block->data);
    char error[128];
    if (!axml_open(&block->doc, block->data, block->size, error, sizeof error)) {
        free(block);
        return 0;
    }
    return (jlong)(intptr_t)block;
}
static jint JNICALL string_count(JNIEnv* env, jclass unused, jlong handle) {
    (void)env;
    (void)unused;
    return (jint)block_of(handle)->doc.strings.count;
}
static jstring JNICALL get_string(JNIEnv* env, jclass unused, jlong handle, jint index) {
    (void)unused;
    const ResStringPool* pool = &block_of(handle)->doc.strings;
    long n = index < 0 ? -1 : res_string16(pool, (uint32_t)index, NULL, 0);
    if (n < 0) return NULL;
    uint16_t* units = malloc(((size_t)n + 1) * sizeof *units);
    if (!units) return NULL;
    res_string16(pool, (uint32_t)index, units, (size_t)n);
    jstring s = (*env)->NewString(env, units, (jsize)n);
    free(units);
    return s;
}
static void JNICALL destroy(JNIEnv* env, jclass unused, jlong handle) {
    (void)env;
    (void)unused;
    free(block_of(handle));
}
static jlong JNICALL create_state(JNIEnv* env, jclass unused, jlong handle) {
    (void)env;
    (void)unused;
    State* state = malloc(sizeof *state);
    if (!state) return 0;
    axml_parser(&state->parser, &block_of(handle)->doc);
    return (jlong)(intptr_t)state;
}
static void JNICALL destroy_state(JNIEnv* env, jclass unused, jlong handle) {
    (void)env;
    (void)unused;
    free((State*)(intptr_t)handle);
}
static jint JNICALL next(JNIEnv* env, jclass unused, jlong handle) {
    (void)env;
    (void)unused;
    return axml_next(parser_of(handle));
}
static jint JNICALL depth(JNIEnv* env, jclass unused, jlong handle) {
    (void)env;
    (void)unused;
    return parser_of(handle)->depth;
}
static jint JNICALL line(JNIEnv* env, jclass unused, jlong handle) {
    (void)env;
    (void)unused;
    return (jint)axml_line(parser_of(handle));
}
static jint JNICALL name_space(JNIEnv* env, jclass unused, jlong handle) {
    (void)env;
    (void)unused;
    return string_index(axml_namespace(parser_of(handle)));
}
static jint JNICALL name(JNIEnv* env, jclass unused, jlong handle) {
    (void)env;
    (void)unused;
    return string_index(axml_name(parser_of(handle)));
}
static jint JNICALL text(JNIEnv* env, jclass unused, jlong handle) {
    (void)env;
    (void)unused;
    return string_index(axml_text(parser_of(handle)));
}
static jint JNICALL attribute_count(JNIEnv* env, jclass unused, jlong handle) {
    (void)env;
    (void)unused;
    return (jint)axml_attribute_count(parser_of(handle));
}
static jint JNICALL attribute_namespace(JNIEnv* env, jclass unused, jlong handle, jint i) {
    (void)env;
    (void)unused;
    return string_index(axml_attribute_namespace(parser_of(handle), (uint32_t)i));
}
static jint JNICALL attribute_name(JNIEnv* env, jclass unused, jlong handle, jint i) {
    (void)env;
    (void)unused;
    return string_index(axml_attribute_name(parser_of(handle), (uint32_t)i));
}
static jint JNICALL attribute_resource(JNIEnv* env, jclass unused, jlong handle, jint i) {
    (void)env;
    (void)unused;
    return (jint)axml_attribute_resource(parser_of(handle), (uint32_t)i);
}
static jint JNICALL attribute_type(JNIEnv* env, jclass unused, jlong handle, jint i) {
    (void)env;
    (void)unused;
    return axml_attribute_value(parser_of(handle), (uint32_t)i).type;
}
static jint JNICALL attribute_data(JNIEnv* env, jclass unused, jlong handle, jint i) {
    (void)env;
    (void)unused;
    return (jint)axml_attribute_value(parser_of(handle), (uint32_t)i).data;
}
static jint JNICALL attribute_string(JNIEnv* env, jclass unused, jlong handle, jint i) {
    (void)env;
    (void)unused;
    return string_index(axml_attribute_raw(parser_of(handle), (uint32_t)i));
}
static jint JNICALL attribute_index(JNIEnv* env, jclass unused, jlong handle, jstring ns, jstring attribute) {
    (void)unused;
    const char* n = ns ? (*env)->GetStringUTFChars(env, ns, NULL) : NULL;
    const char* a = (*env)->GetStringUTFChars(env, attribute, NULL);
    /* a null namespace is no namespace, as ResXMLParser::indexOfAttribute has it */
    long i = a ? axml_find(parser_of(handle), n ? n : "", a) : -1;
    if (n) (*env)->ReleaseStringUTFChars(env, ns, n);
    if (a) (*env)->ReleaseStringUTFChars(env, attribute, a);
    return (jint)i;
}
static jint JNICALL id_attribute(JNIEnv* env, jclass unused, jlong handle) {
    (void)env;
    (void)unused;
    return (jint)axml_id_attribute(parser_of(handle));
}
static jint JNICALL class_attribute(JNIEnv* env, jclass unused, jlong handle) {
    (void)env;
    (void)unused;
    return (jint)axml_class_attribute(parser_of(handle));
}
static jint JNICALL style_attribute(JNIEnv* env, jclass unused, jlong handle) {
    (void)env;
    (void)unused;
    return (jint)axml_style_attribute(parser_of(handle));
}

bool android_xml_register(JNIEnv* env) {
    static const JNINativeMethod methods[] = {
        {(char*)"nativeCreate", (char*)"([B)J", (void*)create},
        {(char*)"nativeStringCount", (char*)"(J)I", (void*)string_count},
        {(char*)"nativeGetString", (char*)"(JI)Ljava/lang/String;", (void*)get_string},
        {(char*)"nativeDestroy", (char*)"(J)V", (void*)destroy},
        {(char*)"nativeCreateParseState", (char*)"(J)J", (void*)create_state},
        {(char*)"nativeDestroyParseState", (char*)"(J)V", (void*)destroy_state},
        {(char*)"nativeNext", (char*)"(J)I", (void*)next},
        {(char*)"nativeGetDepth", (char*)"(J)I", (void*)depth},
        {(char*)"nativeGetLineNumber", (char*)"(J)I", (void*)line},
        {(char*)"nativeGetNamespace", (char*)"(J)I", (void*)name_space},
        {(char*)"nativeGetName", (char*)"(J)I", (void*)name},
        {(char*)"nativeGetText", (char*)"(J)I", (void*)text},
        {(char*)"nativeGetAttributeCount", (char*)"(J)I", (void*)attribute_count},
        {(char*)"nativeGetAttributeNamespace", (char*)"(JI)I", (void*)attribute_namespace},
        {(char*)"nativeGetAttributeName", (char*)"(JI)I", (void*)attribute_name},
        {(char*)"nativeGetAttributeResource", (char*)"(JI)I", (void*)attribute_resource},
        {(char*)"nativeGetAttributeDataType", (char*)"(JI)I", (void*)attribute_type},
        {(char*)"nativeGetAttributeData", (char*)"(JI)I", (void*)attribute_data},
        {(char*)"nativeGetAttributeStringValue", (char*)"(JI)I", (void*)attribute_string},
        {(char*)"nativeGetAttributeIndex", (char*)"(JLjava/lang/String;Ljava/lang/String;)I", (void*)attribute_index},
        {(char*)"nativeGetIdAttribute", (char*)"(J)I", (void*)id_attribute},
        {(char*)"nativeGetClassAttribute", (char*)"(J)I", (void*)class_attribute},
        {(char*)"nativeGetStyleAttribute", (char*)"(J)I", (void*)style_attribute},
    };
    return android_jvm_register(env, "android/content/res/XmlBlock", methods,
                                (int)(sizeof methods / sizeof *methods));
}
