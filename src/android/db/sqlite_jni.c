/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx.android.db.SQLite's natives over SQLite. Text goes through SQLite's UTF-16 functions in the host's byte order,
   which is the order of a jchar, so Java's strings pass with no conversion. An error becomes the exception that
   Android's android_database_SQLiteCommon.cpp throws for its code */
#include "android/db/sqlite_jni.h"
#include "android/runtime/jvm.h"
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>

static sqlite3* db_of(jlong handle) {
    return (sqlite3*)(intptr_t)handle;
}
static sqlite3_stmt* statement_of(jlong handle) {
    return (sqlite3_stmt*)(intptr_t)handle;
}

/* the exception class of an error code, as Android chooses it */
static const char* exception_class(int code) {
    switch (code & 0xff) {
    case SQLITE_IOERR: return "android/database/sqlite/SQLiteDiskIOException";
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB: return "android/database/sqlite/SQLiteDatabaseCorruptException";
    case SQLITE_CONSTRAINT: return "android/database/sqlite/SQLiteConstraintException";
    case SQLITE_ABORT: return "android/database/sqlite/SQLiteAbortException";
    case SQLITE_DONE: return "android/database/sqlite/SQLiteDoneException";
    case SQLITE_FULL: return "android/database/sqlite/SQLiteFullException";
    case SQLITE_MISUSE: return "android/database/sqlite/SQLiteMisuseException";
    case SQLITE_PERM: return "android/database/sqlite/SQLiteAccessPermException";
    case SQLITE_BUSY: return "android/database/sqlite/SQLiteDatabaseLockedException";
    case SQLITE_LOCKED: return "android/database/sqlite/SQLiteTableLockedException";
    case SQLITE_READONLY: return "android/database/sqlite/SQLiteReadOnlyDatabaseException";
    case SQLITE_CANTOPEN: return "android/database/sqlite/SQLiteCantOpenDatabaseException";
    case SQLITE_TOOBIG: return "android/database/sqlite/SQLiteBlobTooBigException";
    case SQLITE_RANGE: return "android/database/sqlite/SQLiteBindOrColumnIndexOutOfRangeException";
    case SQLITE_NOMEM: return "android/database/sqlite/SQLiteOutOfMemoryException";
    case SQLITE_MISMATCH: return "android/database/sqlite/SQLiteDatatypeMismatchException";
    case SQLITE_INTERRUPT: return "android/os/OperationCanceledException";
    default: return "android/database/sqlite/SQLiteException";
    }
}

/* the exception for code, with SQLite's message for the connection (NULL for none) and the extended code, as
   Android's message reads: "message (code N SQLITE_NAME)" */
static void throw_error(JNIEnv* env, sqlite3* db, int code, const char* what) {
    if ((*env)->ExceptionCheck(env)) return;
    int extended = db ? sqlite3_extended_errcode(db) : code;
    const char* message = db ? sqlite3_errmsg(db) : sqlite3_errstr(code);
    char text[1024];
    snprintf(text, sizeof text, "%s%s%s (code %d %s)", what ? what : "", what ? ": " : "", message, extended,
             sqlite3_errstr(extended));
    jclass type = (*env)->FindClass(env, exception_class(code));
    if (!type) {
        (*env)->ExceptionClear(env);
        type = (*env)->FindClass(env, "android/database/sqlite/SQLiteException");
    }
    if (type) (*env)->ThrowNew(env, type, text);
}

