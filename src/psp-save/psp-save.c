/* Copyright (c) 2018 38_ViTa_38
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.0 or later versions.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License 2.0 for more details.
 *
 * A copy of the GPL 2.0 should have been included with the program.
 * If not, see http://www.gnu.org/licenses/
 *
 * Description: a tool for encryption and decryption of PSP saves.
 */

/* decrypt_data() and encrypt_data() are from
 * https://github.com/hrydgard/ppsspp/tree/master/Tools/SaveTool, so here's the
 * copyright:
 *
 * PSP Software Development Kit - http://www.pspdev.org
 * -----------------------------------------------------------------------
 * Licensed under the BSD license, see LICENSE in PSPSDK root for details.
 *
 * Copyright (c) 2005 Jim Paris <jim@jtan.com>
 * Coypright (c) 2005 psp123
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chnnlsv.h"
#include "hash.h"
#include "kirk_engine.h"
#include "psp-save.h"

#define SFO_LEN 0x1330

static int align16(unsigned int v)
{
    return ((v + 0xF) >> 4) << 4;
}

FILE *try_open(const char *path, const char *mode)
{
    FILE *fp;
    if (!(fp = fopen(path, mode)))
        die("Can't open %s: %s\n", path, strerror(errno));
    return fp;
}

int filesize(FILE *fp) /* intentionally not size_t or long */
{
    int size;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    if (size <= 0)
        die("Bad file size %ld\n", size);
    rewind(fp);
    return size;
}

void die(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    exit(EXIT_FAILURE);
}

void decrypt_file(FILE *in, FILE *out, unsigned char *key, unsigned int mode)
{
    unsigned char *data;
    int len = filesize(in), aligned_len, tmp;
    aligned_len = align16(len);
    if (!(data = malloc(aligned_len)))
        die("Cannot allocate %d bytes\n", aligned_len);
    memset(data + len, 0, aligned_len - len);
    if ((tmp = fread(data, 1, len, in)) != len) {
        free(data);
        die("decrypt_file: read %d bytes, %d expected\n", tmp, len);
    }
    decrypt_data(mode, data, &len, &aligned_len, key);
    if ((tmp = fwrite(data, 1, len, out)) != len) {
        free(data);
        die("decrypt_file: wrote %d bytes, %d expected\n", tmp, len);
    }
    free(data);
}

/* Do the actual hardware decryption.
   mode is 3 for saves with a cryptkey, or 1 otherwise
   data, dataLen, and cryptkey must be multiples of 0x10.
   cryptkey is NULL if mode == 1.
*/
int decrypt_data(unsigned int mode, unsigned char *data, int *data_len,
  int *aligned_len, unsigned char *key)
{
    pspChnnlsvContext1 ctx1;
    pspChnnlsvContext2 ctx2;

    /* Need a 16-byte IV plus some data */
    if (*aligned_len <= 0x10)
        return -1;
    *data_len -= 0x10;
    *aligned_len -= 0x10;

    /* Set up buffers */
    memset(&ctx1, 0, sizeof(ctx1));
    memset(&ctx2, 0, sizeof(ctx2));

    /* Perform the magic */
    if (sceSdSetIndex_(&ctx1, mode) < 0)
        return -2;
    if (sceSdCreateList_(&ctx2, mode, 2, data, key) < 0)
        return -3;
    if (sceSdRemoveValue_(&ctx1, data, 0x10) < 0)
        return -4;
    if (sceSdRemoveValue_(&ctx1, data + 0x10, *aligned_len) < 0)
        return -5;
    if (sceSdSetMember_(&ctx2, data + 0x10, *aligned_len) < 0)
        return -6;

    /* Verify that it decrypted correctly */
    if (sceChnnlsv_21BE78B4_(&ctx2) < 0)
        return -7;

    /* The decrypted data starts at data + 0x10, so shift it back. */
    memmove(data, data + 0x10, *data_len);
    return 0;
}

