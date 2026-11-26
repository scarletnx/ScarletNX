/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The JIT cache's key (jit_cache.h): what LLVM, the target and the process's IR settings make of the objects */
#include "common/cpu/recompiler/jit/jit_cache.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/rtlib/rtlib_object.h"
#include "common/utilities/bits.h"
#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>
#include <llvm/Config/llvm-config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* snx_jit_cache_key(char opt, int hook, int svc, SnxU64 guest) {
    char* cpu = LLVMGetHostCPUName();
    char* features = snx_ir_host_features();
    char* triple = LLVMGetDefaultTargetTriple();
    if (!cpu || !features || !triple) {
        LLVMDisposeMessage(cpu);
        LLVMDisposeMessage(features);
        LLVMDisposeMessage(triple);
        return NULL;
    }
    size_t n = strlen(cpu) + strlen(features) + strlen(triple) + 256;
    char* key = (char*)malloc(n);
    if (key) {
        SnxU64 rtlib = snx_fnv1a(SNX_FNV_BASIS, snx_rtlib_object, (size_t)snx_rtlib_object_size);
        /* dec: the decoding (a64.decode and its profile) and a64: the guest's features choose what each word does */
        snprintf(key, n,
                 "abi=%d obj=%d llvm=%s triple=%s rtlib=%016llx native_code=%s dec=%016llx a64=%016llx cpu=%s opt=%c "
                 "hook=%d svc=%d slp=%d fpsr=%d volatile=%d contract=%d features=%s",
                 SNX_ABI, SNX_IR_OBJECT_VERSION, LLVM_VERSION_STRING, triple, (unsigned long long)rtlib,
                 snx_native_code(), (unsigned long long)snx_a64_decoder_hash(),
                 (unsigned long long)snx_a64_features(guest), cpu, opt, hook, svc, snx_ir_slp(), snx_ir_fpsr_exact(),
                 snx_ir_volatile(), snx_ir_abi_contract_on(), features);
    }
    LLVMDisposeMessage(cpu);
    LLVMDisposeMessage(features);
    LLVMDisposeMessage(triple);
    return key;
}
