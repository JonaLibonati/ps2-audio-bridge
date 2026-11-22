
/*
 * SPU2 Capture Probe - EE launcher
 *
 * Standalone program for real PS2 hardware.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <kernel.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <debug.h>

#define RPC_ID 0x53505532

extern unsigned char freesd_irx[];
extern unsigned int size_freesd_irx;

extern unsigned char spu2probe_irx[];
extern unsigned int size_spu2probe_irx;

typedef struct {
    int status;
    int dma_write;
    int dma_read;
    unsigned int mismatches;
    unsigned int left_nonzero;
    unsigned int right_nonzero;
    int left_min;
    int left_max;
    int right_min;
    int right_max;
    unsigned int left_first;
    unsigned int right_first;
} probe_result_t;

static SifRpcClientData_t rpc_client
    __attribute__((aligned(64)));

static unsigned int rpc_send[16]
    __attribute__((aligned(64)));

static probe_result_t rpc_result
    __attribute__((aligned(64)));

static void init_iop(void)
{
    SifInitRpc(0);

    while (!SifIopReset("", 0)) {
    }

    while (!SifIopSync()) {
    }

    SifInitRpc(0);
    SifLoadFileInit();
    SifInitIopHeap();

    sbv_patch_enable_lmb();
}

int main(void)
{
    int ret;
    int i;

    init_scr();

    scr_printf("SPU2 Capture Probe v0.2\n");
    scr_printf("------------------------------\n");
    scr_printf("Standalone DMA memory test\n");
    scr_printf("No games / no OPL integration\n\n");

    init_iop();

    scr_printf("Loading freesd...\n");

    ret = SifExecModuleBuffer(
        freesd_irx,
        size_freesd_irx,
        0,
        NULL,
        NULL
    );

    scr_printf("freesd module ID: %d\n", ret);

    if (ret < 0) {
        goto halt;
    }

    scr_printf("Loading SPU2 probe...\n");

    ret = SifExecModuleBuffer(
        spu2probe_irx,
        size_spu2probe_irx,
        0,
        NULL,
        NULL
    );

    scr_printf("probe module ID: %d\n", ret);

    if (ret < 0) {
        goto halt;
    }

    memset(&rpc_client, 0, sizeof(rpc_client));

    scr_printf("Connecting RPC...\n");

    for (i = 0; i < 100; ++i) {
        ret = SifBindRpc(
            &rpc_client,
            RPC_ID,
            0
        );

        if (ret >= 0 && rpc_client.server != NULL) {
            break;
        }

        usleep(100000);
    }

    if (i == 100) {
        scr_printf("RPC timeout\n");
        goto halt;
    }

    memset(rpc_send, 0, sizeof(rpc_send));
    memset(&rpc_result, 0, sizeof(rpc_result));

    ret = SifCallRpc(
        &rpc_client,
        0,
        0,
        rpc_send,
        sizeof(rpc_send),
        &rpc_result,
        sizeof(rpc_result),
        NULL,
        NULL
    );

    if (ret < 0) {
        scr_printf("RPC call failed: %d\n", ret);
        goto halt;
    }

    scr_printf("\n--- DMA TEST ---\n");
    scr_printf("Status: %d\n", rpc_result.status);
    scr_printf("Write: %d bytes\n", rpc_result.dma_write);
    scr_printf("Read:  %d bytes\n", rpc_result.dma_read);
    scr_printf("Mismatches: %u\n", rpc_result.mismatches);

    scr_printf("\n--- CORE 0 BUFFERS ---\n");

    scr_printf(
        "Left  nonzero=%u min=%d max=%d\n",
        rpc_result.left_nonzero,
        rpc_result.left_min,
        rpc_result.left_max
    );

    scr_printf(
        "Right nonzero=%u min=%d max=%d\n",
        rpc_result.right_nonzero,
        rpc_result.right_min,
        rpc_result.right_max
    );

    scr_printf(
        "First L=%08X R=%08X\n",
        rpc_result.left_first,
        rpc_result.right_first
    );

    if (rpc_result.status == 0 &&
        rpc_result.mismatches == 0) {
        scr_printf("\nDMA round-trip PASSED\n");
    } else {
        scr_printf("\nDMA round-trip FAILED\n");
    }

halt:
    scr_printf("\nSafe to restart PS2.\n");

    while (1) {
        SleepThread();
    }

    return 0;
}
