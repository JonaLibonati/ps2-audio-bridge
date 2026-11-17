"""PS2 Audio Bridge: UDP PCM test receiver for Windows.

This is a transport proof of concept, NOT a PS2 SPU2 capture implementation.
"""
import argparse
import queue
import socket
import struct
import threading
import time

MAGIC = b"P2AB"
SAMPLE_RATE = 48000
CHANNELS = 2
BYTES_PER_FRAME = 4  # signed int16 stereo
PACKET_FRAMES = 240  # 5 ms
HEADER = struct.Struct('<4sI')  # magic + sequence


def main():
    parser = argparse.ArgumentParser(description='PS2 Audio Bridge - UDP audio receiver')
    parser.add_argument('--host', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=19752)
    parser.add_argument('--device', default=None, help='Output device index or a case-insensitive name fragment, e.g. JBL')
    parser.add_argument('--list-devices', action='store_true')
    parser.add_argument('--buffer-ms', type=int, default=80, help='Target startup buffer in ms (minimum 20)')
    args = parser.parse_args()

    try:
        import sounddevice as sd
    except ImportError:
        raise SystemExit('Falta sounddevice. Ejecuta: py -m pip install -r requirements.txt')

    if args.list_devices:
        print(sd.query_devices())
        return

    device = None
    if args.device is not None:
        if args.device.isdecimal():
            device = int(args.device)
        else:
            matches = [(i, d) for i, d in enumerate(sd.query_devices())
                       if args.device.lower() in d['name'].lower() and d['max_output_channels'] >= 2]
            if not matches:
                raise SystemExit(f'No encontre una salida stereo con el texto: {args.device!r}. Usa --list-devices.')
            device = matches[0][0]
            print('Salida elegida:', matches[0][1]['name'])

    max_packets = 100
    packets = queue.Queue(maxsize=max_packets)
    stop = threading.Event()
    stats = {'received': 0, 'invalid': 0, 'overflow': 0, 'missing': 0, 'underflow': 0}
    expected = None

    def network():
        nonlocal expected
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((args.host, args.port))
        sock.settimeout(0.5)
        print(f'Escuchando UDP en {args.host}:{args.port}')
        print('Presiona Ctrl+C para detener. Autoriza Python en el firewall solo en red privada.')
        try:
            while not stop.is_set():
                try:
                    datagram, _ = sock.recvfrom(4096)
                except socket.timeout:
                    continue
                if len(datagram) != HEADER.size + PACKET_FRAMES * BYTES_PER_FRAME:
                    stats['invalid'] += 1
                    continue
                magic, seq = HEADER.unpack_from(datagram)
                if magic != MAGIC:
                    stats['invalid'] += 1
                    continue
                if expected is not None:
                    gap = (seq - expected) & 0xFFFFFFFF
                    if 0 < gap < 0x80000000:
                        stats['missing'] += gap
                expected = (seq + 1) & 0xFFFFFFFF
                payload = datagram[HEADER.size:]
                if packets.full():
                    try:
                        packets.get_nowait()  # discard oldest to limit latency
                        stats['overflow'] += 1
                    except queue.Empty:
                        pass
                try:
                    packets.put_nowait(payload)
                    stats['received'] += 1
                except queue.Full:
                    stats['overflow'] += 1
        finally:
            sock.close()

    threading.Thread(target=network, daemon=True).start()
    startup_packets = max(2, min(50, (args.buffer_ms + 4) // 5))
    playing = False
    pending = bytearray()

    def callback(outdata, frames, time_info, status):
        nonlocal playing
        required = frames * BYTES_PER_FRAME
        if not playing:
            if packets.qsize() >= startup_packets:
                playing = True
            else:
                outdata[:] = b'\0' * required
                return
        while len(pending) < required:
            try:
                pending.extend(packets.get_nowait())
            except queue.Empty:
                stats['underflow'] += 1
                playing = False
                break
        available = min(required, len(pending))
        outdata[:available] = pending[:available]
        if available < required:
            outdata[available:] = b'\0' * (required - available)
        del pending[:available]

    try:
        with sd.RawOutputStream(samplerate=SAMPLE_RATE, channels=CHANNELS, dtype='int16',
                                blocksize=PACKET_FRAMES, device=device, callback=callback):
            print(f'Audio: {SAMPLE_RATE} Hz, 16-bit estereo. Buffer inicial: {startup_packets * 5} ms.')
            print('NOTA: Bluetooth agrega latencia; la prueba no la elimina.')
            while True:
                time.sleep(3)
                print('RX={received} Perdidos={missing} Invalidos={invalid} '
                      'Desbordes={overflow} Silencios={underflow} Cola={q} paquetes'.format(
                          **stats, q=packets.qsize()))
    except KeyboardInterrupt:
        print('\nDetenido.')
    finally:
        stop.set()


if __name__ == '__main__':
    main()
