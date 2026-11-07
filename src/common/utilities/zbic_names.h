/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The symbols zstd 1.5.7's decoder keeps global (its FSE, HUF and internal ZSTD functions, which no visibility macro
   reaches), renamed so that zbic.c's copy of the decoder does not clash with snx_zstd's in one program. The list is
   what nm -g --defined-only shows for zbic.c without it: after a change of zstd's version, the same command lists any
   name that must be added */
#ifndef SNX_ZBIC_NAMES_H
#define SNX_ZBIC_NAMES_H
#define ERR_getErrorString               snx_zbic_ERR_getErrorString
#define FSE_buildDTable_wksp             snx_zbic_FSE_buildDTable_wksp
#define FSE_decompress_wksp_bmi2         snx_zbic_FSE_decompress_wksp_bmi2
#define FSE_getErrorName                 snx_zbic_FSE_getErrorName
#define FSE_isError                      snx_zbic_FSE_isError
#define FSE_versionNumber                snx_zbic_FSE_versionNumber
#define g_debuglevel                     snx_zbic_g_debuglevel
#define HUF_decompress1X1_DCtx_wksp      snx_zbic_HUF_decompress1X1_DCtx_wksp
#define HUF_decompress1X2_DCtx_wksp      snx_zbic_HUF_decompress1X2_DCtx_wksp
#define HUF_decompress1X_DCtx_wksp       snx_zbic_HUF_decompress1X_DCtx_wksp
#define HUF_decompress1X_usingDTable     snx_zbic_HUF_decompress1X_usingDTable
#define HUF_decompress4X_hufOnly_wksp    snx_zbic_HUF_decompress4X_hufOnly_wksp
#define HUF_decompress4X_usingDTable     snx_zbic_HUF_decompress4X_usingDTable
#define HUF_getErrorName                 snx_zbic_HUF_getErrorName
#define HUF_isError                      snx_zbic_HUF_isError
#define HUF_readDTableX1_wksp            snx_zbic_HUF_readDTableX1_wksp
#define HUF_readDTableX2_wksp            snx_zbic_HUF_readDTableX2_wksp
#define HUF_readStats                    snx_zbic_HUF_readStats
#define HUF_readStats_wksp               snx_zbic_HUF_readStats_wksp
#define HUF_selectDecoder                snx_zbic_HUF_selectDecoder
#define ZSTD_buildFSETable               snx_zbic_ZSTD_buildFSETable
#define ZSTD_checkContinuity             snx_zbic_ZSTD_checkContinuity
#define ZSTD_copyDDictParameters         snx_zbic_ZSTD_copyDDictParameters
#define ZSTD_DDict_dictContent           snx_zbic_ZSTD_DDict_dictContent
#define ZSTD_DDict_dictSize              snx_zbic_ZSTD_DDict_dictSize
#define ZSTD_decodeLiteralsBlock_wrapper snx_zbic_ZSTD_decodeLiteralsBlock_wrapper
#define ZSTD_decodeSeqHeaders            snx_zbic_ZSTD_decodeSeqHeaders
#define ZSTD_decompressBlock_deprecated  snx_zbic_ZSTD_decompressBlock_deprecated
#define ZSTD_decompressBlock_internal    snx_zbic_ZSTD_decompressBlock_internal
#define ZSTD_getcBlockSize               snx_zbic_ZSTD_getcBlockSize
#define ZSTD_loadDEntropy                snx_zbic_ZSTD_loadDEntropy
#endif
