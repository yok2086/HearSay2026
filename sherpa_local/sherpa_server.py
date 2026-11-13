#!/usr/bin/env python3
"""Speech-to-text server for the Ultra96 using sherpa-onnx (k2-fsa).

Drop-in replacement for whisper.cpp's whisper-server: same address
(127.0.0.1:8090), same request (POST /inference, multipart field "file" = WAV)
and same reply ({"text": "..."}), so the HearSay app works with either one
unchanged. Start whichever you want with its run_server.sh; only one can use
the port at a time.

Usage:  python3 sherpa_server.py [--model DIR] [--port 8090] [--threads 4]
"""
import argparse
import glob
import io
import json
import re
import os
import sys
import threading
import time
import wave
from email.parser import BytesParser
from email.policy import HTTP
from http.server import BaseHTTPRequestHandler, HTTPServer

import numpy as np
import sherpa_onnx

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MODEL = os.path.join(HERE, "models", "sherpa-onnx-streaming-zipformer-en-2023-06-26")


def find(model_dir, pattern):
    """Pick the model file: prefer the 8-bit (int8) version when there is one."""
    files = sorted(glob.glob(os.path.join(model_dir, pattern)))
    if not files:
        sys.exit(f"No {pattern} in {model_dir} - run ./setup.sh first")
    int8 = [f for f in files if ".int8." in f]
    return (int8 or files)[0]


def load_recognizer(model_dir, threads, decoding):
    # Decoder in full precision (tiny, and more accurate); encoder/joiner 8-bit (fast).
    decoders = [f for f in glob.glob(os.path.join(model_dir, "decoder-*.onnx")) if ".int8." not in f]
    kwargs = dict(
        tokens=os.path.join(model_dir, "tokens.txt"),
        encoder=find(model_dir, "encoder-*.onnx"),
        decoder=decoders[0] if decoders else find(model_dir, "decoder-*.onnx"),
        joiner=find(model_dir, "joiner-*.onnx"),
        num_threads=threads,
        sample_rate=16000,
        feature_dim=80,
        decoding_method=decoding,
    )
    if decoding == "modified_beam_search":
        kwargs["max_active_paths"] = 4
    print("Loading", {k: os.path.basename(str(v)) for k, v in kwargs.items()}, flush=True)
    return sherpa_onnx.OnlineRecognizer.from_transducer(**kwargs)


def read_wav(data):
    with wave.open(io.BytesIO(data)) as w:
        rate, width, channels = w.getframerate(), w.getsampwidth(), w.getnchannels()
        frames = w.readframes(w.getnframes())
    if width != 2:
        raise ValueError(f"expected 16-bit WAV, got {8 * width}-bit")
    samples = np.frombuffer(frames, dtype=np.int16).astype(np.float32) / 32768.0
    if channels > 1:
        samples = samples.reshape(-1, channels).mean(axis=1)
    return rate, samples


FIXES_FILE = os.path.join(HERE, "fixes.txt")
_fixes_cache = (None, [])


def load_fixes():
    """fixes.txt: one "wrong -> right" per line (whole words, any case). Re-read when it changes."""
    global _fixes_cache
    try:
        mtime = os.path.getmtime(FIXES_FILE)
    except OSError:
        return []
    if _fixes_cache[0] != mtime:
        rules = []
        with open(FIXES_FILE, encoding="utf-8") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if "->" not in line:
                    continue
                wrong, right = (part.strip() for part in line.split("->", 1))
                if wrong:
                    rules.append((re.compile(r"\b" + re.escape(wrong) + r"\b", re.IGNORECASE), right))
        _fixes_cache = (mtime, rules)
    return _fixes_cache[1]


def apply_fixes(text):
    for pattern, right in load_fixes():
        text = pattern.sub(right, text)
    return text


def sentence_case(text):
    """The English models output CAPITALS ("HELLO HOW ARE YOU")."""
    text = (" " + text.strip().lower() + " ").replace(" i ", " I ").replace(" i'", " I'").strip()
    return text[:1].upper() + text[1:]


class Server:
    def __init__(self, recognizer):
        self.recognizer = recognizer
        self.lock = threading.Lock()  # one clip at a time (4 cores are shared anyway)

    def transcribe(self, wav_bytes):
        rate, samples = read_wav(wav_bytes)
        with self.lock:
            stream = self.recognizer.create_stream()
            stream.accept_waveform(rate, samples)
            # A little silence so the end of the last word is decoded too.
            stream.accept_waveform(rate, np.zeros(int(0.5 * rate), dtype=np.float32))
            stream.input_finished()
            while self.recognizer.is_ready(stream):
                self.recognizer.decode_stream(stream)
            result = self.recognizer.get_result(stream)
        text = result if isinstance(result, str) else getattr(result, "text", "")
        text = apply_fixes(sentence_case(text))  # fixes keep their own capitals (e.g. HearSay)
        return text[:1].upper() + text[1:], len(samples) / rate


def make_handler(server):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def reply(self, code, obj):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            self.reply(200, {"status": "ok", "engine": "sherpa-onnx"})

        def do_POST(self):
            if self.path.split("?")[0] != "/inference":
                return self.reply(404, {"error": "use POST /inference"})
            try:
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length)
                head = f"Content-Type: {self.headers.get('Content-Type', '')}\r\n\r\n".encode()
                msg = BytesParser(policy=HTTP).parsebytes(head + body)
                wav = None
                for part in msg.iter_parts():
                    if part.get_param("name", header="content-disposition") == "file":
                        wav = part.get_payload(decode=True)
                if wav is None:
                    return self.reply(400, {"error": "no 'file' field in the form"})
                start = time.time()
                text, seconds = server.transcribe(wav)
                took = time.time() - start
                print(f"[{seconds:4.1f} s audio -> {took:4.2f} s] {text}", flush=True)
                self.reply(200, {"text": text})
            except Exception as e:  # keep serving after a bad clip
                print("error:", e, flush=True)
                self.reply(500, {"error": str(e)})

    return Handler


def main():
    p = argparse.ArgumentParser(description="sherpa-onnx speech-to-text server (Whisper-compatible API)")
    p.add_argument("--model", default=DEFAULT_MODEL, help="model folder (default: %(default)s)")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8090)
    p.add_argument("--threads", type=int, default=os.cpu_count() or 4)
    p.add_argument("--decoding", default="modified_beam_search",
                   choices=["modified_beam_search", "greedy_search"],
                   help="greedy_search is a bit faster, modified_beam_search a bit more accurate")
    args = p.parse_args()

    recognizer = load_recognizer(args.model, args.threads, args.decoding)
    server = Server(recognizer)
    httpd = HTTPServer((args.host, args.port), make_handler(server))
    print(f"sherpa-onnx server listening at http://{args.host}:{args.port} "
          f"({os.path.basename(args.model)}, {args.threads} threads, {args.decoding})", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
