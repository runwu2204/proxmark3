//-----------------------------------------------------------------------------
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// BigBuf and functions to allocate/free parts of it.
//-----------------------------------------------------------------------------
#include "BigBuf.h"

#include "string.h"
#include "dbprint.h"
#include "pm3_cmd.h"
#include "util.h" // nbytes

#define BIGBUF_ALIGN_BYTES (4)
#define BIGBUF_ALIGN_MASK  (0xFFFF + 1 - BIGBUF_ALIGN_BYTES)

extern uint32_t _stack_start[], __bss_end__[];

// BigBuf is the large multi-purpose buffer, typically used to hold A/D samples or traces.
// Also used to hold various smaller buffers and the Mifare Emulator Memory.
// We know that bss is aligned to 4 bytes.
static uint8_t *const BigBuf = (uint8_t *)__bss_end__;

/* BigBuf memory layout:
Pointer to highest available memory: s_bigbuf_hi
    high s_bigbuf_size
    reserved = BigBuf_malloc()  subtracts amount from s_bigbuf_hi,
    low  0x00
*/

static uint32_t s_bigbuf_size = 0;

// High memory mark
static uint32_t s_bigbuf_hi = 0;

// pointer to the emulator memory.
static uint8_t *s_emulator_memory = NULL;

//=============================================================================
// The ToSend buffer.
// A buffer where we can queue things up to be sent through the FPGA, for
// any purpose (fake tag, as reader, whatever). We go MSB first, since that
// is the order in which they go out on the wire.
//=============================================================================
static tosend_t s_toSend = {
    .max = -1,
    .bit = 8,
    .buf = NULL
};
//=============================================================================
// The dmaBuf 16bit buffer.
// A buffer where we receive IQ samples sent from the FPGA, for demodulating
//=============================================================================
static dmabuf16_t s_dma_16 = {
    .size = DMA_BUFFER_SIZE,
    .buf = NULL
};
// dmaBuf 8bit buffer
static dmabuf8_t s_dma_8 = {
    .size = DMA_BUFFER_SIZE,
    .buf = NULL
};

// trace related variables
static uint32_t s_trace_len = 0;
static bool s_tracing = true;

// compute the available size for BigBuf
void BigBuf_initialize(void) {
    s_bigbuf_size = (uint32_t)_stack_start - (uint32_t)__bss_end__;
    s_bigbuf_hi = s_bigbuf_size;
    s_trace_len = 0;
}

// get the address of BigBuf
uint8_t *BigBuf_get_addr(void) {
    return (uint8_t *)BigBuf;
}

uint32_t BigBuf_get_size(void) {
    return s_bigbuf_size;
}

// get the address of the emulator memory. Allocate part of Bigbuf for it, if not yet done
uint8_t *BigBuf_get_EM_addr(void) {
    // not yet allocated
    if (s_emulator_memory == NULL) {
        s_emulator_memory = BigBuf_calloc(CARD_MEMORY_SIZE);
    }
    return s_emulator_memory;
}

uint32_t BigBuf_get_hi(void) {
    return s_bigbuf_hi;
}

/*
uint32_t BigBuf_get_EM_size(void) {
    return CARD_MEMORY_SIZE;
}
*/

// clear ALL of BigBuf
void BigBuf_Clear(void) {
    BigBuf_Clear_ext(true);
}

// clear ALL of BigBuf
void BigBuf_Clear_ext(bool verbose) {
    memset(BigBuf, 0, s_bigbuf_size);
    clear_trace();
    if (verbose) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Buffer cleared (%i bytes)", s_bigbuf_size);
    }
}

void BigBuf_Clear_EM(void) {
    memset(BigBuf_get_EM_addr(), 0, CARD_MEMORY_SIZE);
}

void BigBuf_Clear_keep_EM(void) {
    memset(BigBuf, 0, s_bigbuf_hi);
}

// allocate a chunk of memory from BigBuf. We allocate high memory first. The unallocated memory
// at the beginning of BigBuf is always for traces/samples
uint8_t *BigBuf_malloc(uint16_t chunksize) {
    chunksize = (chunksize + BIGBUF_ALIGN_BYTES - 1) & BIGBUF_ALIGN_MASK; // round up to next multiple of 4

    if (s_bigbuf_hi - s_trace_len < chunksize || chunksize == 0) {
        // no memory left or chunksize too large
        return NULL;
    }

    s_bigbuf_hi -= chunksize;  // aligned to 4 Byte boundary
    return (uint8_t *)BigBuf + s_bigbuf_hi;
}