static jlong JNICALL open_db(JNIEnv* env, jclass unused, jstring path, jint flags) {
    (void)unused;
    const char* name = (*env)->GetStringUTFChars(env, path, NULL);
    if (!name) return 0;
    sqlite3* db = NULL;
    /* the Java side gives SQLite's own open flags; a connection is used by one thread at a time */
    int code = sqlite3_open_v2(name, &db, (int)flags | SQLITE_OPEN_NOMUTEX, NULL);
    if (code != SQLITE_OK) {
        throw_error(env, db, code, name);
        (*env)->ReleaseStringUTFChars(env, path, name);
        sqlite3_close(db);
        return 0;
    }
    (*env)->ReleaseStringUTFChars(env, path, name);
    /* Android's defaults: a busy timeout, and extended result codes */
    sqlite3_busy_timeout(db, 2500);
    sqlite3_extended_result_codes(db, 1);
    return (jlong)(intptr_t)db;
}
static void JNICALL close_db(JNIEnv* env, jclass unused, jlong db) {
    (void)unused;
    int code = sqlite3_close(db_of(db));
    if (code != SQLITE_OK) throw_error(env, db_of(db), code, "close");
}
static jlong JNICALL prepare(JNIEnv* env, jclass unused, jlong db, jstring sql) {
    (void)unused;
    const jchar* text = (*env)->GetStringChars(env, sql, NULL);
    if (!text) return 0;
    jsize length = (*env)->GetStringLength(env, sql);
    sqlite3_stmt* statement = NULL;
    int code = sqlite3_prepare16_v2(db_of(db), text, (int)length * 2, &statement, NULL);
    (*env)->ReleaseStringChars(env, sql, text);
    if (code != SQLITE_OK) {
        const char* s = (*env)->GetStringUTFChars(env, sql, NULL);
        throw_error(env, db_of(db), code, s ? s : "prepare");
        if (s) (*env)->ReleaseStringUTFChars(env, sql, s);
        return 0;
    }
    if (!statement) { /* a statement of only spaces or comments */
        jclass type = (*env)->FindClass(env, "java/lang/IllegalArgumentException");
        if (type) (*env)->ThrowNew(env, type, "Cannot prepare an empty SQL statement");
        return 0;
    }
    return (jlong)(intptr_t)statement;
}
static void JNICALL finalize_statement(JNIEnv* env, jclass unused, jlong statement) {
    (void)env;
    (void)unused;
    sqlite3_finalize(statement_of(statement));
}
static void JNICALL reset(JNIEnv* env, jclass unused, jlong statement) {
    (void)env;
    (void)unused;
    /* its result is the last step's, which that step reported */
    sqlite3_reset(statement_of(statement));
}
static void JNICALL clear_bindings(JNIEnv* env, jclass unused, jlong statement) {
    (void)env;
    (void)unused;
    sqlite3_clear_bindings(statement_of(statement));
}
static jint JNICALL parameter_count(JNIEnv* env, jclass unused, jlong statement) {
    (void)env;
    (void)unused;
    return sqlite3_bind_parameter_count(statement_of(statement));
}
static void check_bind(JNIEnv* env, jlong statement, int code) {
    if (code != SQLITE_OK) throw_error(env, sqlite3_db_handle(statement_of(statement)), code, "bind");
}
static void JNICALL bind_null(JNIEnv* env, jclass unused, jlong statement, jint index) {
    (void)unused;
    check_bind(env, statement, sqlite3_bind_null(statement_of(statement), index));
}
static void JNICALL bind_long(JNIEnv* env, jclass unused, jlong statement, jint index, jlong value) {
    (void)unused;
    check_bind(env, statement, sqlite3_bind_int64(statement_of(statement), index, value));
}
static void JNICALL bind_double(JNIEnv* env, jclass unused, jlong statement, jint index, jdouble value) {
    (void)unused;
    check_bind(env, statement, sqlite3_bind_double(statement_of(statement), index, value));
}
static void JNICALL bind_string(JNIEnv* env, jclass unused, jlong statement, jint index, jstring value) {
    (void)unused;
    const jchar* text = (*env)->GetStringChars(env, value, NULL);
    if (!text) return;
    jsize length = (*env)->GetStringLength(env, value);
    int code = sqlite3_bind_text16(statement_of(statement), index, text, (int)length * 2, SQLITE_TRANSIENT);
    (*env)->ReleaseStringChars(env, value, text);
    check_bind(env, statement, code);
}
static void JNICALL bind_blob(JNIEnv* env, jclass unused, jlong statement, jint index, jbyteArray value) {
    (void)unused;
    jsize length = (*env)->GetArrayLength(env, value);
    jbyte* bytes = (*env)->GetByteArrayElements(env, value, NULL);
    if (!bytes) return;
    int code = sqlite3_bind_blob(statement_of(statement), index, bytes, (int)length, SQLITE_TRANSIENT);
    (*env)->ReleaseByteArrayElements(env, value, bytes, JNI_ABORT);
    check_bind(env, statement, code);
}
static jint JNICALL step(JNIEnv* env, jclass unused, jlong statement) {
    (void)unused;
    int code = sqlite3_step(statement_of(statement));
    if (code == SQLITE_ROW || code == SQLITE_DONE) return code;
    throw_error(env, sqlite3_db_handle(statement_of(statement)), code, NULL);
    return 0;
}
static jint JNICALL column_count(JNIEnv* env, jclass unused, jlong statement) {
    (void)env;
    (void)unused;
    return sqlite3_column_count(statement_of(statement));
}
/* UTF-16 text from SQLite as a String: bytes of it, NULL for an SQL NULL */
static jstring string16(JNIEnv* env, const void* text, int bytes) {
    return text ? (*env)->NewString(env, text, bytes / 2) : NULL;
}
static jstring JNICALL column_name(JNIEnv* env, jclass unused, jlong statement, jint index) {
    (void)unused;
    const void* name = sqlite3_column_name16(statement_of(statement), index);
    jsize n = 0;
    if (name)
        while (((const jchar*)name)[n]) ++n;
    return name ? (*env)->NewString(env, name, n) : NULL;
}
static jint JNICALL column_type(JNIEnv* env, jclass unused, jlong statement, jint index) {
    (void)env;
    (void)unused;
    return sqlite3_column_type(statement_of(statement), index);
}
static jlong JNICALL column_long(JNIEnv* env, jclass unused, jlong statement, jint index) {
    (void)env;
    (void)unused;
    return sqlite3_column_int64(statement_of(statement), index);
}
static jdouble JNICALL column_double(JNIEnv* env, jclass unused, jlong statement, jint index) {
    (void)env;
    (void)unused;
    return sqlite3_column_double(statement_of(statement), index);
}
static jstring JNICALL column_string(JNIEnv* env, jclass unused, jlong statement, jint index) {
    (void)unused;
    const void* text = sqlite3_column_text16(statement_of(statement), index);
    return string16(env, text, sqlite3_column_bytes16(statement_of(statement), index));
}
static jbyteArray JNICALL column_blob(JNIEnv* env, jclass unused, jlong statement, jint index) {
    (void)unused;
    if (sqlite3_column_type(statement_of(statement), index) == SQLITE_NULL) return NULL;
    const void* blob = sqlite3_column_blob(statement_of(statement), index);
    int size = sqlite3_column_bytes(statement_of(statement), index);
    jbyteArray out = (*env)->NewByteArray(env, size);
    if (out && size) (*env)->SetByteArrayRegion(env, out, 0, size, blob);
    return out;
}
static jint JNICALL changes(JNIEnv* env, jclass unused, jlong db) {
    (void)env;
    (void)unused;
    return sqlite3_changes(db_of(db));
}
static jlong JNICALL last_insert_row_id(JNIEnv* env, jclass unused, jlong db) {
    (void)env;
    (void)unused;
    return sqlite3_last_insert_rowid(db_of(db));
}
static jboolean JNICALL read_only(JNIEnv* env, jclass unused, jlong statement) {
    (void)env;
    (void)unused;
    return sqlite3_stmt_readonly(statement_of(statement)) ? JNI_TRUE : JNI_FALSE;
}

