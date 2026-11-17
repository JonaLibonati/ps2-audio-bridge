
"""PS2 Audio Bridge v0.3: UDP PCM receiver with adaptive rebuffering.

Transport proof of concept only: the PS2 ELF sends a synthetic test tone,
not audio captured from commercial games.
"""
import argparse
import queue
import socket
import struct
import threading
import time

MAGIC = b'P2AB'
RATE = 48000
CHANNELS = 2
FRAME_BYTES = 4  # little-endian signed 16-bit stereo
PACKET_FRAMES = 240  # 5 ms per datagram
PACKET_MS = 5
HEADER = struct.Struct('<4sI')
EXPECTED_BYTES = HEADER.size + PACKET_FRAMES * FRAME_BYTES


def main():
    parser = argparse.ArgumentParser(description='PS2 Audio Bridge v0.3 — receptor UDP')
    parser.add_argument('--host', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=19752)
    parser.add_argument('--device', default='JBL', help='Fragmento del nombre o indice de dispositivo')
    parser.add_argument('--list-devices', action='store_true')
    parser.add_argument('--buffer-ms', type=int, default=120, help='Búfer inicial, 20–300 ms')
    parser.add_argument('--max-buffer-ms', type=int, default=240, help='Límite del búfer adaptativo, hasta 500 ms')
    args = parser.parse_args()
    if not 20 <= args.buffer_ms <= 300:
        parser.error('--buffer-ms debe estar entre 20 y 300')
    if not args.buffer_ms <= args.max_buffer_ms <= 500:
        parser.error('--max-buffer-ms debe ser >= --buffer-ms y <= 500')

    try:
        import sounddevice as sd
    except ImportError:
        raise SystemExit('Instala sounddevice: python3 -m pip install -r requirements.txt')

    if args.list_devices:
        print(sd.query_devices())
        return

    device = None
    if args.device:
        if args.device.isdecimal():
            device = int(args.device)
        else:
            matches = [(i, d) for i, d in enumerate(sd.query_devices())
                       if args.device.lower() in d['name'].lower() and d['max_output_channels'] >= CHANNELS]
            if not matches:
                raise SystemExit(f'No hay salida estéreo que coincida con {args.device!r}; usa --list-devices')
            device = matches[0][0]
            print('Salida elegida:', matches[0][1]['name'], flush=True)

    packets = queue.Queue(maxsize=120)  # 600 ms máximo; descartamos los más viejos si se llena
    stats = dict(rx=0, invalid=0, missing=0, overflow=0, rebuffer=0,
                 end_of_stream=0, late10=0, late20=0, max_gap_ms=0.0,
                 callback_flags=0)
    stop = threading.Event()
    ready = threading.Event()
    network_error = []
    last_rx = [0.0]
    expected = [None]

    def network(sock):
        previous_arrival = None
        try:
            ready.set()
            while not stop.is_set():
                try:
                    datagram, sender = sock.recvfrom(4096)
                except socket.timeout:
                    continue
                now = time.perf_counter()
                if len(datagram) != EXPECTED_BYTES:
                    stats['invalid'] += 1
                    continue
                magic, seq = HEADER.unpack_from(datagram)
                if magic != MAGIC:
                    stats['invalid'] += 1
                    continue
                if previous_arrival is not None:
                    gap_ms = (now - previous_arrival) * 1000
                    stats['max_gap_ms'] = max(stats['max_gap_ms'], gap_ms)
                    if gap_ms > 10:
                        stats['late10'] += 1
                    if gap_ms > 20:
                        stats['late20'] += 1
                previous_arrival = now
                if expected[0] is not None:
                    gap = (seq - expected[0]) & 0xFFFFFFFF
                    if 0 < gap < 0x80000000:
                        stats['missing'] += gap
                expected[0] = (seq + 1) & 0xFFFFFFFF
                last_rx[0] = now
                if packets.full():
                    try:
                        packets.get_nowait()
                        stats['overflow'] += 1
                    except queue.Empty:
                        pass
                try:
                    packets.put_nowait(datagram[HEADER.size:])
                    stats['rx'] += 1
                except queue.Full:
                    stats['overflow'] += 1
        except Exception as exc:
            network_error.append(exc)
        finally:
            stop.set()
            sock.close()

    # En caso de bind fallido, terminar antes de abrir el altavoz.
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind((args.host, args.port))
        sock.settimeout(0.2)
    except OSError as exc:
        sock.close()
        raise SystemExit(f'No se pudo abrir UDP {args.host}:{args.port}: {exc}. Cierra otros receptores.')
    threading.Thread(target=network, args=(sock,), daemon=True).start()
    ready.wait(2)
    print(f'Escuchando UDP en {args.host}:{args.port}', flush=True)

    start_packets = max(4, (args.buffer_ms + PACKET_MS - 1) // PACKET_MS)
    max_packets = max(start_packets, (args.max_buffer_ms + PACKET_MS - 1) // PACKET_MS)
    target_packets = [start_packets]
    state = ['priming']  # priming, playing, ended
    pending = bytearray()
    last_underrun_at = [0.0]
    last_started_at = [0.0]

    def callback(outdata, frames, time_info, status):
        required = frames * FRAME_BYTES
        outdata[:] = bytes(required)
        if status:
            stats['callback_flags'] += 1
        now = time.perf_counter()
        if state[0] != 'playing':
            if packets.qsize() < target_packets[0]:
                return
            state[0] = 'playing'
            last_started_at[0] = now
        # El stream tira del búfer a su propio ritmo; no bloquear el callback.
        while len(pending) < required:
            try:
                pending.extend(packets.get_nowait())
            except queue.Empty:
                if last_rx[0] and now - last_rx[0] > 0.3:
                    if state[0] != 'ended':
                        stats['end_of_stream'] += 1
                    state[0] = 'ended'
                else:
                    # Dejar el próximo bloque completo en fase de precarga.
                    stats['rebuffer'] += 1
                    state[0] = 'priming'
                    # Evitar contar varias veces el mismo episodio.
                    if now - last_underrun_at[0] > 0.15:
                        target_packets[0] = min(max_packets, target_packets[0] + 4)
                        last_underrun_at[0] = now
                pending.clear()
                return
        outdata[:required] = pending[:required]
        del pending[:required]

    try:
        with sd.RawOutputStream(samplerate=RATE, channels=CHANNELS, dtype='int16',
                                blocksize=PACKET_FRAMES, device=device, callback=callback):
            print(f'Audio: {RATE} Hz, 16-bit estéreo. Búfer inicial: {start_packets * 5} ms.')
            print('La prueba transmite un tono generado en PS2; NO captura audio de juegos.')
            while not stop.is_set():
                time.sleep(3)
                print('RX={rx} Perdidos={missing} Invalidos={invalid} Desbordes={overflow} '
                      'Rebuffers={rebuffer} Fin={end_of_stream} Cola={q} '
                      'Buffer={buffer_ms}ms GapMax={max_gap_ms:.1f}ms Gap>20ms={late20} '
                      'CallbackFlags={callback_flags}'.format(
                          **stats, q=packets.qsize(), buffer_ms=target_packets[0] * 5), flush=True)
    except KeyboardInterrupt:
        print('\nDetenido.')
    finally:
        stop.set()
    if network_error:
        raise SystemExit(f'Error de red: {network_error[0]}')


if __name__ == '__main__':
    main()
