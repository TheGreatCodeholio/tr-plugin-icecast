<img src="https://raw.githubusercontent.com/TrunkRecorder/trunk-recorder/refs/heads/master/docs/media/trunk-recorder-header.png" width="75%" height="75%">

[![Discord](https://raw.githubusercontent.com/TrunkRecorder/trunk-recorder/refs/heads/master/docs/media/discord.jpg)](https://discord.gg/btJAhESnks) &nbsp;&nbsp;
# Trunk Recorder Icecast Plugin <!-- omit from toc -->

This is a plugin for Trunk Recorder that streams live calls onto an [Icecast](https://icecast.org) server in real time. Each stream is a standard MP3 mountpoint, so anyone can listen in a web browser, VLC, a scanner app, or feed it to [Broadcastify](https://www.broadcastify.com) — no special software required.

Several talkgroups can be merged onto a single stream. Calls play back-to-back in the order they arrive, with silence filling the gaps between them — just like listening to a scanner.

Requires Trunk Recorder 5.0 or later, and the LAME and libsamplerate audio libraries.

- [Install](#install)
- [Configure](#configure)
  - [Server connection](#server-connection)
  - [Streams](#streams)
  - [Talkgroup mapping](#talkgroup-mapping)
  - [Trunk Recorder option](#trunk-recorder-option)
  - [Plugin usage](#plugin-usage)
- [How Streaming Works](#how-streaming-works)
- [Listening](#listening)
- [Icecast Server](#icecast-server)

## Install

1. **Clone Trunk Recorder** source following these [instructions](https://github.com/robotastic/trunk-recorder/blob/master/docs/Install/INSTALL-LINUX.md).

2. **Install the audio libraries** the plugin needs to encode MP3:

```bash
sudo apt install libmp3lame-dev libsamplerate0-dev libssl-dev pkg-config
```

&emsp; Boost and OpenSSL are already required by Trunk Recorder.
s
3. **Build and install the plugin:**

&emsp; This plugin source should be cloned into the `user_plugins` directory of the Trunk Recorder 5.0+ source tree. It will be built and installed along with Trunk Recorder.

```bash
cd [your trunk-recorder source directory]
cd user_plugins
git clone https://github.com/TheGreatCodeholio/tr-plugin-icecast
cd [your trunk-recorder build directory]
sudo make install
```

&emsp; **NOTE:** Plugins are automatically built and installed with Trunk Recorder. To update either Trunk Recorder or a plugin, simply `cd` into the appropriate git directory and `git pull`. Refer to the above instructions to `make install` any updates.

## Configure

All settings go in a plugin block under `"plugins"` in Trunk Recorder's main `config.json`. See the included [config.example.json](./config.example.json) for a complete example.

### Server connection

These top-level keys tell the plugin how to reach your Icecast server.

| Key             | Required | Default Value | Type   | Description                                                                                       |
| --------------- | :------: | ------------- | ------ | ------------------------------------------------------------------------------------------------- |
| host            |    ✓     | localhost     | string | Hostname or IP address of your Icecast server.                                                    |
| port            |          | 8000          | int    | Port your Icecast server listens on.                                                              |
| source_user     |          | source        | string | Source-client username configured on the Icecast server.                                         |
| source_password |    ✓     |               | string | Source-client password configured on the Icecast server. The plugin will not start without this. |

The config also needs a `mounts` array and a `systems` array, described below.

### Streams

Each entry in the `mounts` array defines one live stream. Listeners connect to these.

| Key         | Required | Default Value  | Type       | Description                                                                                                |
| ----------- | :------: | -------------- | ---------- | ---------------------------------------------------------------------------------------------------------- |
| mount       |    ✓     |                | string     | The stream's path on the server. Must start with `/` (e.g. `/dispatch.mp3`). This becomes the listener URL. |
| name        |          | same as mount  | string     | Friendly stream name shown in players and the Icecast directory.                                           |
| description |          |                | string     | Longer description shown in the Icecast directory.                                                         |
| genre       |          | Public Safety  | string     | Genre tag shown in the Icecast directory.                                                                  |
| public      |          | false          | true/false | List the stream in Icecast's public directory.                                                             |
| sample_rate |          | 22050          | int        | Output sample rate in Hz. 22050 is plenty for voice.                                                       |
| bitrate     |          | 64             | int        | MP3 bitrate in kbps.                                                                                       |
| channels    |          | 1              | int        | `1` for mono (recommended for radio), `2` for stereo.                                                      |

### Talkgroup mapping

Each entry in the `systems` array decides which talkgroups go to which stream.

| Key        | Required | Default Value | Type   | Description                                                                                       |
| ---------- | :------: | ------------- | ------ | ------------------------------------------------------------------------------------------------- |
| shortName  |    ✓     |               | string | Must match the `shortName` of a system in Trunk Recorder's main config.                           |
| talkgroups |    ✓     |               | object | Map of talkgroup ID to a stream's `mount`. Several talkgroups may point at the same stream.       |

Talkgroups not listed here are simply ignored — they are recorded by Trunk Recorder as usual but not streamed. For a conventional system, the talkgroup IDs are the channel numbers from the first column of its channel CSV.

### Trunk Recorder option

| Key            | Required | Default Value | Type       | Description                                                                                                          |
| -------------- | :------: | ------------- | ---------- | -------------------------------------------------------------------------------------------------------------------- |
| audioStreaming |    ✓     | false         | true/false | Must be set to `true` at the **top level** of Trunk Recorder's main config. The plugin will not start without it. |

### Plugin usage

```json
{
  "audioStreaming": true,

  "plugins": [
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
          "description": "Combined fire/EMS dispatch",
          "genre": "Public Safety",
          "public": false,
          "sample_rate": 22050,
          "bitrate": 64,
          "channels": 1
        },
        {
          "mount": "/ops.mp3",
          "name": "Operations",
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
            "30002": "/dispatch.mp3",
            "30010": "/ops.mp3"
          }
        }
      ]
    }
  ]
}
```

If the plugin cannot be found, or it is being run from a different location, it may be necessary to supply the full path:

```json
        "library": "/usr/local/lib/trunk-recorder/libtr_plugin_icecast.so",
```

## How Streaming Works

- Each entry in `mounts` is one continuous live stream. As soon as Trunk Recorder starts, every stream goes live — listeners can connect right away and will hear silence until a call comes in.
- When a call is recorded on a mapped talkgroup, its audio is streamed onto the matching mount as the call happens.
- When several talkgroups share one stream and calls overlap, the calls are queued and played one after another in arrival order — first in, first out. Nothing is dropped or mixed; a later caller simply waits its turn.
- Between calls, the stream sends silence so listeners' players stay connected and don't time out.
- Streams on different mounts run independently and at the same time.
- If the connection to Icecast drops, the plugin reconnects on its own, retrying with an increasing delay (up to 30 seconds). Audio missed during an outage is not replayed — these are live streams.

## Listening

Listeners connect to `http://<host>:<port><mount>`. With the example above:

```
http://icecast.example.com:8000/dispatch.mp3
http://icecast.example.com:8000/ops.mp3
```

Paste a URL into a web browser, VLC, or a scanner app to listen. To send a stream to Broadcastify or a similar service, point the feed at the same URL.

## Icecast Server

You need an [Icecast](https://icecast.org) server for the plugin to stream to. Icecast is free and available in most package managers:

```bash
sudo apt install icecast2
```

A few things must line up between this plugin's config and the Icecast server's `icecast.xml`:

- The plugin's `source_user` and `source_password` must match a source account on the server (the `<source-password>` in `icecast.xml`, or a per-mount account).
- The plugin's `port` must match the server's `<listen-socket>` port.
- Make sure `<limits>` allows enough `<sources>` for the number of mounts you configure.

No special tuning is required — Icecast accepts MP3 source clients out of the box. Mountpoints are created automatically when the plugin connects, so they do not need to be pre-declared in `icecast.xml`.