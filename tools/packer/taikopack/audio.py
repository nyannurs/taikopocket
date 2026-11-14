"""Audio transcoding through ffmpeg, with a cache so unchanged songs are not re-encoded.

Formats (always 48 kHz):
  pcm         s16le stereo, 192 kB/s, bit exact
  adpcm       IMA ADPCM stereo in WAV block layout, 48 kB/s (4:1)
  adpcm-mono  IMA ADPCM mono, 24 kB/s (8:1 against stereo PCM)
  none        no audio (charts only; for tests)

The decoder is pluggable (DECODERS) so a browser build of the packer can swap
ffmpeg for something else.
"""

from __future__ import annotations

import hashlib
import json
import os
import struct
import subprocess
from dataclasses import dataclass

RATE = 48000
CODEC_PCM16, CODEC_IMA_ADPCM = 0, 1
CACHE_VERSION = 1

FORMATS = {
    # name: (codec id, channels, block bytes, samples per block)
    "pcm": (CODEC_PCM16, 2, 4, 1),
    "adpcm": (CODEC_IMA_ADPCM, 2, 2048, 2041),
    "adpcm-mono": (CODEC_IMA_ADPCM, 1, 1024, 2041),
}


@dataclass
class Encoded:
    path: str  # file holding the blob
    codec: int
    channels: int
    block_bytes: int
    block_samples: int
    samples: int
    size: int


def _ffmpeg(args: list[str]) -> None:
    proc = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", *args],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.decode("utf-8", errors="replace").strip() or "ffmpeg failed")


def _strip_wav(wav_path: str, out_path: str) -> int:
    """Copies the 'data' chunk of a WAV to out_path; returns the sample count ('fact' chunk)."""
    with open(wav_path, "rb") as f:
        head = f.read(12)
        if head[:4] != b"RIFF" or head[8:] != b"WAVE":
            raise RuntimeError("not a WAV file")
        samples = 0
        while True:
            chunk = f.read(8)
            if len(chunk) < 8:
                raise RuntimeError("WAV without data chunk")
            cid, size = chunk[:4], struct.unpack("<I", chunk[4:])[0]
            if cid == b"fact":
                samples = struct.unpack("<I", f.read(4))[0]
                f.seek(size - 4 + (size & 1), 1)
            elif cid == b"data":
                with open(out_path, "wb") as out:
                    while size > 0:
                        buf = f.read(min(size, 1 << 20))
                        if not buf:
                            break
                        out.write(buf)
                        size -= len(buf)
                return samples
            else:
                f.seek(size + (size & 1), 1)


def encode_ffmpeg(src: str, fmt: str, out_path: str) -> int:
    """Encodes src into out_path; returns the number of samples per channel."""
    codec, channels, block_bytes, _ = FORMATS[fmt]
    common = ["-i", src, "-vn", "-ac", str(channels), "-ar", str(RATE), "-bitexact", "-flags:a", "+bitexact"]
    if codec == CODEC_PCM16:
        _ffmpeg(common + ["-f", "s16le", out_path])
        return os.path.getsize(out_path) // (2 * channels)
    tmp = out_path + ".wav"
    try:
        _ffmpeg(common + ["-c:a", "adpcm_ima_wav", "-block_size", str(block_bytes), "-f", "wav", tmp])
        return _strip_wav(tmp, out_path)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


DECODERS = {"ffmpeg": encode_ffmpeg}


def encode(src: str, fmt: str, cache_dir: str, decoder: str = "ffmpeg") -> Encoded:
    """Returns the encoded blob for src, from the cache when the source is unchanged."""
    codec, channels, block_bytes, block_samples = FORMATS[fmt]
    st = os.stat(src)
    key = hashlib.sha1(json.dumps([CACHE_VERSION, os.path.abspath(src), st.st_size, st.st_mtime_ns, fmt])
                       .encode()).hexdigest()
    blob = os.path.join(cache_dir, key[:2], key + ".bin")
    meta = blob + ".json"
    if os.path.exists(blob) and os.path.exists(meta):
        with open(meta, encoding="utf-8") as f:
            info = json.load(f)
        if info.get("size") == os.path.getsize(blob):
            return Encoded(blob, codec, channels, block_bytes, block_samples, info["samples"], info["size"])
    os.makedirs(os.path.dirname(blob), exist_ok=True)
    tmp = blob + ".part"
    samples = DECODERS[decoder](src, fmt, tmp)
    os.replace(tmp, blob)
    size = os.path.getsize(blob)
    if size == 0 or samples == 0:
        os.remove(blob)
        raise RuntimeError("no audio decoded")
    with open(meta, "w", encoding="utf-8") as f:
        json.dump({"samples": samples, "size": size, "source": os.path.basename(src)}, f)
    return Encoded(blob, codec, channels, block_bytes, block_samples, samples, size)