// allocate a chunk of memory from BigBuf, and returns a pointer to it.
// sets the memory to zero
uint8_t *BigBuf_calloc(uint16_t chunksize) {
    uint8_t *mem = BigBuf_malloc(chunksize);
    if (mem != NULL) {
        memset(mem, 0x00, ((chunksize + BIGBUF_ALIGN_BYTES - 1) & BIGBUF_ALIGN_MASK)); // round up to next multiple of 4
    }
    return mem;
}

// free ALL allocated chunks. The whole BigBuf is available for traces or samples again.
void BigBuf_free(void) {
    s_bigbuf_hi = s_bigbuf_size;
    s_emulator_memory = NULL;
    // shouldn't this empty BigBuf also?
    s_toSend.buf = NULL;
    s_dma_16.buf = NULL;
    s_dma_8.buf = NULL;
}

// free allocated chunks EXCEPT the emulator memory
void BigBuf_free_keep_EM(void) {
    if (s_emulator_memory != NULL)
        s_bigbuf_hi = s_emulator_memory - (uint8_t *)BigBuf;
    else
        s_bigbuf_hi = s_bigbuf_size;

    s_toSend.buf = NULL;
    s_dma_16.buf = NULL;
    s_dma_8.buf = NULL;
}

void BigBuf_print_status(void) {
    DbpString(_CYAN_("Memory"));
    Dbprintf("  BigBuf_size............. %d", s_bigbuf_size);
    Dbprintf("  Available memory........ %d", s_bigbuf_hi);
    DbpString(_CYAN_("Tracing"));
    Dbprintf("  tracing ................ %d", s_tracing);
    Dbprintf("  traceLen ............... %d", s_trace_len);

    if (g_dbglevel >= DBG_DEBUG) {
        DbpString(_CYAN_("Sending buffers"));

        uint16_t d8 = 0;
        if (s_dma_8.buf)
            d8 = s_dma_8.buf - BigBuf_get_addr();

        uint16_t d16 = 0;
        if (s_dma_16.buf)
            d16 = (uint8_t *)s_dma_16.buf - BigBuf_get_addr();

        uint16_t ts = 0;
        if (s_toSend.buf)
            ts = s_toSend.buf - BigBuf_get_addr();

        Dbprintf("  dma8 memory............. %u", d8);
        Dbprintf("  dma16 memory............ %u", d16);
        Dbprintf("  toSend memory........... %u", ts);
    }
}

// return the maximum trace length (i.e. the unallocated size of BigBuf)
uint16_t BigBuf_max_traceLen(void) {
    return s_bigbuf_hi & BIGBUF_ALIGN_MASK;
}

void clear_trace(void) {
    s_trace_len = 0;
}

void set_tracelen(uint32_t value) {
    s_trace_len = value;
}

void set_tracing(bool enable) {
    s_tracing = enable;
}

bool get_tracing(void) {
    return s_tracing;
}

/**
 * Get the number of bytes traced
 * @return
 */
uint32_t BigBuf_get_traceLen(void) {
    return s_trace_len;
}

/**
  This is a function to store traces. All protocols can use this generic tracer-function.
  The traces produced by calling this function can be fetched on the client-side
  by 'hf list -t raw', alternatively 'hf list -t <proto>' for protocol-specific
  annotation of commands/responses.
**/
bool RAMFUNC LogTrace(const uint8_t *btBytes, uint16_t iLen, uint32_t timestamp_start, uint32_t timestamp_end, const uint8_t *parity, bool reader2tag) {
    if (btBytes == NULL || s_tracing == false) {
        return false;
    }

    // Ignore too-small or too-large logs
    if (iLen == 0 || iLen >= (1 << 15)) {
        return false;
    }

    // number of valid paritybytes in *parity
    const uint16_t num_paritybytes = (iLen - 1) / 8 + 1;

    // Disable tracing and return when trace is full
    const uint32_t max_trace_len = BigBuf_max_traceLen();
    const uint32_t trace_entry_len = TRACELOG_HDR_LEN + iLen + num_paritybytes;
    if (s_trace_len >= max_trace_len || trace_entry_len >= max_trace_len - s_trace_len) {
        s_tracing = false;
        return false;
    }

    uint32_t duration;
    if (timestamp_end > timestamp_start) {
        duration = timestamp_end - timestamp_start;
    } else {
        duration = (UINT32_MAX - timestamp_start) + timestamp_end;
    }

    if (duration > 0xFFFF) {
        /*
        if (g_dbglevel >= DBG_DEBUG) {
            Dbprintf("Error in LogTrace: duration too long for 16 bits encoding: 0x%08x   start: 0x%08x end: 0x%08x", duration, timestamp_start, timestamp_end);
        }
        */
        duration = 0xFFFF;
    }

    tracelog_hdr_t *hdr = (tracelog_hdr_t *)(BigBuf_get_addr() + s_trace_len);
    hdr->timestamp = timestamp_start;
    hdr->duration = duration & 0xFFFF;
    hdr->data_len = iLen;
    hdr->isResponse = !reader2tag;
    memcpy(hdr->frame, btBytes, iLen);
    if (parity != NULL) {
        memcpy(&hdr->frame[iLen], parity, num_paritybytes);
    } else {
        memset(&hdr->frame[iLen], 0x00, num_paritybytes);
    }

    s_trace_len += trace_entry_len;
    return true;
}

