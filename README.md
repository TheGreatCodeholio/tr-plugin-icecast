# tr-plugin-icecast

Trunk-recorder plugin that streams live trunked-radio calls onto icecast2 mountpoints in real time. Multiple talkgroups can be merged onto a single mountpoint, FIFO in arrival order — listeners hear calls played back-to-back as they happen, with silence filling the gaps.

Sister project to [tr-zello-bridge](../tr-zello-bridge); shares the same single-asio-thread design but with MP3/icecast on the wire instead of Opus/Zello.

## How it fits

Trunk-recorder calls each plugin's `audio_stream()` hook with `int16_t` PCM as the vocoder produces it. This plugin owns one Boost.Asio io_context on a single dedicated thread. That thread:

1. Drains a per-call ring buffer.
2. Resamples (libsamplerate) from the recorder's native rate (typically 8 kHz) to the mountpoint's output rate (default 22 050 Hz).
3. Encodes 1152-sample MP3 frames (libmp3lame).
4. Writes them onto a persistent HTTP `PUT` connection to icecast2.

A per-mountpoint pacer timer ticks once per MP3 frame interval (≈52 ms at 22 050 Hz). When a call is active, the pacer pulls one encoded frame from that call's drain function. When no call is active, it emits a pre-encoded silent frame so the bytestream — and listeners' players — never stall. On socket failure, the session reconnects with exponential backoff (1 s → 30 s cap); audio in flight during a disconnect is dropped (live source, no replay).

Concurrent calls landing on the **same mountpoint** queue FIFO; the next call promotes as soon as the active one drains. Concurrent calls on **different mountpoints** stream in parallel.

## Build

This plugin is intended to be dropped into trunk-recorder's `user_plugins/` directory:

```bash
ln -s /srv/dev_projects/personal/CLionProjects/tr-plugin-icecast \
      /srv/dev_projects/personal/CLionProjects/trunk-recorder/user_plugins/tr-plugin-icecast
cd /srv/dev_projects/personal/CLionProjects/trunk-recorder
cmake -S . -B build && cmake --build build -j
```

System dependencies (Ubuntu/Debian):

```
sudo apt install libmp3lame-dev libsamplerate0-dev libssl-dev pkg-config
```

Boost (≥1.71) and OpenSSL are already required by trunk-recorder.

## Configure

In trunk-recorder's main `config.json`:

1. Enable live audio streaming (off by default):

   ```json
   { "audioStreaming": true, ... }
   ```

2. Add a plugin block under `"plugins"` — see [`config.example.json`](config.example.json).

   ```json
   {
     "name": "Icecast Bridge",
     "library": "libtr_plugin_icecast.so",
     "host": "icecast.example.com",
     "port": 8000,
     "source_user": "source",
     "source_password": "REPLACE_ME",
     "mounts": [
       {
         "mount": "/dispatch.mp3",
         "name": "Dispatch",
         "sample_rate": 22050,
         "bitrate": 64,
         "channels": 1
       }
     ],
     "systems": [
       {
         "shortName": "your_system",
         "talkgroups": {
           "30001": "/dispatch.mp3",
           "30002": "/dispatch.mp3"
         }
       }
     ]
   }
   ```

   Mountpoint paths must start with `/`. Multiple talkgroups mapping to the same mountpoint is the **expected** case — that's what makes this plugin different from the Zello bridge.

## Source layout

```
src/
  codec/        Pure encoders, no I/O
    mp3_encoder.h/.cc      libmp3lame wrapper, 1152-sample MP3 frames
    resampler.h/.cc        libsamplerate wrapper, per-call SRC_STATE
  transport/    Boost.Asio HTTP-source transport
    icecast_session.h/.cc  Per-mount persistent PUT, silence keepalive,
                           reconnect-with-backoff
    icecast_pool.h/.cc     Map of <mount> -> session
  pipeline/     Per-call audio data flow
    ring_buffer.h          SPSC PCM buffer (mutex-guarded)
    call_state.h/.cc       CallState struct + thread-safe registry by call_num
  plugin/       Trunk-recorder Plugin_Api glue
    config.h/.cc           PluginConfig + parse_plugin_config (JSON validation)
    icecast_bridge_plugin.cc  Plugin_Api impl, owns io_context + worker thread,
                              BOOST_DLL_ALIAS at the bottom
```

## Status

**Functionally complete, not yet field-tested.** What's implemented:

- Project / CMake / config schema / plugin lifecycle (parse_config → init → start → call_start → audio_stream → call_end → stop).
- Per-mountpoint FIFO queueing of concurrent calls.
- Asio-based session: connect, HTTP `PUT` source handshake (with `100 Continue`
  handling), pacer, write loop, exponential-backoff reconnect.
- `Mp3FrameEncoder` (libmp3lame) and `Resampler` (libsamplerate) — full bodies.
- Silence keepalive: the session encodes a silent MP3 frame per tick when no
  call is active (no separate pre-encoded `silence_frame_`).
- Stale-call watchdog: if trunk-recorder skips `call_end`, the active call is
  finalized after `kStaleAfterMs` so it can't wedge the mountpoint queue.
- Ghost-call cleanup: a per-call timer armed in `call_start` reclaims the
  registry entry after `kGhostCleanupSeconds` when a call produces no audio
  and gets no `call_end` (squelch-flutter ghosts).

Known gaps / not yet done:

- Not run against a live icecast2 server yet — the wire handshake is unverified
  end to end.
- Optional: icecast `/admin/metadata` updates on each call promotion.