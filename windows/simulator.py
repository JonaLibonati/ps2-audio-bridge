"""Generate an original sine tone and send 48 kHz/16-bit stereo PCM over UDP."""
import argparse
import math
import socket
import struct
import time

RATE = 48000
SAMPLES = 240  # 5 ms per packet
MAGIC = b'P2AB'


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--host', default='127.0.0.1')
    p.add_argument('--port', type=int, default=19752)
    p.add_argument('--frequency', type=float, default=440.0)
    p.add_argument('--seconds', type=float, default=20.0)
    args = p.parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    seq = 0
    start = time.perf_counter()
    deadline = start
    print(f'Enviando tono de {args.frequency:g} Hz a {args.host}:{args.port} por {args.seconds:g} s')
    try:
        while time.perf_counter() - start < args.seconds:
            payload = bytearray(SAMPLES * 4)
            for i in range(SAMPLES):
                sample = int(6000 * math.sin(2 * math.pi * args.frequency * ((seq * SAMPLES + i) / RATE)))
                struct.pack_into('<hh', payload, i * 4, sample, sample)
            sock.sendto(struct.pack('<4sI', MAGIC, seq) + payload, (args.host, args.port))
            seq = (seq + 1) & 0xFFFFFFFF
            deadline += 0.005
            delay = deadline - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()
        print('Finalizado.')


if __name__ == '__main__':
    main()
