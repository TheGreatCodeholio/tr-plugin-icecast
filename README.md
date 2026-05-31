<img src="https://raw.githubusercontent.com/TrunkRecorder/trunk-recorder/refs/heads/master/docs/media/trunk-recorder-header.png" width="75%" height="75%">

[![Discord](https://raw.githubusercontent.com/TrunkRecorder/trunk-recorder/refs/heads/master/docs/media/discord.jpg)](https://discord.gg/btJAhESnks) &nbsp;&nbsp;
# Trunk Recorder Icecast Plugin <!-- omit from toc -->

This is a plugin for Trunk Recorder that streams live calls onto an [Icecast](https://icecast.org) server in real time. Each stream is a standard MP3 mountpoint, so anyone can listen in a web browser, VLC, a scanner app, or feed it to [Broadcastify](https://www.broadcastify.com) — no special software required.

Several talkgroups can be merged onto a single stream. Calls play back-to-back in the order they arrive, with silence filling the gaps between them — just like listening to a scanner.

Stream metadata (ICY `StreamTitle`) updates automatically when each call starts, showing the talkgroup, talkgroup ID, talker alias (if configured), and the local time. Players that support ICY metadata — Winamp, VLC, most browser players — display this in real time.

Requires Trunk Recorder 5.0 or later, and the LAME and libsamplerate audio libraries.

- [Install](#install)
- [Configure](#configure)
  - [Server connection](#server-connection)
  - [Streams](#streams)
  - [Talkgroup mapping](#talkgroup-mapping)
  - [Trunk Recorder option](#trunk-recorder-option)
  - [Plugin usage](#plugin-usage)
- [How Streaming Works](#how-streaming-works)
- [Stream Metadata](#stream-metadata)
- [Listening](#listening)
- [Icecast Server](#icecast-server)

## Install

1. **Clone Trunk Recorder** source following these [instructions](https://github.com/robotastic/trunk-recorder/blob/master/docs/Install/INSTALL-LINUX.md).

2. **Install the audio libraries** the plugin needs to encode MP3:

```bash
sudo apt install libmp3lame-dev libsamplerate0-dev libssl-dev pkg-config
```

&emsp; Boost and OpenSSL are already required by Trunk Recorder.

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

| Key              | Required | Default Value | Type   | Description                                                                                       |
| ---------------- | :------: | ------------- | ------ | ------------------------------------------------------------------------------------------------- |
| host             |    ✓     | localhost     | string | Hostname or IP address of your Icecast server.                                                    |
| port             |          | 8000          | int    | Port your Icecast server listens on.                                                              |
| source_user      |          | source        | string | Source-client username configured on the Icecast server.                                          |
| source_password  |    ✓     |               | string | Source-client password configured on the Icecast server. The plugin will not start without this. |

The config also needs a `mounts` array and a `systems` array, described below.

### Streams

Each entry in the `mounts` array defines one live stream. Listeners connect to these.

| Key              | Required | Default Value | Type       | Description                                                                                                  |
| ---------------- | :------: | ------------- | ---------- | ------------------------------------------------------------------------------------------------------------ |
| mount            |    ✓     |               | string     | The stream's path on the server. Must start with `/` (e.g. `/dispatch.mp3`). This becomes the listener URL. |
| name             |          | same as mount | string     | Friendly stream name shown in players and the Icecast directory.                                             |
| description      |          |               | string     | Longer description shown in the Icecast directory.                                                           |
| genre            |          | Public Safety | string     | Genre tag shown in the Icecast directory.                                                                    |
| public           |          | false         | true/false | List the stream in Icecast's public directory.                                                               |
| sample_rate      |          | 22050         | int        | Output sample rate in Hz. 22050 is plenty for voice.                                                         |
| bitrate          |          | 64            | int        | MP3 bitrate in kbps.                                                                                         |
| channels         |          | 1             | int        | `1` for mono (recommended for radio), `2` for stereo.                                                        |
| gain             |          | 1.0           | float      | Linear amplitude multiplier applied before MP3 encoding. `1.0` = unity. `2.0` ≈ +6 dB. `0.5` ≈ -6 dB. Samples are clamped to prevent clipping. |
| admin_user       |          | admin         | string     | Icecast admin username used to push stream metadata updates. Must match `<admin-user>` in `icecast.xml`.     |
| admin_password   |          | source_password | string   | Icecast admin password used to push stream metadata updates. Defaults to `source_password` if not set. Must match `<admin-password>` in `icecast.xml`. |

### Talkgroup mapping

Each entry in the `systems` array decides which talkgroups go to which stream.

| Key        | Required | Default Value | Type   | Description                                                                                  |
| ---------- | :------: | ------------- | ------ | -------------------------------------------------------------------------------------------- |
| shortName  |    ✓     |               | string | Must match the `shortName` of a system in Trunk Recorder's main config.                      |
| talkgroups |    ✓     |               | object | Map of talkgroup ID to a stream's `mount`. Several talkgroups may point at the same stream.  |

Talkgroups not listed here are simply ignored — they are recorded by Trunk Recorder as usual but not streamed. For a conventional system, the talkgroup IDs are the channel numbers from the first column of its channel CSV.

### Trunk Recorder option

| Key            | Required | Default Value | Type       | Description                                                                                                       |
| -------------- | :------: | ------------- | ---------- | ----------------------------------------------------------------------------------------------------------------- |
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
          "channels": 1,
          "gain": 1.0,
          "admin_user": "admin",
          "admin_password": "REPLACE_ME"
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

## Stream Metadata

The plugin pushes ICY `StreamTitle` updates to Icecast via its admin metadata endpoint whenever a new call becomes active or the transmitting unit changes mid-call. Players that support ICY metadata (Winamp, VLC, most browser-based players) will display this in real time.

The metadata format is:

```
TG: <talkgroup_display> (<talkgroup_id>) <talker_alias> <HH:MM:SS>
```

For example:
```
TG: FIRE DISP (21101) E4-Smith 14:32:07
TG: FIRE DISP (21101) 14:32:07
```

The talker alias is included when Trunk Recorder has a unit tags file configured for the system and the transmitting unit's ID is found in it. If no alias is found the field is omitted. The timestamp is local wall-clock time on the machine running Trunk Recorder. When no call is active the stream title is set to `Standby`.

Metadata updates require the Icecast admin endpoint to be reachable using the `admin_user` and `admin_password` configured on each mount. A metadata failure never affects the audio stream itself.

### Talker alias requirements

- Set `unitTagsFile` in the relevant system block of Trunk Recorder's `config.json` pointing at a CSV of unit IDs and aliases.
- Or use over-the-air (OTA) aliases if your P25 system broadcasts them — set `unitTagsMode` to `"ota"` or `"user_first"`.
- If neither is configured the talker alias field will always be empty, but the rest of the metadata (talkgroup, ID, time) will still update correctly.

## Listening

Listeners connect to `http://<host>:<port><mount>`. With the example above:

```
http://icecast.example.com:8000/dispatch.mp3
http://icecast.example.com:8000/ops.mp3
```

Paste a URL into a web browser, VLC, Winamp, or a scanner app to listen. To send a stream to Broadcastify or a similar service, point the feed at the same URL.

## Icecast Server

You need an [Icecast](https://icecast.org) server for the plugin to stream to. Icecast is free and available in most package managers:

```bash
sudo apt install icecast2
```

A few things must line up between this plugin's config and the Icecast server's `icecast.xml`:

- The plugin's `source_user` and `source_password` must match a source account on the server (the `<source-password>` in `icecast.xml`, or a per-mount account).
- The `admin_user` and `admin_password` on each mount must match the `<admin-user>` and `<admin-password>` in `icecast.xml`. These are used to push stream title updates and default to `admin` / `source_password` if not set.
- The plugin's `port` must match the server's `<listen-socket>` port.
- Make sure `<limits>` allows enough `<sources>` for the number of mounts you configure.

No special tuning is required — Icecast accepts MP3 source clients out of the box. Mountpoints are created automatically when the plugin connects, so they do not need to be pre-declared in `icecast.xml`.
