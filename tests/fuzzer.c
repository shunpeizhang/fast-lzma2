/*
 * Copyright (c) 2015-present, Yann Collet, Facebook, Inc.
 * All rights reserved.
 * Modified for FL2 by Conor McCarthy
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */


/*-************************************
*  Compiler specific
**************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  define _CRT_SECURE_NO_WARNINGS   /* fgets */
#  pragma warning(disable : 4127)   /* disable: C4127: conditional expression is constant */
#  pragma warning(disable : 4204)   /* disable: C4204: non-constant aggregate initializer */
#endif


/*-************************************
*  Includes
**************************************/
#include <stdlib.h>       /* free */
#include <stdio.h>        /* fgets, sscanf */
#include <string.h>       /* strcmp */
#include <time.h>         /* clock_t */
#define FL2_STATIC_LINKING_ONLY  /* FL2_compressContinue, FL2_compressBlock */
#include "../fast-lzma2.h"         /* FL2_VERSION_STRING */
#include "../fl2_errors.h"  /* FL2_getErrorCode */
#include "datagen.h"      /* RDG_genBuffer */
#include "../mem.h"
#include "../xxhash.h"

/*-************************************
*  Constants
**************************************/
#define KB *(1U<<10)
#define MB *(1U<<20)
#define GB *(1U<<30)

static const U32 FUZ_compressibility_default = 50;
static const U32 nbTestsDefault = 10000;


/*-************************************
*  Display Macros
**************************************/
#define DISPLAY(...)          fprintf(stdout, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...)  if (g_displayLevel>=l) { DISPLAY(__VA_ARGS__); }
static U32 g_displayLevel = 2;

#define DISPLAYUPDATE(l, ...) if (g_displayLevel>=l) { \
            if ((FUZ_clockSpan(g_displayClock) > g_refreshRate) || (g_displayLevel>=4)) \
            { g_displayClock = clock(); DISPLAY(__VA_ARGS__); \
            if (g_displayLevel>=4) fflush(stdout); } }
static const clock_t g_refreshRate = CLOCKS_PER_SEC / 6;
static clock_t g_displayClock = 0;


/*-*******************************************************
*  Fuzzer functions
*********************************************************/
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))

static clock_t FUZ_clockSpan(clock_t cStart)
{
    return clock() - cStart;   /* works even when overflow; max span ~ 30mn */
}

#define FUZ_rotl32(x,r) ((x << r) | (x >> (32 - r)))
static unsigned FUZ_rand(unsigned* src)
{
    static const U32 prime1 = 2654435761U;
    static const U32 prime2 = 2246822519U;
    U32 rand32 = *src;
    rand32 *= prime1;
    rand32 += prime2;
    rand32  = FUZ_rotl32(rand32, 13);
    *src = rand32;
    return rand32 >> 5;
}

static unsigned FUZ_highbit32(U32 v32)
{
    unsigned nbBits = 0;
    if (v32==0) return 0;
    while (v32) v32 >>= 1, nbBits++;
    return nbBits;
}


/*=============================================
*   Memory Tests
=============================================*/
#if defined(__APPLE__) && defined(__MACH__)

#include <malloc/malloc.h>    /* malloc_size */

typedef struct {
    unsigned long long totalMalloc;
    size_t currentMalloc;
    size_t peakMalloc;
    unsigned nbMalloc;
    unsigned nbFree;
} mallocCounter_t;

static const mallocCounter_t INIT_MALLOC_COUNTER = { 0, 0, 0, 0, 0 };

static void* FUZ_mallocDebug(void* counter, size_t size)
{
    mallocCounter_t* const mcPtr = (mallocCounter_t*)counter;
    void* const ptr = malloc(size);
    if (ptr==NULL) return NULL;
    DISPLAYLEVEL(4, "allocating %u KB => effectively %u KB \n",
        (U32)(size >> 10), (U32)(malloc_size(ptr) >> 10));  /* OS-X specific */
    mcPtr->totalMalloc += size;
    mcPtr->currentMalloc += size;
    if (mcPtr->currentMalloc > mcPtr->peakMalloc)
        mcPtr->peakMalloc = mcPtr->currentMalloc;
    mcPtr->nbMalloc += 1;
    return ptr;
}

static void FUZ_freeDebug(void* counter, void* address)
{
    mallocCounter_t* const mcPtr = (mallocCounter_t*)counter;
    DISPLAYLEVEL(4, "freeing %u KB \n", (U32)(malloc_size(address) >> 10));
    mcPtr->nbFree += 1;
    mcPtr->currentMalloc -= malloc_size(address);  /* OS-X specific */
    free(address);
}

static void FUZ_displayMallocStats(mallocCounter_t count)
{
    DISPLAYLEVEL(3, "peak:%6u KB,  nbMallocs:%2u, total:%6u KB \n",
        (U32)(count.peakMalloc >> 10),
        count.nbMalloc,
        (U32)(count.totalMalloc >> 10));
}

#define CHECK_Z(f) {                               \
    size_t const err = f;                          \
    if (FL2_isError(err)) {                       \
        DISPLAY("Error => %s : %s ",               \
                #f, FL2_getErrorName(err));       \
        exit(1);                                   \
}   }