// specific LogTrace function for ISO15693: the duration needs to be scaled because otherwise it won't fit into a uint16_t
bool LogTrace_ISO15693(const uint8_t *bytes, uint16_t len, uint32_t ts_start, uint32_t ts_end, const uint8_t *parity, bool reader2tag) {
    uint32_t duration = ts_end - ts_start;
    duration /= 32;
    ts_end = ts_start + duration;
    return LogTrace(bytes, len, ts_start, ts_end, parity, reader2tag);
}

// specific LogTrace function for bitstreams: the partial byte size is stored in first parity byte. E.g. bitstream "1100 00100010" -> partial byte: 4 bits
bool RAMFUNC LogTraceBits(const uint8_t *btBytes, uint16_t bitLen, uint32_t timestamp_start, uint32_t timestamp_end, bool reader2tag) {

    if (bitLen == 0) {
        return false;
    }

    uint8_t parity[(nbytes(bitLen) - 1) / 8 + 1];
    memset(parity, 0x00, sizeof(parity));
    // parity has amount of leftover bits.
    parity[0] = bitLen % 8;
    return LogTrace(btBytes, nbytes(bitLen), timestamp_start, timestamp_end, parity, reader2tag);
}

// Emulator memory
int emlSet(const uint8_t *data, uint32_t offset, uint32_t length) {
    uint8_t *mem = BigBuf_get_EM_addr();
    if (mem == NULL) {
        return PM3_EMALLOC;
    }

    if (offset + length <= CARD_MEMORY_SIZE) {
        memcpy(mem + offset, data, length);
        return PM3_SUCCESS;
    }

    Dbprintf("Error, trying to set memory outside of bounds! " _RED_("%d") " > %d", (offset + length), CARD_MEMORY_SIZE);
    return PM3_EOUTOFBOUND;
}

int emlGet(uint8_t *out, uint32_t offset, uint32_t length) {
    uint8_t *mem = BigBuf_get_EM_addr();
    if (mem == NULL) {
        return PM3_EMALLOC;
    }

    if (offset + length <= CARD_MEMORY_SIZE) {
        memcpy(out, mem + offset, length);
        return PM3_SUCCESS;
    }

    Dbprintf("Error, trying to read memory outside of bounds! " _RED_("%d") " > %d", (offset + length), CARD_MEMORY_SIZE);
    return PM3_EOUTOFBOUND;
}


// get the address of the ToSend buffer. Allocate part of Bigbuf for it, if not yet done
tosend_t *get_tosend(void) {

    if (s_toSend.buf == NULL) {
        s_toSend.buf = BigBuf_malloc(TOSEND_BUFFER_SIZE);
    }
    return &s_toSend;
}

void tosend_reset(void) {
    s_toSend.max = -1;
    s_toSend.bit = 8;
}

void tosend_stuffbit(int b) {

    if (s_toSend.max >= TOSEND_BUFFER_SIZE - 1) {
        Dbprintf(_RED_("s_toSend overflow"));
        return;
    }

    if (s_toSend.bit >= 8) {
        s_toSend.max++;
        s_toSend.buf[s_toSend.max] = 0;
        s_toSend.bit = 0;
    }

    if (b)
        s_toSend.buf[s_toSend.max] |= (1 << (7 - s_toSend.bit));

    s_toSend.bit++;

    if (s_toSend.max >= TOSEND_BUFFER_SIZE) {
        s_toSend.bit = 0;
    }
}

dmabuf16_t *get_dma16(void) {
    if (s_dma_16.buf == NULL) {
        s_dma_16.buf = (uint16_t *)BigBuf_malloc(DMA_BUFFER_SIZE * sizeof(uint16_t));
    }

    return &s_dma_16;
}

dmabuf8_t *get_dma8(void) {
    if (s_dma_8.buf == NULL)
        s_dma_8.buf = BigBuf_malloc(DMA_BUFFER_SIZE);

    return &s_dma_8;
}
