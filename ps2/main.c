
/*
 * PS2 Audio Bridge v0.3 - UDP tone TEST ONLY.
 *
 * Generates synthetic PCM on the Emotion Engine.
 * Does NOT capture SPU2 audio or run alongside games.
 *
 * Improvements:
 * - Absolute-time packet scheduling (200 packets/second)
 * - EE hardware timer for transmission pacing
 * - Explicit end-of-stream packet: P2AE
 * - Transmission duration diagnostics
 *
 * Audio protocol:
 *   P2AB + uint32 LE sequence + 960 bytes PCM
 *
 * EOS protocol:
 *   P2AE + uint32 LE total_packets
 *
 * PCM format:
 *   48000 Hz, signed 16-bit LE, stereo
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <kernel.h>
#include <timer.h>
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

#define AUDIO_FRAMES       240
#define RATE               48000
#define HEADER_LEN         8

#define PACKET_LEN \
    (HEADER_LEN + AUDIO_FRAMES * 4)

#define PACKETS_PER_SECOND \
    (RATE / AUDIO_FRAMES)

#define TOTAL_PACKETS \
    (TEST_SECONDS * PACKETS_PER_SECOND)

/*
 * Each packet represents 5 ms of PCM.
 *
 * EE BUSCLK: 147456000 Hz
 * At 200 packets/s: 737280 bus cycles/packet.
 */
#define TICKS_PER_PACKET \
    ((uint64_t)kBUSCLK / PACKETS_PER_SECOND)

#define SPIN_THRESHOLD_US  300

static unsigned char packet[PACKET_LEN];
static unsigned char eos_packet[HEADER_LEN];

static uint32_t phase = 0;

/* 440 Hz * 2^32 / 48000 */
static const uint32_t phase_increment = 39370534U;


/* ---------------------------------------------------------
 * PCM generator
 * --------------------------------------------------------- */

static void make_packet(uint32_t seq)
{
    int i;

    memcpy(packet, "P2AB", 4);

    packet[4] = (unsigned char)seq;
    packet[5] = (unsigned char)(seq >> 8);
    packet[6] = (unsigned char)(seq >> 16);
    packet[7] = (unsigned char)(seq >> 24);

    for (i = 0; i < AUDIO_FRAMES; ++i) {

        /* Low-amplitude triangle wave; no libm required. */
        int32_t v = (phase & 0x80000000U)
            ? (int32_t)((0xFFFFFFFFU - phase) >> 16)
            : (int32_t)(phase >> 16);

        int16_t sample =
            (int16_t)((v - 16384) / 3);

        int off = HEADER_LEN + i * 4;

        packet[off] =
            (unsigned char)sample;

        packet[off + 1] =
            (unsigned char)((uint16_t)sample >> 8);

        packet[off + 2] = packet[off];
        packet[off + 3] = packet[off + 1];

        phase += phase_increment;
    }
}


/* ---------------------------------------------------------
 * End-of-stream packet
 * --------------------------------------------------------- */

static void make_eos_packet(uint32_t total_packets)
{
    memcpy(eos_packet, "P2AE", 4);

    eos_packet[4] =
        (unsigned char)total_packets;

    eos_packet[5] =
        (unsigned char)(total_packets >> 8);

    eos_packet[6] =
        (unsigned char)(total_packets >> 16);

    eos_packet[7] =
        (unsigned char)(total_packets >> 24);
}


/* ---------------------------------------------------------
 * EE timer
 * --------------------------------------------------------- */

static int prepare_timer(void)
{
    /*
     * Recent PS2SDK startup code normally initializes
     * the system timer. Check before using it.
     */
    if (GetTimerSystemTime() == 0) {
        _ps2sdk_init_timer();
    }

    if (GetTimerSystemTime() == 0) {
        scr_printf("EE timer unavailable\n");
        return -1;
    }

    return 0;
}


/*
 * Wait until an absolute deadline, expressed in
 * EE bus-clock ticks.
 *
 * Use short sleeps while far from the deadline,
 * followed by a brief busy wait for precision.
 *
 * We never add processing time to the next deadline.
 */