static int FUZ_mallocTests(unsigned seed, double compressibility, unsigned part)
{
    size_t const inSize = 64 MB + 16 MB + 4 MB + 1 MB + 256 KB + 64 KB; /* 85.3 MB */
    size_t const outSize = FL2_compressBound(inSize);
    void* const inBuffer = malloc(inSize);
    void* const outBuffer = malloc(outSize);

    /* test only played in verbose mode, as they are long */
    if (g_displayLevel<3) return 0;

    /* Create compressible noise */
    if (!inBuffer || !outBuffer) {
        DISPLAY("Not enough memory, aborting\n");
        exit(1);
    }
    RDG_genBuffer(inBuffer, inSize, compressibility, 0. /*auto*/, seed);

    /* simple compression tests */
    if (part <= 1)
    {   int compressionLevel;
        for (compressionLevel=1; compressionLevel<=6; compressionLevel++) {
            mallocCounter_t malcount = INIT_MALLOC_COUNTER;
            FL2_customMem const cMem = { FUZ_mallocDebug, FUZ_freeDebug, &malcount };
            FL2_CCtx* const cctx = FL2_createCCtx_advanced(cMem);
            CHECK_Z( FL2_compressCCtx(cctx, outBuffer, outSize, inBuffer, inSize, compressionLevel) );
            FL2_freeCCtx(cctx);
            DISPLAYLEVEL(3, "compressCCtx level %i : ", compressionLevel);
            FUZ_displayMallocStats(malcount);
    }   }

    /* streaming compression tests */
    if (part <= 2)
    {   int compressionLevel;
        for (compressionLevel=1; compressionLevel<=6; compressionLevel++) {
            mallocCounter_t malcount = INIT_MALLOC_COUNTER;
            FL2_customMem const cMem = { FUZ_mallocDebug, FUZ_freeDebug, &malcount };
            FL2_CCtx* const cstream = FL2_createCStream_advanced(cMem);
            FL2_outBuffer out = { outBuffer, outSize, 0 };
            FL2_inBuffer in = { inBuffer, inSize, 0 };
            CHECK_Z( FL2_initCStream(cstream, compressionLevel) );
            CHECK_Z( FL2_compressStream(cstream, &out, &in) );
            CHECK_Z( FL2_endStream(cstream, &out) );
            FL2_freeCStream(cstream);
            DISPLAYLEVEL(3, "compressStream level %i : ", compressionLevel);
            FUZ_displayMallocStats(malcount);
    }   }

    /* advanced MT API test */
    if (part <= 3)
    {   U32 nbThreads;
        for (nbThreads=1; nbThreads<=4; nbThreads++) {
            int compressionLevel;
            for (compressionLevel=1; compressionLevel<=6; compressionLevel++) {
                mallocCounter_t malcount = INIT_MALLOC_COUNTER;
                FL2_customMem const cMem = { FUZ_mallocDebug, FUZ_freeDebug, &malcount };
                FL2_CCtx* const cctx = FL2_createCCtx_advanced(cMem);
                FL2_outBuffer out = { outBuffer, outSize, 0 };
                FL2_inBuffer in = { inBuffer, inSize, 0 };
                CHECK_Z( FL2_CCtx_setParameter(cctx, FL2_p_compressionLevel, (U32)compressionLevel) );
                CHECK_Z( FL2_CCtx_setParameter(cctx, FL2_p_nbThreads, nbThreads) );
                while ( FL2_compress_generic(cctx, &out, &in, FL2_e_end) ) {}
                FL2_freeCCtx(cctx);
                DISPLAYLEVEL(3, "compress_generic,-T%u,end level %i : ",
                                nbThreads, compressionLevel);
                FUZ_displayMallocStats(malcount);
    }   }   }

    /* advanced MT streaming API test */
    if (part <= 4)
    {   U32 nbThreads;
        for (nbThreads=1; nbThreads<=4; nbThreads++) {
            int compressionLevel;
            for (compressionLevel=1; compressionLevel<=6; compressionLevel++) {
                mallocCounter_t malcount = INIT_MALLOC_COUNTER;
                FL2_customMem const cMem = { FUZ_mallocDebug, FUZ_freeDebug, &malcount };
                FL2_CCtx* const cctx = FL2_createCCtx_advanced(cMem);
                FL2_outBuffer out = { outBuffer, outSize, 0 };
                FL2_inBuffer in = { inBuffer, inSize, 0 };
                CHECK_Z( FL2_CCtx_setParameter(cctx, FL2_p_compressionLevel, (U32)compressionLevel) );
                CHECK_Z( FL2_CCtx_setParameter(cctx, FL2_p_nbThreads, nbThreads) );
                CHECK_Z( FL2_compress_generic(cctx, &out, &in, FL2_e_continue) );
                while ( FL2_compress_generic(cctx, &out, &in, FL2_e_end) ) {}
                FL2_freeCCtx(cctx);
                DISPLAYLEVEL(3, "compress_generic,-T%u,continue level %i : ",
                                nbThreads, compressionLevel);
                FUZ_displayMallocStats(malcount);
    }   }   }

    return 0;
}

#else

static int FUZ_mallocTests(unsigned seed, double compressibility, unsigned part)
{
    (void)seed; (void)compressibility; (void)part;
    return 0;
}

#endif

static size_t findDiff(const void* buf1, const void* buf2, size_t max)
{
    const BYTE* b1 = (const BYTE*)buf1;
    const BYTE* b2 = (const BYTE*)buf2;
    size_t u;
    for (u = 0; u<max; u++) {
        if (b1[u] != b2[u]) break;
    }
    return u;
}

static int callback(const void *src, size_t size, void *opaque)
{
    FL2_outBuffer *out = (FL2_outBuffer*)opaque;
    memcpy((BYTE*)out->dst + out->pos, src, size);
    out->pos += size;
    return 0;
}

/*=============================================
*   Unit tests
=============================================*/

#define CHECK_V(var, fn)  size_t const var = fn; if (FL2_isError(var)) goto _output_error
#define CHECK(fn)  { CHECK_V(err, fn); }
#define CHECKPLUS(var, fn, more)  { CHECK_V(var, fn); more; }