void encrypt_file(FILE *in, FILE *out, const char *name, FILE *sfo_in,
  FILE *sfo_out, unsigned char *key, unsigned int mode)
{
    unsigned char *data, *hash, sfo[SFO_LEN];
    int len = filesize(in), aligned_len, tmp;

    aligned_len = align16(len);
    if ((tmp = filesize(sfo_in)) != SFO_LEN)
        die("PARAM.SFO is not %d bytes long\n", SFO_LEN);
    if (!(data = malloc(aligned_len + 16)))
        die("Cannot allocate %d bytes\n", aligned_len + 16);
    if (!(hash = malloc(16)))
        die("Cannot allocate 16 bytes, what a shame\n");

    memset(data + len, 0, aligned_len - len);
    if ((tmp = fread(data, 1, len, in)) != len) {
        free(data);
        free(hash);
        die("encrypt_file: read %d bytes, %d expected\n", tmp, len);
    }
    if ((tmp = fread(sfo, 1, SFO_LEN, sfo_in)) != SFO_LEN) {
        free(data);
        free(hash);
        die("encrypt_file: read %d bytes, %d expected\n", tmp, SFO_LEN);
    }

    if ((tmp = encrypt_data(mode, data, &len, &aligned_len, hash, key))) {
        free(data);
        free(hash);
        die("encrypt_data failed (%d)\n", tmp);
    }
    if ((tmp = fwrite(data, 1, len, out)) != len) {
        free(data);
        free(hash);
        die("encrypt_file: wrote %d bytes, %d expected\n", tmp, len);
    }
    free(data);
    if ((tmp = update_hashes(sfo, SFO_LEN, name, hash, key ? 3 : 1))) {
        /*free(hash);*/
        printf("update_hashes returned %d\n", tmp);
    }
    if ((tmp = fwrite(sfo, 1, SFO_LEN, sfo_out)) != SFO_LEN) {
        free(hash);
        die("encrypt_file: wrote %d bytes, %d expected\n", tmp, SFO_LEN);
    }
    free(hash);
}

/* Do the actual hardware encryption.
   mode is 3 for saves with a cryptkey, or 1 otherwise
   data, dataLen, and cryptkey must be multiples of 0x10.
   cryptkey is NULL if mode == 1.
*/
int encrypt_data(unsigned int mode, unsigned char *data, int *dataLen,
  int *alignedLen, unsigned char *hash, unsigned char *cryptkey)
{
    pspChnnlsvContext1 ctx1;
    pspChnnlsvContext2 ctx2;

    /* Make room for the IV in front of the data. */
    memmove(data + 0x10, data, *alignedLen);

    /* Set up buffers */
    memset(&ctx1, 0, sizeof(ctx1));
    memset(&ctx2, 0, sizeof(ctx2));
    memset(hash, 0, 0x10);
    memset(data, 0, 0x10);

    /* Build the 0x10-byte IV and setup encryption */
    if (sceSdCreateList_(&ctx2, mode, 1, data, cryptkey) < 0)
        return -1;
    if (sceSdSetIndex_(&ctx1, mode) < 0)
        return -2;
    if (sceSdRemoveValue_(&ctx1, data, 0x10) < 0)
        return -3;
    if (sceSdSetMember_(&ctx2, data + 0x10, *alignedLen) < 0)
        return -4;

    /* Clear any extra bytes left from the previous steps */
    memset(data + 0x10 + *dataLen, 0, *alignedLen - *dataLen);

    /* Encrypt the data */
    if (sceSdRemoveValue_(&ctx1, data + 0x10, *alignedLen) < 0)
        return -5;

    /* Verify encryption */
    if (sceChnnlsv_21BE78B4_(&ctx2) < 0)
        return -6;

    /* Build the file hash from this PSP */
    if (sceSdGetLastIndex_(&ctx1, hash, cryptkey) < 0)
        return -7;

    /* Adjust sizes to account for IV */
    *alignedLen += 0x10;
    *dataLen += 0x10;

    /* All done */
    return 0;
}

