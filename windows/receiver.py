
"""PS2 Audio Bridge v0.4 - receptor UDP para Windows.

Protocolo:
  P2AB + uint32 little-endian (secuencia) + 960 bytes PCM
  P2AE + uint32 little-endian (total de paquetes), opcional

PCM: 48000 Hz, estéreo, signed 16-bit little-endian.
"""

import argparse
import queue
import socket
import struct
import threading
import time

MAGIC_AUDIO = b"P2AB"
MAGIC_END = b"P2AE"

RATE = 48000
CHANNELS = 2
FRAME_BYTES = 4
PACKET_FRAMES = 240
PACKET_MS = 5

HEADER = struct.Struct("<4sI")
AUDIO_SIZE = HEADER.size + PACKET_FRAMES * FRAME_BYTES
END_TIMEOUT = 0.5


def main():
    parser = argparse.ArgumentParser(
        description="PS2 Audio Bridge v0.4"
    )
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=19752)
    parser.add_argument("--device", default="JBL")
    parser.add_argument("--list-devices", action="store_true")
    parser.add_argument("--buffer-ms", type=int, default=200)
    parser.add_argument("--max-buffer-ms", type=int, default=350)
    args = parser.parse_args()

    if not 20 <= args.buffer_ms <= 500:
        parser.error("--buffer-ms debe estar entre 20 y 500")

    if not args.buffer_ms <= args.max_buffer_ms <= 1000:
        parser.error(
            "--max-buffer-ms debe ser >= --buffer-ms y <= 1000"
        )

    try:
        import sounddevice as sd
    except ImportError:
        raise SystemExit(
            "Instala sounddevice: python3 -m pip install sounddevice"
        )

    if args.list_devices:
        print(sd.query_devices())
        return

    device = None

    if args.device:
        if args.device.isdecimal():
            device = int(args.device)
        else:
            matches = [
                (i, d)
                for i, d in enumerate(sd.query_devices())
                if args.device.lower() in d["name"].lower()
                and d["max_output_channels"] >= CHANNELS
            ]

            if not matches:
                raise SystemExit(
                    f"No se encontro salida estereo: {args.device}"
                )

            device = matches[0][0]
            print("Salida elegida:", matches[0][1]["name"])

    packets = queue.Queue(maxsize=200)

    target = [
        max(4, (args.buffer_ms + PACKET_MS - 1) // PACKET_MS)
    ]
    max_target = max(
        target[0],
        (args.max_buffer_ms + PACKET_MS - 1) // PACKET_MS
    )

    stats = {
        "rx": 0,
        "missing": 0,
        "invalid": 0,
        "overflow": 0,
        "rebuffer": 0,
        "end": 0,
        "eos": 0,
        "callback_flags": 0,
        "late20": 0,
        "max_gap": 0.0,
        "first_arrival": None,
        "last_arrival": None,
    }

    stop = threading.Event()
    network_errors = []

    state = {
        "mode": "priming",
        "pending": bytearray(),
        "had_underrun": False,
        "end_received": False,
        "last_rx": None,
        "expected": None,
    }

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    try:
        sock.bind((args.host, args.port))
        sock.settimeout(0.2)
    except OSError as exc:
        sock.close()
        raise SystemExit(
            f"No se pudo abrir UDP {args.host}:{args.port}: {exc}"
        )

    def network():
        try:
            while not stop.is_set():
                try:
                    data, addr = sock.recvfrom(4096)
                except socket.timeout:
                    continue
                except OSError:
                    if stop.is_set():
                        break
                    raise

                now = time.perf_counter()

                if len(data) < HEADER.size:
                    stats["invalid"] += 1
                    continue

                magic, seq = HEADER.unpack_from(data)

                if magic == MAGIC_END and len(data) == HEADER.size:
                    state["end_received"] = True
                    stats["eos"] += 1
                    continue

                if magic != MAGIC_AUDIO or len(data) != AUDIO_SIZE:
                    stats["invalid"] += 1
                    continue

                if state["mode"] == "ended":
                    # Para otra prueba, reiniciar el receptor.
                    continue

                previous = stats["last_arrival"]

                if previous is None:
                    stats["first_arrival"] = now
                else:
                    gap = (now - previous) * 1000.0
                    stats["max_gap"] = max(stats["max_gap"], gap)

                    if gap > 20.0:
                        stats["late20"] += 1

                stats["last_arrival"] = now
                state["last_rx"] = now

                expected = state["expected"]

                if expected is not None:
                    delta = (seq - expected) & 0xFFFFFFFF

                    if 0 < delta < 0x80000000:
                        stats["missing"] += delta

                state["expected"] = (seq + 1) & 0xFFFFFFFF

                if packets.full():
                    try:
                        packets.get_nowait()
                        stats["overflow"] += 1
                    except queue.Empty:
                        pass

                try:
                    packets.put_nowait(data[HEADER.size:])
                    stats["rx"] += 1
                except queue.Full:
                    stats["overflow"] += 1

        except Exception as exc:
            network_errors.append(exc)
            stop.set()

    thread = threading.Thread(target=network, daemon=True)
    thread.start()

    print(
        f"Escuchando UDP en {args.host}:{args.port}",
        flush=True
    )

    print(
        f"Audio: {RATE} Hz, 16-bit estereo | "
        f"Buffer: {target[0] * PACKET_MS} ms | "
        f"Maximo: {max_target * PACKET_MS} ms",
        flush=True
    )

    def mark_finished():
        if state["mode"] != "ended":
            state["mode"] = "ended"
            state["had_underrun"] = False
            stats["end"] += 1

    def callback(outdata, frames, time_info, status):
        required = frames * FRAME_BYTES
        outdata[:] = bytes(len(outdata))

        if status:
            stats["callback_flags"] += 1

        if state["mode"] == "ended":
            return

        now = time.perf_counter()
        last_rx = state["last_rx"]

        quiet = (
            last_rx is not None
            and now - last_rx >= END_TIMEOUT
        )

        ending = state["end_received"] or quiet

        # Esperar la precarga inicial o la recuperacion.
        if state["mode"] != "playing":
            available = packets.qsize()

            if ending and available == 0 and not state["pending"]:
                mark_finished()
                return

            # Al terminar, reproducir tambien los ultimos
            # paquetes aunque no alcancen el buffer objetivo.
            enough = (
                available >= target[0]
                or (ending and available > 0)
                or bool(state["pending"])
            )

            if not enough:
                return

            if state["had_underrun"] and not ending:
                stats["rebuffer"] += 1
                target[0] = min(max_target, target[0] + 4)

            state["had_underrun"] = False
            state["mode"] = "playing"

        pending = state["pending"]

        # Completar el bloque solicitado sin bloquear el callback.
        while len(pending) < required:
            try:
                pending.extend(packets.get_nowait())
            except queue.Empty:
                break

        copied = min(required, len(pending))

        if copied:
            outdata[:copied] = pending[:copied]
            del pending[:copied]

        if copied < required:
            # Se reprodujeron todas las muestras disponibles.
            # El resto del bloque queda en silencio.
            if ending:
                mark_finished()
            else:
                # No descartar muestras pendientes ni contar
                # todavia una interrupcion como Rebuffer.
                state["had_underrun"] = True
                state["mode"] = "priming"

    def report():
        first = stats["first_arrival"]
        last = stats["last_arrival"]
        rx = stats["rx"]

        pps = 0.0

        if first is not None and last is not None and last > first:
            pps = max(0, rx - 1) / (last - first)

        print(
            f"RX={rx} "
            f"Perdidos={stats['missing']} "
            f"Invalidos={stats['invalid']} "
            f"Desbordes={stats['overflow']} "
            f"Rebuffers={stats['rebuffer']} "
            f"Fin={stats['end']} "
            f"EOS={stats['eos']} "
            f"Cola={packets.qsize()} "
            f"Buffer={target[0] * PACKET_MS}ms "
            f"PPS={pps:.2f} "
            f"GapMax={stats['max_gap']:.1f}ms "
            f"Gap>20ms={stats['late20']} "
            f"CallbackFlags={stats['callback_flags']}",
            flush=True
        )

    try:
        with sd.RawOutputStream(
            samplerate=RATE,
            channels=CHANNELS,
            dtype="int16",
            blocksize=PACKET_FRAMES,
            device=device,
            callback=callback,
        ):
            print("Presiona Ctrl+C para detener.")
            print("Prueba: tono sintetico de PS2, no audio de juegos.")

            while not stop.is_set():
                time.sleep(3)
                report()

    except KeyboardInterrupt:
        print("\nDetenido por el usuario.")

    finally:
        stop.set()
        sock.close()
        thread.join(timeout=1.0)

    if network_errors:
        raise SystemExit(
            f"Error en el receptor UDP: {network_errors[0]}"
        )


if __name__ == "__main__":
    main()