static int basicUnitTests(U32 seed, double compressibility)
{
    size_t const CNBuffSize = 5 MB;
    void* const CNBuffer = malloc(CNBuffSize);
    size_t const compressedBufferSize = FL2_compressBound(CNBuffSize);
    void* const compressedBuffer = malloc(compressedBufferSize);
    void* const decodedBuffer = malloc(CNBuffSize);
    FL2_CStream *const cstream = FL2_createCStream();
    FL2_DStream *const dstream = FL2_createDStream();
    int testResult = 0;
    U32 testNb=0;
    size_t cSize;

    /* Create compressible noise */
    if (!CNBuffer || !compressedBuffer || !decodedBuffer || !cstream || !dstream) {
        DISPLAY("Not enough memory, aborting\n");
        testResult = 1;
        goto _end;
    }
    RDG_genBuffer(CNBuffer, CNBuffSize, compressibility, 0., seed);

    /* Basic tests */
    DISPLAYLEVEL(4, "test%3i : FL2_getErrorName : ", testNb++);
    {   const char* errorString = FL2_getErrorName(0);
        DISPLAYLEVEL(4, "OK : %s \n", errorString);
    }

    DISPLAYLEVEL(4, "test%3i : FL2_getErrorName with wrong value : ", testNb++);
    {   const char* errorString = FL2_getErrorName(499);
        DISPLAYLEVEL(4, "OK : %s \n", errorString);
    }


    DISPLAYLEVEL(4, "test%3i : compress %u bytes : ", testNb++, (U32)CNBuffSize);
    {   FL2_CCtx* cctx = FL2_createCCtxMt(0);
        if (cctx==NULL) goto _output_error;
/*        FL2_CCtx_setParameter(cctx, FL2_p_useReferenceMF, 1);*/
        CHECKPLUS(r, FL2_compressCCtx(cctx,
                            compressedBuffer, compressedBufferSize,
                            CNBuffer, CNBuffSize, 1),
                  cSize=r );
        DISPLAYLEVEL(4, "OK (%u bytes : %.2f%%)\n", (U32)cSize, (double)cSize/CNBuffSize*100);

        FL2_freeCCtx(cctx);
    }

    DISPLAYLEVEL(4, "test%3i : FL2_findDecompressedSize test : ", testNb++);
    {   unsigned long long const rSize = FL2_findDecompressedSize(compressedBuffer, cSize);
        if (rSize != CNBuffSize) goto _output_error;
    }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : decompress %u bytes : ", testNb++, (U32)CNBuffSize);
    { size_t const r = FL2_decompress(decodedBuffer, CNBuffSize, compressedBuffer, cSize);
      if (r != CNBuffSize) goto _output_error; }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : check decompressed result : ", testNb++);
    {   size_t diff = findDiff(decodedBuffer, CNBuffer, CNBuffSize);
        if(diff < CNBuffSize) goto _output_error;
    }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : decompress with 1 missing byte : ", testNb++);
    { size_t const r = FL2_decompress(decodedBuffer, CNBuffSize, compressedBuffer, cSize-1);
      if (!FL2_isError(r)) goto _output_error;
      if (FL2_getErrorCode((size_t)r) != FL2_error_srcSize_wrong) goto _output_error; }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : compress/decompress empty input : ", testNb++);
    {   FL2_CCtx* cctx = FL2_createCCtx();
        size_t r = FL2_compressCCtx(cctx, compressedBuffer, compressedBufferSize, NULL, 0, 10);
        if (FL2_isError(r)) goto _output_error;
        FL2_freeCCtx(cctx);
        r = FL2_decompress(decodedBuffer, CNBuffSize, compressedBuffer, r);
        if (r != 0) goto _output_error;
    }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : increase match buffer size : ", testNb++);
    {   FL2_CCtx* cctx = FL2_createCCtxMt(0);
        if (cctx == NULL) goto _output_error;
        CHECK(FL2_compressCCtx(cctx, compressedBuffer, compressedBufferSize, CNBuffer, CNBuffSize, 2));
        FL2_CCtx_setParameter(cctx, FL2_p_bufferLog, (unsigned)FL2_CCtx_setParameter(cctx, FL2_p_bufferLog, 0) - 1);
        CHECK(FL2_compressCCtx(cctx, compressedBuffer, compressedBufferSize, CNBuffer, CNBuffSize, 0));
        FL2_freeCCtx(cctx);
        CHECK(FL2_decompress(decodedBuffer, CNBuffSize, compressedBuffer, compressedBufferSize));
    }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : compress to callback fn : ", testNb++);
    {   FL2_blockBuffer in = { CNBuffer, 0, CNBuffSize, CNBuffSize };
        FL2_outBuffer out = { compressedBuffer, compressedBufferSize, 0 };
        FL2_CCtx* cctx = FL2_createCCtxMt(0);
        if (cctx == NULL) goto _output_error;
        FL2_CCtx_setParameter(cctx, FL2_p_compressionLevel, 1);
        BYTE prop = FL2_dictSizeProp(cctx);
        callback(&prop, 1, &out);
        CHECK(FL2_compressCCtxBlock_toFn(cctx, callback, &out, &in, NULL));
        CHECK(FL2_endFrame_toFn(cctx, callback, &out));
        FL2_freeCCtx(cctx);
        CHECK(FL2_decompress(decodedBuffer, CNBuffSize, compressedBuffer, compressedBufferSize));
    }
    DISPLAYLEVEL(4, "OK \n");

    /* streaming tests */

    DISPLAYLEVEL(4, "test%3i : compress stream in many chunks : ", testNb++);
    {   BYTE cBuf[0x8101];
        FL2_outBuffer out = { cBuf, sizeof(cBuf), 0 };
        FL2_inBuffer in = { CNBuffer, 0, 0 };
        BYTE *end = (BYTE*)CNBuffer + CNBuffSize;
        size_t r;
        CHECK(FL2_initCStream(cstream, 4));
        cSize = 0;
        while ((BYTE*)in.src < end) {
            in.src = (BYTE*)in.src + in.pos;
            in.size = MIN(0x8101, end - (BYTE*)in.src);
            in.pos = 0;
            CHECK(FL2_compressStream(cstream, &out, &in));
            if (out.pos == out.size) {
                memcpy((BYTE*)compressedBuffer + cSize, out.dst, out.pos);
                cSize += out.pos;
                out.pos = 0;
            }
        }
        do {
            r = FL2_endStream(cstream, &out);
            if (FL2_isError(r)) goto _output_error;
            memcpy((BYTE*)compressedBuffer + cSize, out.dst, out.pos);
            cSize += out.pos;
            out.pos = 0;
        } while (r);
    }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : decompress stream in many chunks : ", testNb++);
    {   FL2_inBuffer in = { compressedBuffer, 0, 0 };
        FL2_outBuffer out = { decodedBuffer, 0, 0 };
        BYTE *send = (BYTE*)compressedBuffer + cSize;
        BYTE *oend = (BYTE*)decodedBuffer + CNBuffSize;
        size_t r;
        size_t total = 0;
        CHECK(FL2_initDStream(dstream));
        do {
            if (in.pos + LZMA_REQUIRED_INPUT_MAX >= in.size) {
                in.src = (BYTE*)in.src + in.pos;
                in.size = MIN(0x8101, send - (BYTE*)in.src);
                in.pos = 0;
            }
            out.dst = (BYTE*)out.dst + out.pos;
            out.size = MIN(0x8101, oend - (BYTE*)out.dst);
            out.pos = 0;
            r = FL2_decompressStream(dstream, &out, &in);
            total += out.pos;
            if (FL2_isError(r)) goto _output_error;
        } while (r);
        {   size_t diff = findDiff(CNBuffer, decodedBuffer, total);
            if(diff < CNBuffSize) goto _output_error;
    }   }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : compress stream in one chunk : ", testNb++);
    {   FL2_outBuffer out = { compressedBuffer, compressedBufferSize, 0 };
        FL2_inBuffer in = { CNBuffer, CNBuffSize, 0 };
        size_t r;
        CHECK(FL2_initCStream(cstream, 4));
        FL2_CStream_setParameter(cstream, FL2_p_blockSizeLog, 21);
        CHECK(FL2_compressStream(cstream, &out, &in));
        r = FL2_endStream(cstream, &out);
        if (r != 0) goto _output_error;
        cSize = out.pos;
    }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : decompress stream in one chunk : ", testNb++);
    {   FL2_inBuffer in = { compressedBuffer, cSize, 0 };
        FL2_outBuffer out = { decodedBuffer, CNBuffSize, 0 };
        CHECK(FL2_initDStream(dstream));
        CHECK(FL2_decompressStream(dstream, &out, &in));
        {   size_t diff = findDiff(CNBuffer, decodedBuffer, out.pos);
            if (diff < CNBuffSize) goto _output_error;
        }
    }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : compress stream split hash write : ", testNb++);
    {   FL2_outBuffer out = { compressedBuffer, cSize - 1, 0 };
        FL2_inBuffer in = { CNBuffer, CNBuffSize, 0 };
        size_t r;
        CHECK(FL2_initCStream(cstream, 4));
        CHECK(FL2_compressStream(cstream, &out, &in));
        r = FL2_endStream(cstream, &out);
        if(!r) goto _output_error;
        out.size = cSize;
        r = FL2_endStream(cstream, &out);
        if (r != 0) goto _output_error;
        cSize = out.pos;
    }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : decompress stream from split hash write : ", testNb++);
    {   size_t const r = FL2_decompress(decodedBuffer, CNBuffSize, compressedBuffer, cSize);
        if (r != CNBuffSize) goto _output_error; }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : compress stream < dictionary_size : ", testNb++);
    {   FL2_outBuffer out = { compressedBuffer, compressedBufferSize, 0 };
        FL2_inBuffer in = { CNBuffer, 512 KB, 0 };
        size_t r;
        CHECK(FL2_initCStream(cstream, 4));
        CHECK(FL2_compressStream(cstream, &out, &in));
        r = FL2_endStream(cstream, &out);
        if (r != 0) goto _output_error;
        cSize = out.pos;
    }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : decompress stream < dictionary_size : ", testNb++);
    {   size_t const r = FL2_decompress(decodedBuffer, CNBuffSize, compressedBuffer, cSize);
        if (r != 512 KB) goto _output_error; }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : compress stream < dictionary_size with flush : ", testNb++);
    {
        FL2_outBuffer out = { compressedBuffer, compressedBufferSize, 0 };
        FL2_inBuffer in = { CNBuffer, 128 KB - 1, 0 };
        size_t r;
        FL2_CStream_setParameter(cstream, FL2_p_posBits, 4);
        CHECK(FL2_initCStream(cstream, 4));
        CHECK(FL2_compressStream(cstream, &out, &in));
        CHECK(FL2_flushStream(cstream, &out));
        in.src = (BYTE*)CNBuffer + 128 KB - 1;
        in.pos = 0;
        in.size = 1 MB;
        CHECK(FL2_compressStream(cstream, &out, &in));
        r = FL2_endStream(cstream, &out);
        if (r != 0) goto _output_error;
        cSize = out.pos;
    }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : decompress stream < dictionary_size with flush : ", testNb++);
    {   size_t const r = FL2_decompress(decodedBuffer, CNBuffSize, compressedBuffer, cSize);
        if (r != 128 KB + 1 MB - 1) goto _output_error; }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : compress empty stream : ", testNb++);
    {   FL2_outBuffer out = { compressedBuffer, compressedBufferSize, 0 };
        FL2_inBuffer in = { CNBuffer, 0, 0 };
        size_t r;
        CHECK(FL2_initCStream(cstream, 4));
        CHECK(FL2_compressStream(cstream, &out, &in));
        r = FL2_endStream(cstream, &out);
        if (r != 0) goto _output_error;
        cSize = out.pos;
    }
    DISPLAYLEVEL(4, "OK \n");

    DISPLAYLEVEL(4, "test%3i : decompress empty stream : ", testNb++);
    {   size_t const r = FL2_decompress(decodedBuffer, CNBuffSize, compressedBuffer, cSize);
        if (r != 0) goto _output_error; }
    DISPLAYLEVEL(4, "OK \n");

    /* long rle test */
    {   size_t sampleSize = 0;
        DISPLAYLEVEL(4, "test%3i : Long RLE test : ", testNb++);
        RDG_genBuffer(CNBuffer, sampleSize, compressibility, 0., seed+1);
        memset((char*)CNBuffer+sampleSize, 'B', 256 KB - 1);
        sampleSize += 256 KB - 1;
        RDG_genBuffer((char*)CNBuffer+sampleSize, 96 KB, compressibility, 0., seed+2);
        sampleSize += 96 KB;
        cSize = FL2_compressMt(compressedBuffer, FL2_compressBound(sampleSize), CNBuffer, sampleSize, 1, 0);
        if (FL2_isError(cSize)) goto _output_error;
        { CHECK_V(regenSize, FL2_decompress(decodedBuffer, sampleSize, compressedBuffer, cSize));
          if (regenSize!=sampleSize) goto _output_error; }
        DISPLAYLEVEL(4, "OK \n");
    }

    /* All zeroes test */
    #define ZEROESLENGTH 100
    DISPLAYLEVEL(4, "test%3i : compress %u zeroes : ", testNb++, ZEROESLENGTH);
    memset(CNBuffer, 0, ZEROESLENGTH);
    { CHECK_V(r, FL2_compressMt(compressedBuffer, FL2_compressBound(ZEROESLENGTH), CNBuffer, ZEROESLENGTH, 1, 0) );
      cSize = r; }
    DISPLAYLEVEL(4, "OK (%u bytes : %.2f%%)\n", (U32)cSize, (double)cSize/ZEROESLENGTH*100);

    DISPLAYLEVEL(4, "test%3i : decompress %u zeroes : ", testNb++, ZEROESLENGTH);
    { CHECK_V(r, FL2_decompress(decodedBuffer, ZEROESLENGTH, compressedBuffer, cSize) );
      if (r != ZEROESLENGTH) goto _output_error; }
    DISPLAYLEVEL(4, "OK \n");

    /* Repeat test */
    #define REPTESTLENGTH 0x20000
    for (int rpt = 2; rpt <= 4; rpt <<= 1) {
        static const char rptBytes[4] = "\0\1\2\3";
        for (int i = 0; i < REPTESTLENGTH; i += rpt)    /* note : CNBuffer size > REPTESTLENGTH */
            memcpy((BYTE*)CNBuffer + i, rptBytes, rpt);
        DISPLAYLEVEL(4, "test%3i : compress %d-byte repeats : ", testNb++, rpt);
        { CHECK_V(r, FL2_compress(compressedBuffer, FL2_compressBound(REPTESTLENGTH),
            CNBuffer, REPTESTLENGTH, 10));
        cSize = r; }
        DISPLAYLEVEL(4, "OK (%u bytes : %.2f%%)\n", (U32)cSize, (double)cSize / REPTESTLENGTH * 100);

        DISPLAYLEVEL(4, "test%3i : decompress %d-byte repeats : ", testNb++, rpt);
        { CHECK_V(r, FL2_decompress(decodedBuffer, REPTESTLENGTH, compressedBuffer, cSize));
        if (r != REPTESTLENGTH) goto _output_error; }
        DISPLAYLEVEL(4, "OK \n");
    }

    /* error string tests */
    DISPLAYLEVEL(4, "test%3i : testing FL2 error code strings : ", testNb++);
    if (strcmp("No error detected", FL2_getErrorName((FL2_ErrorCode)(0-FL2_error_no_error))) != 0) goto _output_error;
    if (strcmp("No error detected", FL2_getErrorString(FL2_error_no_error)) != 0) goto _output_error;
    if (strcmp("Unspecified error code", FL2_getErrorString((FL2_ErrorCode)(0-FL2_error_GENERIC))) != 0) goto _output_error;
    if (strcmp("Error (generic)", FL2_getErrorName((size_t)0-FL2_error_GENERIC)) != 0) goto _output_error;
    if (strcmp("Error (generic)", FL2_getErrorString(FL2_error_GENERIC)) != 0) goto _output_error;
    if (strcmp("No error detected", FL2_getErrorName(FL2_error_GENERIC)) != 0) goto _output_error;
    DISPLAYLEVEL(4, "OK \n");

_end:
    free(CNBuffer);
    free(compressedBuffer);
    free(decodedBuffer);
    FL2_freeCStream(cstream);
    FL2_freeDStream(dstream);
    return testResult;

_output_error:
    testResult = 1;
    DISPLAY("Error detected in Unit tests ! \n");
    goto _end;
}