bool android_sqlite_register(JNIEnv* env) {
    static const JNINativeMethod methods[] = {
        {(char*)"open", (char*)"(Ljava/lang/String;I)J", (void*)open_db},
        {(char*)"close", (char*)"(J)V", (void*)close_db},
        {(char*)"prepare", (char*)"(JLjava/lang/String;)J", (void*)prepare},
        {(char*)"finalizeStatement", (char*)"(J)V", (void*)finalize_statement},
        {(char*)"reset", (char*)"(J)V", (void*)reset},
        {(char*)"clearBindings", (char*)"(J)V", (void*)clear_bindings},
        {(char*)"parameterCount", (char*)"(J)I", (void*)parameter_count},
        {(char*)"bindNull", (char*)"(JI)V", (void*)bind_null},
        {(char*)"bindLong", (char*)"(JIJ)V", (void*)bind_long},
        {(char*)"bindDouble", (char*)"(JID)V", (void*)bind_double},
        {(char*)"bindString", (char*)"(JILjava/lang/String;)V", (void*)bind_string},
        {(char*)"bindBlob", (char*)"(JI[B)V", (void*)bind_blob},
        {(char*)"step", (char*)"(J)I", (void*)step},
        {(char*)"columnCount", (char*)"(J)I", (void*)column_count},
        {(char*)"columnName", (char*)"(JI)Ljava/lang/String;", (void*)column_name},
        {(char*)"columnType", (char*)"(JI)I", (void*)column_type},
        {(char*)"columnLong", (char*)"(JI)J", (void*)column_long},
        {(char*)"columnDouble", (char*)"(JI)D", (void*)column_double},
        {(char*)"columnString", (char*)"(JI)Ljava/lang/String;", (void*)column_string},
        {(char*)"columnBlob", (char*)"(JI)[B", (void*)column_blob},
        {(char*)"changes", (char*)"(J)I", (void*)changes},
        {(char*)"lastInsertRowId", (char*)"(J)J", (void*)last_insert_row_id},
        {(char*)"readOnly", (char*)"(J)Z", (void*)read_only},
    };
    return android_jvm_register(env, "snx/android/db/SQLite", methods, (int)(sizeof methods / sizeof *methods));
}