static void wait_until_ticks(uint64_t deadline)
{
    const uint64_t spin_ticks =
        ((uint64_t)kBUSCLK * SPIN_THRESHOLD_US)
        / 1000000ULL;

    for (;;) {
        uint64_t now = GetTimerSystemTime();

        if (now >= deadline) {
            return;
        }

        uint64_t remaining = deadline - now;

        if (remaining > spin_ticks +
                        ((uint64_t)kBUSCLK / 1000ULL)) {

            /* Sleep briefly; leave room for scheduling jitter. */
            usleep(500);

        } else if (remaining > spin_ticks) {

            usleep(100);

        } else {

            /*
             * Final short spin.
             * Prevents the relative usleep(5000) drift.
             */
            while (GetTimerSystemTime() < deadline) {
                /* Busy wait */
            }

            return;
        }
    }
}


/* ---------------------------------------------------------
 * Network initialization - preserved from v0.2
 * --------------------------------------------------------- */

static int init_network(void)
{
    struct ip4_addr ip, mask, gateway;
    int r;

    SifInitRpc(0);

    while (!SifIopReset("", 0)) {
    }

    while (!SifIopSync()) {
    }

    SifInitRpc(0);
    SifLoadFileInit();
    SifInitIopHeap();

    sbv_patch_enable_lmb();

    r = SifExecModuleBuffer(
        DEV9_irx,
        size_DEV9_irx,
        0,
        NULL,
        NULL
    );

    if (r < 0) {
        scr_printf("DEV9 failed: %d\n", r);
        return -1;
    }

    r = SifExecModuleBuffer(
        NETMAN_irx,
        size_NETMAN_irx,
        0,
        NULL,
        NULL
    );

    if (r < 0) {
        scr_printf("NETMAN failed: %d\n", r);
        return -1;
    }

    r = SifExecModuleBuffer(
        SMAP_irx,
        size_SMAP_irx,
        0,
        NULL,
        NULL
    );

    if (r < 0) {
        scr_printf("SMAP failed: %d\n", r);
        return -1;
    }

    r = NetManInit();

    if (r < 0) {
        scr_printf("NetManInit failed: %d\n", r);
        return -1;
    }

    IP4_ADDR(
        &ip,
        PS2_IP_A,
        PS2_IP_B,
        PS2_IP_C,
        PS2_IP_D
    );

    IP4_ADDR(
        &mask,
        MASK_A,
        MASK_B,
        MASK_C,
        MASK_D
    );

    IP4_ADDR(
        &gateway,
        PC_IP_A,
        PC_IP_B,
        PC_IP_C,
        PC_IP_D
    );

    r = ps2ipInit(&ip, &mask, &gateway);

    if (r < 0) {
        scr_printf("ps2ipInit failed: %d\n", r);
        return -1;
    }

    return 0;
}


/* ---------------------------------------------------------
 * Main
 * --------------------------------------------------------- */