static int decompressionTests(U32 seed, U32 nbTests, unsigned startTest, U32 const maxDurationS, double compressibility)
{
    size_t const CNBuffSize = 5 MB;
    void* const CNBuffer = malloc(CNBuffSize);
    size_t const compressedBufferSize = FL2_compressBound(CNBuffSize);
    void* const compressedBuffer = malloc(compressedBufferSize);
    void* const decodedBuffer = malloc(CNBuffSize);
    FL2_DStream *const dstream = FL2_createDStream();
    clock_t const startClock = clock();
    clock_t const maxClockSpan = maxDurationS * CLOCKS_PER_SEC;
    int testResult = 0;
    U32 testNb = 0;
    U32 coreSeed = seed, lseed = 0;
    size_t cSize;

    /* Create compressible noise */
    if (!CNBuffer || !compressedBuffer || !decodedBuffer || !dstream) {
        DISPLAY("Not enough memory, aborting\n");
        testResult = 1;
        goto _end;
    }
    RDG_genBuffer(CNBuffer, CNBuffSize, compressibility, 0., seed);

    {   FL2_CCtx* cctx = FL2_createCCtxMt(0);
        if (cctx == NULL) goto _output_error;
        CHECKPLUS(r,
            FL2_compressCCtx(cctx, compressedBuffer, compressedBufferSize, CNBuffer, CNBuffSize, 4),
            cSize = r);

        FL2_freeCCtx(cctx);
    }


    /* catch up testNb */
    for (testNb = 1; testNb < startTest; testNb++) FUZ_rand(&coreSeed);

    /* main test loop */
    for (; (testNb <= nbTests) || (FUZ_clockSpan(startClock) < maxClockSpan); testNb++) {
        FL2_inBuffer in = { compressedBuffer, 0, 0 };
        FL2_outBuffer out = { decodedBuffer, 0, 0 };
        BYTE *send = (BYTE*)compressedBuffer + cSize;
        BYTE *oend = (BYTE*)decodedBuffer + CNBuffSize;
        size_t r;
        size_t total = 0;
        unsigned in_bits = 10 + FUZ_rand(&seed) % 11;
        size_t in_size = 0x100 + (FUZ_rand(&seed) & ((1U << in_bits) - 1));
        unsigned out_bits = 10 + FUZ_rand(&seed) % 13;
        size_t out_size = 0x400 + (FUZ_rand(&seed) & ((1U << out_bits) - 1));

        /* notification */
        if (nbTests >= testNb) { DISPLAYUPDATE(2, "\r%6u/%6u    ", testNb, nbTests); }
        else { DISPLAYUPDATE(2, "\r%6u          ", testNb); }

        CHECK(FL2_initDStream(dstream));

        do {
            if (in.pos + LZMA_REQUIRED_INPUT_MAX >= in.size) {
                in.src = (BYTE*)in.src + in.pos;
                in.size = MIN(in_size, (size_t)(send - (BYTE*)in.src));
                in.pos = 0;
            }
            out.dst = (BYTE*)out.dst + out.pos;
            out.size = MIN(out_size, (size_t)(oend - (BYTE*)out.dst));
            out.pos = 0;
            r = FL2_decompressStream(dstream, &out, &in);
            total += out.pos;
            if (FL2_isError(r)) goto _output_error;
        } while (r);
        if (findDiff(CNBuffer, decodedBuffer, total) < CNBuffSize) goto _output_error;
    }
    DISPLAY("\r%u streaming decompression tests completed   \n", testNb - 1);

_end:
    free(CNBuffer);
    free(compressedBuffer);
    free(decodedBuffer);
    FL2_freeDStream(dstream);
    return testResult;

_output_error:
    testResult = 1;
    DISPLAY("Error detected in Unit tests ! \n");
    goto _end;
}

