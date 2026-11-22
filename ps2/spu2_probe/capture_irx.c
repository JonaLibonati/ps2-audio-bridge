
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
#include <loadcore.h>
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

/* SPU2 core selectors */
#define SD_CORE_0 0
#define SD_CORE_1 1

#define TEST_VOICE       0
#define TEST_ADPCM_ADDR  0x20000
#define TEST_ADPCM_SIZE  512
#define TEST_READS       20
#define TEST_INTERVAL_US 10000


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
    s32 playback_status;
    s32 playback_transfer;
    u32 playback_reads;
    u32 playback_left_nonzero;
    u32 playback_right_nonzero;
    s32 playback_left_min;
    s32 playback_left_max;
    s32 playback_right_min;
    s32 playback_right_max;
} probe_result_t;

static u8 source[BUFFER_SIZE]
    __attribute__((aligned(64)));
static u8 destination[BUFFER_SIZE]
    __attribute__((aligned(64)));
static u8 left_buffer[BUFFER_SIZE]
    __attribute__((aligned(64)));
static u8 right_buffer[BUFFER_SIZE]
    __attribute__((aligned(64)));
static u8 test_adpcm[TEST_ADPCM_SIZE]
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


/*
 * Create 32 PS-ADPCM frames, each 16 bytes.
 *
 * A frame decodes to 28 PCM samples.
 * Predictor 0, shift 8.
 * Alternating positive and negative blocks produce
 * a recognizable periodic signal.
 *
 * ADPCM loop markers:
 *   first frame: loop start
 *   last frame:  loop end + repeat
 */
static void build_test_sample(void)
{
    int block;
    int j;

    memset(test_adpcm, 0, sizeof(test_adpcm));

    for (block = 0; block < 32; ++block) {
        int offset = block * 16;
        u8 value = (block & 1) ? 0xDD : 0x33;

        test_adpcm[offset] = 0x02;
        test_adpcm[offset + 1] = 0;

        if (block == 0) {
            test_adpcm[offset + 1] = 0x06;
        }

        if (block == 31) {
            test_adpcm[offset + 1] = 0x03;
        }

        for (j = 2; j < 16; ++j) {
            test_adpcm[offset + j] = value;
        }
    }
}

static void accumulate_stats(
    const u8 *buffer,
    u32 *nonzero_total,
    s32 *minimum,
    s32 *maximum
)
{
    u32 nonzero;
    u32 first;
    s32 local_min;
    s32 local_max;

    analyze(
        buffer,
        &nonzero,
        &local_min,
        &local_max,
        &first
    );

    *nonzero_total += nonzero;

    if (local_min < *minimum) {
        *minimum = local_min;
    }

    if (local_max > *maximum) {
        *maximum = local_max;
    }
}

static void run_playback_test(void)
{
    int ret;
    int i;

    result.playback_status = -1;
    result.playback_left_min = 32767;
    result.playback_left_max = -32768;
    result.playback_right_min = 32767;
    result.playback_right_max = -32768;

    build_test_sample();

    /*
     * Ensure test voice 0 is not active.
     */
    sceSdSetSwitch(
        SD_SWITCH_KOFF,
        1U << TEST_VOICE
    );

    /*
     * Upload the ADPCM loop into ordinary SPU2 RAM.
     */
    ret = sceSdVoiceTrans(
        0,
        SD_TRANS_WRITE | SD_TRANS_MODE_DMA,
        test_adpcm,
        (u32 *)TEST_ADPCM_ADDR,
        TEST_ADPCM_SIZE
    );

    result.playback_transfer = ret;

    if (ret < 0) {
        result.playback_status = -10;
        return;
    }

    if (!sceSdVoiceTransStatus(0, 1)) {
        result.playback_status = -11;
        return;
    }

    /*
     * Moderate master volume for the test.
     */
    sceSdSetParam(
        SD_PARAM_MVOLL,
        0x1000
    );

    sceSdSetParam(
        SD_PARAM_MVOLR,
        0x1000
    );


    /*
    * Core 1 is the final output stage for
    * the usual SPU2 core 0 -> core 1 path.
    *
    * Enable moderate master volume on core 1.
    */
    sceSdSetParam(
        SD_CORE_1 | SD_PARAM_MVOLL,
        0x1000
    );

    sceSdSetParam(
        SD_CORE_1 | SD_PARAM_MVOLR,
        0x1000
    );


    /*
     * Voice 0: audible through left and right.
     * Pitch 0x1000 = nominal playback rate.
     */
    sceSdSetParam(
        SD_VPARAM_VOLL | (TEST_VOICE << 1),
        0x1800
    );

    sceSdSetParam(
        SD_VPARAM_VOLR | (TEST_VOICE << 1),
        0x1800
    );

    sceSdSetParam(
        SD_VPARAM_PITCH | (TEST_VOICE << 1),
        0x1000
    );

    /*
     * Fast attack, high sustain level.
     */
    sceSdSetParam(
        SD_VPARAM_ADSR1 | (TEST_VOICE << 1),
        SD_SET_ADSR1(0, 0x7F, 0, 0xF)
    );

    sceSdSetParam(
        SD_VPARAM_ADSR2 | (TEST_VOICE << 1),
        SD_SET_ADSR2(0, 0, 0, 0)
    );

    sceSdSetAddr(
        SD_VADDR_SSA | (TEST_VOICE << 1),
        TEST_ADPCM_ADDR
    );

    /*
     * Enable dry mixing for voice 0 on both channels.
     */
    sceSdSetSwitch(SD_SWITCH_VMIXL, 1U << TEST_VOICE);
    sceSdSetSwitch(SD_SWITCH_VMIXR, 1U << TEST_VOICE);

    /*
     * Start the hardware voice.
     */
    sceSdSetSwitch(
        SD_SWITCH_KON,
        1U << TEST_VOICE
    );

    DelayThread(20000);

    /*
     * Take snapshots while the SPU2 voice plays.
     * These are diagnostic snapshots, NOT a
     * gapless 48 kHz audio capture.
     */
    for (i = 0; i < TEST_READS; ++i) {
        ret = transfer(
            CORE0_LEFT,
            left_buffer,
            SD_TRANS_READ
        );

        if (ret < 0) {
            result.playback_status = -20;
            goto stop_voice;
        }

        accumulate_stats(
            left_buffer,
            &result.playback_left_nonzero,
            &result.playback_left_min,
            &result.playback_left_max
        );

        ret = transfer(
            CORE0_RIGHT,
            right_buffer,
            SD_TRANS_READ
        );

        if (ret < 0) {
            result.playback_status = -30;
            goto stop_voice;
        }

        accumulate_stats(
            right_buffer,
            &result.playback_right_nonzero,
            &result.playback_right_min,
            &result.playback_right_max
        );

        ++result.playback_reads;

        DelayThread(TEST_INTERVAL_US);
    }

    result.playback_status = 0;

stop_voice:
    sceSdSetSwitch(
        SD_SWITCH_KOFF,
        1U << TEST_VOICE
    );
}

static void run_probe(void)
{
    unsigned int i;
    int ret;

    memset(&result, 0, sizeof(result));
    result.status = -1;

    ret = sceSdInit(0);

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

    /*
    * The original DMA round-trip has passed.
    * Now perform the independent playback test.
    */
    run_playback_test();

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
