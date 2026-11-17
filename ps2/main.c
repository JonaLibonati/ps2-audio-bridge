/* PS2 Audio Bridge v0.2: PS2 -> Windows UDP tone TEST ONLY.
 * Generates synthetic PCM on the EE. It does NOT capture SPU2 or run with OPL.
 * Networking based on documented ps2sdk EE ps2ip + netman/smap model.
 * This source is UNCOMPILED/UNTESTED against real PS2 hardware.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <kernel.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <debug.h>
#include <netman.h>
#include <ps2ip.h>
#include "config.h"

extern unsigned char DEV9_irx[];
extern unsigned int size_DEV9_irx;
extern unsigned char NETMAN_irx[];
extern unsigned int size_NETMAN_irx;
extern unsigned char SMAP_irx[];
extern unsigned int size_SMAP_irx;

#define AUDIO_FRAMES 240
#define RATE 48000
#define HEADER_LEN 8
#define PACKET_LEN (HEADER_LEN + AUDIO_FRAMES * 4)
static unsigned char packet[PACKET_LEN];
static uint32_t phase = 0;
static const uint32_t phase_increment = 39370534U; /* 440 * 2^32 / 48000 */

static void make_packet(uint32_t seq)
{
    int i;
    memcpy(packet, "P2AB", 4);
    packet[4] = (unsigned char)seq;
    packet[5] = (unsigned char)(seq >> 8);
    packet[6] = (unsigned char)(seq >> 16);
    packet[7] = (unsigned char)(seq >> 24);
    for (i = 0; i < AUDIO_FRAMES; ++i) {
        /* low-amplitude triangle wave, avoids floating-point libm */
        int32_t v = (phase & 0x80000000U)
            ? (int32_t)((0xFFFFFFFFU - phase) >> 16)
            : (int32_t)(phase >> 16);
        int16_t sample = (int16_t)((v - 16384) / 3);
        int off = HEADER_LEN + i * 4;
        packet[off] = (unsigned char)sample;
        packet[off + 1] = (unsigned char)((uint16_t)sample >> 8);
        packet[off + 2] = packet[off];
        packet[off + 3] = packet[off + 1];
        phase += phase_increment;
    }
}

static int init_network(void)
{
    struct ip4_addr ip, mask, gateway;
    int r;
    SifInitRpc(0);
    while (!SifIopReset("", 0)) { }
    while (!SifIopSync()) { }
    SifInitRpc(0);
    SifLoadFileInit();
    SifInitIopHeap();
    sbv_patch_enable_lmb();
    r = SifExecModuleBuffer(DEV9_irx, size_DEV9_irx, 0, NULL, NULL);
    if (r < 0) { scr_printf("DEV9 failed: %d\n", r); return -1; }
    r = SifExecModuleBuffer(NETMAN_irx, size_NETMAN_irx, 0, NULL, NULL);
    if (r < 0) { scr_printf("NETMAN failed: %d\n", r); return -1; }
    r = SifExecModuleBuffer(SMAP_irx, size_SMAP_irx, 0, NULL, NULL);
    if (r < 0) { scr_printf("SMAP failed: %d\n", r); return -1; }
    r = NetManInit();
    if (r < 0) { scr_printf("NetManInit failed: %d\n", r); return -1; }
    IP4_ADDR(&ip, PS2_IP_A, PS2_IP_B, PS2_IP_C, PS2_IP_D);
    IP4_ADDR(&mask, MASK_A, MASK_B, MASK_C, MASK_D);
    IP4_ADDR(&gateway, PC_IP_A, PC_IP_B, PC_IP_C, PC_IP_D);
    r = ps2ipInit(&ip, &mask, &gateway);
    if (r < 0) { scr_printf("ps2ipInit failed: %d\n", r); return -1; }
    return 0;
}

int main(int argc, char **argv)
{
    struct sockaddr_in target;
    struct ip4_addr dest;
    int sock, i, sent, errors = 0;
    (void)argc; (void)argv;
    init_scr();
    scr_printf("PS2 Audio Bridge v0.2 - UDP TEST\n");
    scr_printf("No SPU2 capture, no OPL integration\n");
    scr_printf("PS2 %d.%d.%d.%d -> PC %d.%d.%d.%d:%d\n",
        PS2_IP_A, PS2_IP_B, PS2_IP_C, PS2_IP_D,
        PC_IP_A, PC_IP_B, PC_IP_C, PC_IP_D, UDP_PORT);
    if (init_network() < 0) goto halt;
    /* Give the SMAP interface time to establish link on direct Ethernet. */
    scr_printf("Waiting for Ethernet link...\n");
    for (i = 0; i < 50; ++i) {
        if (NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0)
            == NETMAN_NETIF_ETH_LINK_STATE_UP) break;
        usleep(100000);
    }
    if (i == 50) { scr_printf("Ethernet link timeout\n"); goto halt; }
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { scr_printf("UDP socket failed: %d\n", sock); goto halt; }
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(UDP_PORT);
    IP4_ADDR(&dest, PC_IP_A, PC_IP_B, PC_IP_C, PC_IP_D);
    memcpy(&target.sin_addr, &dest, sizeof(target.sin_addr));
    scr_printf("Sending %d seconds, 200 pps...\n", TEST_SECONDS);
    for (i = 0; i < TEST_SECONDS * 200; ++i) {
        make_packet((uint32_t)i);
        sent = sendto(sock, packet, PACKET_LEN, 0,
                      (struct sockaddr *)&target, sizeof(target));
        if (sent != PACKET_LEN) ++errors;
        if ((i + 1) % 200 == 0) scr_printf("sent %d / %d, errors %d\n",
                           i + 1, TEST_SECONDS * 200, errors);
        usleep(5000); /* approximate pace, NOT a real-time scheduler */
    }
    lwip_close(sock);
    scr_printf("Finished. Send errors: %d\n", errors);
    ps2ipDeinit();
    NetManDeinit();
halt:
    scr_printf("Safe to restart PS2.\n");
    while (1) SleepThread();
    return 0;
}