static size_t FUZ_rLogLength(U32* seed, U32 logLength)
{
    size_t const lengthMask = ((size_t)1 << logLength) - 1;
    return (lengthMask+1) + (FUZ_rand(seed) & lengthMask);
}

static size_t FUZ_randomLength(U32* seed, U32 maxLog)
{
    U32 const logLength = FUZ_rand(seed) % maxLog;
    return FUZ_rLogLength(seed, logLength);
}

#undef CHECK
#define CHECK(cond, ...) {                                    \
    if (cond) {                                               \
        DISPLAY("Error => ");                                 \
        DISPLAY(__VA_ARGS__);                                 \
        DISPLAY(" (seed %u, test nb %u)  \n", seed, testNb);  \
        goto _output_error;                                   \
}   }

#undef CHECK_Z
#define CHECK_Z(f) {                                          \
    size_t const err = f;                                     \
    if (FL2_isError(err)) {                                  \
        DISPLAY("Error => %s : %s ",                          \
                #f, FL2_getErrorName(err));                  \
        DISPLAY(" (seed %u, test nb %u)  \n", seed, testNb);  \
        goto _output_error;                                   \
}   }


static int fuzzerTests(unsigned nbThreads, U32 seed, U32 nbTests, unsigned startTest, U32 const maxDurationS, double compressibility, int bigTests)
{
    static const U32 maxSrcLog = 26;
    static const U32 maxSampleLog = 25;
    size_t const srcBufferSize = (size_t)1<<maxSrcLog;
    size_t const dstBufferSize = (size_t)1<<maxSampleLog;
    size_t const cBufferSize   = FL2_compressBound(dstBufferSize);
    BYTE* cNoiseBuffer[5];
    BYTE* srcBuffer;   /* jumping pointer */
    BYTE* const cBuffer = (BYTE*) malloc (cBufferSize);
    BYTE* const dstBuffer = (BYTE*) malloc (dstBufferSize);
    BYTE* const mirrorBuffer = (BYTE*) malloc (dstBufferSize);
    FL2_CCtx* const cctx = FL2_createCCtxMt(nbThreads);
    FL2_DCtx* const dctx = FL2_createDCtx();
    U32 result = 0;
    U32 testNb = 0;
    U32 coreSeed = seed, lseed = 0;
    clock_t const startClock = clock();
    clock_t const maxClockSpan = maxDurationS * CLOCKS_PER_SEC;
    int const cLevelLimiter = bigTests ? 2 : 3;

    /* allocation */
    cNoiseBuffer[0] = (BYTE*)malloc (srcBufferSize);
    cNoiseBuffer[1] = (BYTE*)malloc (srcBufferSize);
    cNoiseBuffer[2] = (BYTE*)malloc (srcBufferSize);
    cNoiseBuffer[3] = (BYTE*)malloc (srcBufferSize);
    cNoiseBuffer[4] = (BYTE*)malloc (srcBufferSize);
    CHECK (!cNoiseBuffer[0] || !cNoiseBuffer[1] || !cNoiseBuffer[2] || !cNoiseBuffer[3] || !cNoiseBuffer[4]
           || !dstBuffer || !mirrorBuffer || !cBuffer || !cctx || !dctx,
           "Not enough memory, fuzzer tests cancelled");

    /* Create initial samples */
    RDG_genBuffer(cNoiseBuffer[0], srcBufferSize, 0.00, 0., coreSeed);    /* pure noise */
    RDG_genBuffer(cNoiseBuffer[1], srcBufferSize, 0.05, 0., coreSeed);    /* barely compressible */
    RDG_genBuffer(cNoiseBuffer[2], srcBufferSize, compressibility, 0., coreSeed);
    RDG_genBuffer(cNoiseBuffer[3], srcBufferSize, 0.95, 0., coreSeed);    /* highly compressible */
    RDG_genBuffer(cNoiseBuffer[4], srcBufferSize, 1.00, 0., coreSeed);    /* sparse content */
    srcBuffer = cNoiseBuffer[2];

    /* catch up testNb */
    for (testNb=1; testNb < startTest; testNb++) FUZ_rand(&coreSeed);

    /* main test loop */
    for ( ; (testNb <= nbTests) || (FUZ_clockSpan(startClock) < maxClockSpan); testNb++ ) {
        size_t sampleSize;
        size_t cSize;
        BYTE* sampleBuffer;

        /* notification */
        if (nbTests >= testNb) { DISPLAYUPDATE(2, "\r%6u/%6u    ", testNb, nbTests); }
        else { DISPLAYUPDATE(2, "\r%6u          ", testNb); }

        FUZ_rand(&coreSeed);
        { U32 const prime1 = 2654435761U; lseed = coreSeed ^ prime1; }

        /* srcBuffer selection [0-4] */
        {   U32 buffNb = FUZ_rand(&lseed) & 0x7F;
            if (buffNb & 7) buffNb=2;   /* most common : compressible (P) */
            else {
                buffNb >>= 3;
                if (buffNb & 7) {
                    const U32 tnb[2] = { 1, 3 };   /* barely/highly compressible */
                    buffNb = tnb[buffNb >> 3];
                } else {
                    const U32 tnb[2] = { 0, 4 };   /* not compressible / sparse */
                    buffNb = tnb[buffNb >> 3];
            }   }
            srcBuffer = cNoiseBuffer[buffNb];
        }

        /* select src segment */
        sampleSize = FUZ_randomLength(&lseed, maxSampleLog);

        /* create sample buffer (to catch read error with valgrind & sanitizers)  */
        sampleBuffer = (BYTE*)malloc(sampleSize);
        CHECK(sampleBuffer==NULL, "not enough memory for sample buffer");
        { size_t const sampleStart = FUZ_rand(&lseed) % (srcBufferSize - sampleSize);
          memcpy(sampleBuffer, srcBuffer + sampleStart, sampleSize); }

        /* compression tests */
        {   unsigned const cLevel =
                    ( FUZ_rand(&lseed) %
                     (FL2_maxCLevel() * 2U / cLevelLimiter) )
                     + 1;
            unsigned lc = FUZ_rand(&lseed) % 5;
			FL2_CCtx_setParameter(cctx, FL2_p_compressionLevel, cLevel);
            FL2_CCtx_setParameter(cctx, FL2_p_highCompression, (FUZ_rand(&lseed) & 3) > 2);
            if((FUZ_rand(&lseed) & 7) > 6)
                FL2_CCtx_setParameter(cctx, FL2_p_searchDepth, 64);
            if ((FUZ_rand(&lseed) & 3) > 2)
                FL2_CCtx_setParameter(cctx, FL2_p_divideAndConquer, 0);
            FL2_CCtx_setParameter(cctx, FL2_p_literalCtxBits, lc);
            FL2_CCtx_setParameter(cctx, FL2_p_literalPosBits, FUZ_rand(&lseed) % (5 - lc));
            FL2_CCtx_setParameter(cctx, FL2_p_posBits, FUZ_rand(&lseed) % 5);
            FL2_CCtx_setParameter(cctx, FL2_p_doXXHash, FUZ_rand(&lseed) & 1);
            cSize = FL2_compressCCtx(cctx, cBuffer, cBufferSize, sampleBuffer, sampleSize, 0);
            CHECK(FL2_isError(cSize), "FL2_compressCCtx failed : %s", FL2_getErrorName(cSize));

            /* compression failure test : too small dest buffer */
            if (cSize >= 2) {
                const size_t missing = (FUZ_rand(&lseed) % (cSize-1)) + 1;
                const size_t tooSmallSize = cSize - missing;
                const U32 endMark = 0x4DC2B1A9;
                memcpy(dstBuffer+tooSmallSize, &endMark, 4);
                { size_t const errorCode = FL2_compressCCtx(cctx, dstBuffer, tooSmallSize, sampleBuffer, sampleSize, 0);
                  CHECK(!FL2_isError(errorCode), "FL2_compressCCtx should have failed ! (buffer too small : %u < %u)", (U32)tooSmallSize, (U32)cSize); }
                { U32 endCheck; memcpy(&endCheck, dstBuffer+tooSmallSize, 4);
                  CHECK(endCheck != endMark, "FL2_compressCCtx : dst buffer overflow"); }
        }   }

        /* Decompressed size test */
        {   unsigned long long const rSize = FL2_findDecompressedSize(cBuffer, cSize);
            CHECK(rSize != sampleSize, "decompressed size incorrect");
        }

        /* Incompressible test */
        {size_t ccSize = FL2_compressCCtx(cctx, dstBuffer, dstBufferSize, cBuffer, cSize, 0);
        CHECK(ccSize > FL2_compressBound(cSize), "FL2_compressBound failed : %u > %u", (U32)ccSize, (U32)FL2_compressBound(cSize));
        }

        /* successful decompression test */
        {   size_t const margin = (FUZ_rand(&lseed) & 1) ? 0 : (FUZ_rand(&lseed) & 31) + 1;
            size_t const dSize = FL2_decompressDCtx(dctx, dstBuffer, sampleSize + margin, cBuffer, cSize);
            CHECK(dSize != sampleSize, "FL2_decompress failed (%s) (srcSize : %u ; cSize : %u)", FL2_getErrorName(dSize), (U32)sampleSize, (U32)cSize);
            {   size_t diff = findDiff(sampleBuffer, dstBuffer, sampleSize);
                CHECK(diff < sampleSize, "decompression result corrupted (pos %u / %u)", (U32)diff, (U32)sampleSize);
        }   }

        free(sampleBuffer);   /* no longer useful after this point */
        sampleBuffer = NULL;

        /* truncated src decompression test */
        {   size_t const missing = (FUZ_rand(&lseed) % (cSize-1)) + 1;
            size_t const tooSmallSize = cSize - missing;
            void* cBufferTooSmall = malloc(tooSmallSize);   /* valgrind will catch read overflows */
            CHECK(cBufferTooSmall == NULL, "not enough memory !");
            memcpy(cBufferTooSmall, cBuffer, tooSmallSize);
            { size_t const errorCode = FL2_decompressDCtx(dctx, dstBuffer, dstBufferSize, cBufferTooSmall, tooSmallSize);
              CHECK(!FL2_isError(errorCode), "FL2_decompress should have failed ! (truncated src buffer)"); }
            free(cBufferTooSmall);
        }

        /* too small dst decompression test */
        if (sampleSize >= 2) {
            size_t const missing = (FUZ_rand(&lseed) % (sampleSize-1)) + 1;
            size_t const tooSmallSize = sampleSize - missing;
            static const BYTE token = 0xA9;
            dstBuffer[tooSmallSize] = token;
            { size_t const errorCode = FL2_decompressDCtx(dctx, dstBuffer, tooSmallSize, cBuffer, cSize);
              CHECK(!FL2_isError(errorCode), "FL2_decompress should have failed : %u > %u (dst buffer too small)", (U32)errorCode, (U32)tooSmallSize); }
            CHECK(dstBuffer[tooSmallSize] != token, "FL2_decompress : dst buffer overflow");
        }

        /* noisy src decompression test */
        if (cSize > 6) {
            /* insert noise into src */
            {   U32 const maxNbBits = FUZ_highbit32((U32)(cSize-4));
                size_t pos = 0;
                for (;;) {
                    /* keep some original src */
                    {   U32 const nbBits = FUZ_rand(&lseed) % maxNbBits;
                        size_t const mask = (1<<nbBits) - 1;
                        size_t const skipLength = FUZ_rand(&lseed) & mask;
                        pos += skipLength;
                    }
                    if (pos >= cSize) break;
                    /* add noise */
                    {   U32 const nbBitsCodes = FUZ_rand(&lseed) % maxNbBits;
                        U32 const nbBits = nbBitsCodes ? nbBitsCodes-1 : 0;
                        size_t const mask = (1<<nbBits) - 1;
                        size_t const rNoiseLength = (FUZ_rand(&lseed) & mask) + 1;
                        size_t const noiseLength = MIN(rNoiseLength, cSize-pos);
                        size_t const noiseStart = FUZ_rand(&lseed) % (srcBufferSize - noiseLength);
                        memcpy(cBuffer + pos, srcBuffer + noiseStart, noiseLength);
                        pos += noiseLength;
            }   }   }

            /* decompress noisy source */
            {   U32 const endMark = 0xA9B1C3D6;
                memcpy(dstBuffer+sampleSize, &endMark, 4);
                {   size_t const decompressResult = FL2_decompressDCtx(dctx, dstBuffer, sampleSize, cBuffer, cSize);
                    /* result *may* be an unlikely success, but even then, it must strictly respect dst buffer boundaries */
                    CHECK((!FL2_isError(decompressResult)) && (decompressResult>sampleSize),
                          "FL2_decompress on noisy src : result is too large : %u > %u (dst buffer)", (U32)decompressResult, (U32)sampleSize);
                }
                {   U32 endCheck; memcpy(&endCheck, dstBuffer+sampleSize, 4);
                    CHECK(endMark!=endCheck, "FL2_decompress on noisy src : dst buffer overflow");
        }   }   }   /* noisy src decompression test */
    }   /* for ( ; (testNb <= nbTests) */
    DISPLAY("\r%u fuzzer tests completed   \n", testNb-1);

_cleanup:
    FL2_freeCCtx(cctx);
    FL2_freeDCtx(dctx);
    free(cNoiseBuffer[0]);
    free(cNoiseBuffer[1]);
    free(cNoiseBuffer[2]);
    free(cNoiseBuffer[3]);
    free(cNoiseBuffer[4]);
    free(cBuffer);
    free(dstBuffer);
    free(mirrorBuffer);
    return result;

_output_error:
    result = 1;
    goto _cleanup;
}