int main(int argc, char **argv)
{
    struct sockaddr_in target;
    struct ip4_addr dest;

    int sock;
    int i;
    int sent;
    int errors = 0;
    int eos_errors = 0;
    int late_packets = 0;

    uint64_t start_ticks;
    uint64_t end_ticks;
    uint64_t deadline;
    uint64_t now;

    uint32_t elapsed_ms;
    uint32_t total_packets = TOTAL_PACKETS;

    (void)argc;
    (void)argv;

    init_scr();

    scr_printf("PS2 Audio Bridge v0.3 - UDP TEST\n");
    scr_printf("Absolute timer + EOS\n");
    scr_printf("No SPU2 capture, no OPL integration\n");

    scr_printf(
        "PS2 %d.%d.%d.%d -> PC %d.%d.%d.%d:%d\n",
        PS2_IP_A,
        PS2_IP_B,
        PS2_IP_C,
        PS2_IP_D,
        PC_IP_A,
        PC_IP_B,
        PC_IP_C,
        PC_IP_D,
        UDP_PORT
    );

    if (init_network() < 0) {
        goto halt;
    }

    scr_printf("Waiting for Ethernet link...\n");

    for (i = 0; i < 50; ++i) {

        if (NetManIoctl(
                NETMAN_NETIF_IOCTL_GET_LINK_STATUS,
                NULL,
                0,
                NULL,
                0
            ) == NETMAN_NETIF_ETH_LINK_STATE_UP) {
            break;
        }

        usleep(100000);
    }

    if (i == 50) {
        scr_printf("Ethernet link timeout\n");
        goto halt;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        scr_printf("UDP socket failed: %d\n", sock);
        goto halt;
    }

    memset(&target, 0, sizeof(target));

    target.sin_family = AF_INET;
    target.sin_port = htons(UDP_PORT);

    IP4_ADDR(
        &dest,
        PC_IP_A,
        PC_IP_B,
        PC_IP_C,
        PC_IP_D
    );

    memcpy(
        &target.sin_addr,
        &dest,
        sizeof(target.sin_addr)
    );

    if (prepare_timer() < 0) {
        lwip_close(sock);
        goto halt;
    }

    scr_printf(
        "Sending %d seconds, %d pps...\n",
        TEST_SECONDS,
        PACKETS_PER_SECOND
    );

    /*
     * Prime ARP/network state before precise timing.
     * The first real send may still experience ARP delay.
     */
    usleep(100000);

    start_ticks = GetTimerSystemTime();

    for (i = 0; i < (int)total_packets; ++i) {

        /*
         * Generate packet before the scheduled deadline.
         */
        make_packet((uint32_t)i);

        deadline =
            start_ticks +
            (uint64_t)i * TICKS_PER_PACKET;

        wait_until_ticks(deadline);

        /*
         * Count packets that missed their deadline
         * by more than one complete packet interval.
         */
        now = GetTimerSystemTime();

        if (now > deadline + TICKS_PER_PACKET) {
            ++late_packets;
        }

        sent = sendto(
            sock,
            packet,
            PACKET_LEN,
            0,
            (struct sockaddr *)&target,
            sizeof(target)
        );

        if (sent != PACKET_LEN) {
            ++errors;
        }

        /*
         * Do not call scr_printf inside this loop.
         * Console rendering could disturb pacing.
         */
    }

    /*
     * Mark end at the scheduled end of the 10-second
     * sample interval, not at the last packet's start.
     */
    wait_until_ticks(
        start_ticks +
        (uint64_t)total_packets * TICKS_PER_PACKET
    );

    end_ticks = GetTimerSystemTime();

    make_eos_packet(total_packets);

    /*
    * Send the end-of-stream marker multiple times.
    * UDP does not guarantee delivery.
    *
    * Give the network stack time to finish transmitting
    * before closing the socket and deinitializing ps2ip.
    */

    for (int attempt = 0; attempt < 3; ++attempt) {

        sent = sendto(
            sock,
            eos_packet,
            HEADER_LEN,
            0,
            (struct sockaddr *)&target,
            sizeof(target)
        );

        if (sent != HEADER_LEN) {
            ++eos_errors;
        }

        usleep(20000);
    }

    /* Allow queued network traffic to drain. */
    usleep(200000);


    elapsed_ms = (uint32_t)(
        (end_ticks - start_ticks)
        / ((uint64_t)kBUSCLK / 1000ULL)
    );

    scr_printf("Transmission finished.\n");

    scr_printf(
        "Audio packets: %u / %u\n",
        total_packets - (uint32_t)errors,
        total_packets
    );

    scr_printf(
        "Send errors: %d\n",
        errors
    );

    scr_printf(
        "Late packets: %d\n",
        late_packets
    );

    scr_printf(
        "Elapsed: %u ms\n",
        elapsed_ms
    );

    scr_printf(
        "EOS sent: %s\n",
        eos_errors == 0 ? "YES" : "NO"
    );

    lwip_close(sock);

    ps2ipDeinit();
    NetManDeinit();

halt:
    scr_printf("Safe to restart PS2.\n");

    while (1) {
        SleepThread();
    }

    return 0;
}
