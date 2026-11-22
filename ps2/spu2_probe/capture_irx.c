
/*
 * SPU2 Capture Probe v0.2
 * IOP module - standalone hardware experiment.
 *
 * Test A: write/read a known DMA pattern in SPU2 RAM.
 * Test B: read core 0 left/right capture regions.
 *
 * No games, no OPL integration, no continuous capture.
 */

#include <types.h>
#include <irx.h>
#include <libsd.h>
#include <sifrpc.h>
#include <thbase.h>
#include <stdio.h>
#include <string.h>

IRX_ID("spu2probe", 1, 0);

#define RPC_ID         0x53505532
#define BUFFER_SIZE    512
#define SCRATCH_ADDR   0x10000
#define CORE0_LEFT     0x0800
#define CORE0_RIGHT    0x0A00

typedef struct {
    s32 status;
    s32 dma_write;
    s32 dma_read;
    u32 mismatches;
    u32 left_nonzero;
    u32 right_nonzero;
    s32 left_min;
    s32 left_max;
    s32 right_min;
    s32 right_max;
    u32 left_first;
    u32 right_first;
} probe_result_t;

static u8 source[BUFFER_SIZE]
    __attribute__((aligned(64)));
static u8 destination[BUFFER_SIZE]
    __attribute__((aligned(64)));
static u8 left_buffer[BUFFER_SIZE]
    __attribute__((aligned(64)));
static u8 right_buffer[BUFFER_SIZE]
    __attribute__((aligned(64)));

static probe_result_t result
    __attribute__((aligned(64)));

static SifRpcDataQueue_t rpc_queue;
static SifRpcServerData_t rpc_server;

static u32 rpc_buffer[16]
    __attribute__((aligned(64)));

static int transfer(
    unsigned int address,
    unsigned char *buffer,
    unsigned short direction
)
{
    int ret;

    ret = sceSdVoiceTrans(
        0,
        direction | SD_TRANS_MODE_DMA,
        buffer,
        (u32 *)address,
        BUFFER_SIZE
    );

    if (ret < 0) {
        return ret;
    }

    if (!sceSdVoiceTransStatus(0, 1)) {
        return -2;
    }

    return ret;
}

static void analyze(
    const u8 *buffer,
    u32 *nonzero,
    s32 *minimum,
    s32 *maximum,
    u32 *first
)
{
    unsigned int i;

    *nonzero = 0;
    *minimum = 32767;
    *maximum = -32768;

    *first =
        (u32)buffer[0] |
        ((u32)buffer[1] << 8) |
        ((u32)buffer[2] << 16) |
        ((u32)buffer[3] << 24);

    for (i = 0; i < BUFFER_SIZE; i += 2) {
        u16 raw = (u16)buffer[i] |
                  ((u16)buffer[i + 1] << 8);

        s32 value = (raw & 0x8000)
            ? (s32)raw - 65536
            : (s32)raw;

        if (value != 0) {
            ++(*nonzero);
        }

        if (value < *minimum) {
            *minimum = value;
        }

        if (value > *maximum) {
            *maximum = value;
        }
    }
}

static void run_probe(void)
{
    unsigned int i;
    int ret;

    memset(&result, 0, sizeof(result));
    result.status = -1;

    ret = sceSdInit(SD_INIT_COLD);

    if (ret < 0) {
        result.status = -10;
        return;
    }

    /*
     * Test A: create a recognizable pattern.
     * Scratch region is used only in this standalone test.
     */
    for (i = 0; i < BUFFER_SIZE; ++i) {
        source[i] = (u8)((i * 37 + 91) & 0xFF);
    }

    memset(destination, 0, BUFFER_SIZE);

    result.dma_write = transfer(
        SCRATCH_ADDR,
        source,
        SD_TRANS_WRITE
    );

    if (result.dma_write < 0) {
        result.status = -20;
        goto done;
    }

    result.dma_read = transfer(
        SCRATCH_ADDR,
        destination,
        SD_TRANS_READ
    );

    if (result.dma_read < 0) {
        result.status = -30;
        goto done;
    }

    for (i = 0; i < BUFFER_SIZE; ++i) {
        if (source[i] != destination[i]) {
            ++result.mismatches;
        }
    }

    if (result.mismatches != 0) {
        result.status = -40;
        goto done;
    }

    /*
     * Test B: inspect core 0 capture buffers.
     * Zero samples may be entirely normal:
     * this test has not started an SPU2 voice.
     */
    memset(left_buffer, 0, BUFFER_SIZE);
    memset(right_buffer, 0, BUFFER_SIZE);

    ret = transfer(
        CORE0_LEFT,
        left_buffer,
        SD_TRANS_READ
    );

    if (ret < 0) {
        result.status = -50;
        goto done;
    }

    ret = transfer(
        CORE0_RIGHT,
        right_buffer,
        SD_TRANS_READ
    );

    if (ret < 0) {
        result.status = -60;
        goto done;
    }

    analyze(
        left_buffer,
        &result.left_nonzero,
        &result.left_min,
        &result.left_max,
        &result.left_first
    );

    analyze(
        right_buffer,
        &result.right_nonzero,
        &result.right_min,
        &result.right_max,
        &result.right_first
    );

    result.status = 0;

done:
    sceSdQuit();
}

static void *rpc_handler(
    int function,
    void *data,
    int size
)
{
    (void)function;
    (void)data;
    (void)size;

    return &result;
}

static void rpc_thread(void *arg)
{
    (void)arg;

    sceSifInitRpc(0);

    sceSifSetRpcQueue(
        &rpc_queue,
        GetThreadId()
    );

    sceSifRegisterRpc(
        &rpc_server,
        RPC_ID,
        (SifRpcFunc_t)rpc_handler,
        rpc_buffer,
        NULL,
        NULL,
        &rpc_queue
    );

    sceSifRpcLoop(&rpc_queue);
}

int _start(int argc, char **argv)
{
    iop_thread_t thread;
    int thread_id;

    (void)argc;
    (void)argv;

    run_probe();

    memset(&thread, 0, sizeof(thread));
    thread.attr = TH_C;
    thread.thread = rpc_thread;
    thread.priority = 40;
    thread.stacksize = 0x2000;

    thread_id = CreateThread(&thread);

    if (thread_id < 0) {
        return MODULE_NO_RESIDENT_END;
    }

    if (StartThread(thread_id, NULL) < 0) {
        return MODULE_NO_RESIDENT_END;
    }

    return MODULE_RESIDENT_END;
}