/*_*******************************************************
*  Command line
*********************************************************/
static int FUZ_usage(const char* programName)
{
    DISPLAY( "Usage :\n");
    DISPLAY( "      %s [args]\n", programName);
    DISPLAY( "\n");
    DISPLAY( "Arguments :\n");
    DISPLAY( " -i#    : Nb of tests (default:%u) \n", nbTestsDefault);
    DISPLAY( " -s#    : Select seed (default:prompt user)\n");
    DISPLAY( " -t#    : Select starting test number (default:0)\n");
    DISPLAY( " -P#    : Select compressibility in %% (default:%u%%)\n", FUZ_compressibility_default);
    DISPLAY( " -d     : Perform streaming decompression tests\n");
    DISPLAY( " -v     : verbose\n");
    DISPLAY( " -p     : pause at the end\n");
    DISPLAY( " -h     : display help and exit\n");
    return 0;
}

/*! readU32FromChar() :
    @return : unsigned integer value read from input in `char` format
    allows and interprets K, KB, KiB, M, MB and MiB suffix.
    Will also modify `*stringPtr`, advancing it to position where it stopped reading.
    Note : function result can overflow if digit string > MAX_UINT */
static unsigned readU32FromChar(const char** stringPtr)
{
    unsigned result = 0;
    while ((**stringPtr >='0') && (**stringPtr <='9'))
        result *= 10, result += **stringPtr - '0', (*stringPtr)++ ;
    if ((**stringPtr=='K') || (**stringPtr=='M')) {
        result <<= 10;
        if (**stringPtr=='M') result <<= 10;
        (*stringPtr)++ ;
        if (**stringPtr=='i') (*stringPtr)++;
        if (**stringPtr=='B') (*stringPtr)++;
    }
    return result;
}

/** longCommandWArg() :
 *  check if *stringPtr is the same as longCommand.
 *  If yes, @return 1 and advances *stringPtr to the position which immediately follows longCommand.
 *  @return 0 and doesn't modify *stringPtr otherwise.
 */
static unsigned longCommandWArg(const char** stringPtr, const char* longCommand)
{
    size_t const comSize = strlen(longCommand);
    int const result = !strncmp(*stringPtr, longCommand, comSize);
    if (result) *stringPtr += comSize;
    return result;
}

int main(int argc, const char** argv)
{
    U32 seed = 0;
    int seedset = 0;
    int argNb;
    int nbTests = nbTestsDefault;
    int testNb = 0;
    unsigned nbThreads = 0;
    U32 proba = FUZ_compressibility_default;
    int result = 0;
    U32 mainPause = 0;
    U32 maxDuration = 0;
    int bigTests = 1;
    U32 memTestsOnly = 0;
    int decompTests = 0;
    const char* const programName = argv[0];

    /* Check command line */
    for (argNb=1; argNb<argc; argNb++) {
        const char* argument = argv[argNb];
        if(!argument) continue;   /* Protection if argument empty */

        /* Handle commands. Aggregated commands are allowed */
        if (argument[0]=='-') {

            if (longCommandWArg(&argument, "--memtest=")) { memTestsOnly = readU32FromChar(&argument); continue; }

            if (!strcmp(argument, "--memtest")) { memTestsOnly=1; continue; }
            if (!strcmp(argument, "--no-big-tests")) { bigTests=0; continue; }

            argument++;
            while (*argument!=0) {
                switch(*argument)
                {
                case 'h':
                    return FUZ_usage(programName);

                case 'd':
                    argument++;
                    decompTests = 1;
                    break;

                case 'v':
                    argument++;
                    g_displayLevel = 4;
                    break;

                case 'q':
                    argument++;
                    g_displayLevel--;
                    break;

                case 'p': /* pause at the end */
                    argument++;
                    mainPause = 1;
                    break;

                case 'i':
                    argument++; maxDuration = 0;
                    nbTests = readU32FromChar(&argument);
                    break;

                case 'm':
                    argument++;
                    nbThreads = readU32FromChar(&argument);
                    break;

                case 'T':
                    argument++;
                    nbTests = 0;
                    maxDuration = readU32FromChar(&argument);
                    if (*argument=='s') argument++;   /* seconds */
                    if (*argument=='m') maxDuration *= 60, argument++;   /* minutes */
                    if (*argument=='n') argument++;
                    break;

                case 's':
                    argument++;
                    seedset = 1;
                    seed = readU32FromChar(&argument);
                    break;

                case 't':
                    argument++;
                    testNb = readU32FromChar(&argument);
                    break;

                case 'P':   /* compressibility % */
                    argument++;
                    proba = readU32FromChar(&argument);
                    if (proba>100) proba = 100;
                    break;

                default:
                    return (FUZ_usage(programName), 1);
    }   }   }   }   /* for (argNb=1; argNb<argc; argNb++) */

    /* Get Seed */
    DISPLAY("Starting fast-lzma2 tester (%i-bits, %s)\n", (int)(sizeof(size_t)*8), FL2_VERSION_STRING);

    if (!seedset) {
        time_t const t = time(NULL);
        U32 const h = (U32)t * 506832829U;
        seed = h % 10000;
    }

    DISPLAY("Seed = %u\n", seed);
    if (proba!=FUZ_compressibility_default) DISPLAY("Compressibility : %u%%\n", proba);

    if (memTestsOnly) {
        g_displayLevel = MAX(3, g_displayLevel);
        return FUZ_mallocTests(seed, ((double)proba) / 100, memTestsOnly);
    }

    if (nbTests < testNb) nbTests = testNb;

    if (testNb==0)
        result = basicUnitTests(0, ((double)proba) / 100);  /* constant seed for predictability */
    if (!result && decompTests)
        result = decompressionTests(seed, nbTests, testNb, maxDuration, ((double)proba) / 100);
    if (!result)
        result = fuzzerTests(nbThreads, seed, nbTests, testNb, maxDuration, ((double)proba) / 100, bigTests);
    if (mainPause) {
        int unused;
        DISPLAY("Press Enter \n");
        unused = getchar();
        (void)unused;
    }
    return result;
}
