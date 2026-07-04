# REAPER C++ MCP Extension

This project builds a REAPER extension that starts a small local MCP-style JSON-RPC server. It lets external tools call selected REAPER actions over HTTP while the extension marshals REAPER API work back onto REAPER's main thread.

The default server address is:

```sh
http://127.0.0.1:8766
```

You can override the port with `REAPER_MCP_PORT` and the diagnostic log path with `REAPER_MCP_LOG`. If no log path is set, diagnostics are written to `/tmp/reaper-mcp.log` as well as the REAPER console.

## Get the supporting repos

```
cd lib
git clone git@github.com:justinfrankel/reaper-sdk.git
git clone https://github.com/justinfrankel/WDL.git
```

## Generate the header:

In Reaper, generate 'reaper_plugin_functions.h' via Action “[developer] Write C++ API functions header"

Save it to `lib/reaper-sdk/sdk/reaper_plugin_functions.h`.


## Do the build

```
cmake -B build .
cmake --build build --config Release # this will copy the dylib to the right place
```

## MCP tools

The server currently exposes these tools through `tools/list` and `tools/call`.

| Tool | Args | What it does |
| --- | --- | --- |
| `reaper.create_track` | none | Creates a new track named `MCP Track` and returns its zero-based `track_index`. |
| `reaper.list_plugins` | optional `limit`, optional `query` | Lists installed plugins from `EnumInstalledFX`, including compact `name`, `ident`, inferred `format`, `role`, and `category`. Use `query` to filter plugin names/idents. |
| `reaper.list_tracks` | none | Lists current tracks with names, inferred `kind`, media counts, FX count, instrument FX index, and FX names. |
| `reaper.insert_plugin` | `track_index`, `plugin`, optional `plugin_type`, optional `always_create` | Inserts a plugin on a track using `TrackFX_AddByName`. If `plugin_type` is provided, the search string is built as `plugin_type:plugin`. |
| `reaper.insert_midi_file` | `track_index`, `file_path`, optional `position_seconds` | Inserts a MIDI file onto a track and repositions inserted items to the requested time. REAPER may embed imported MIDI, so later media queries can report it as embedded without an original file path. |
| `reaper.get_track_media_files` | `track_index`, optional `media_type` | Lists media sources on a track. `media_type` can be `all`, `midi`, or `audio`. |
| `reaper.undo` | optional `prefer_native` | Undoes the most recent MCP action. It prefers REAPER native undo for MCP undo blocks and falls back to an internal direct rollback stack for created tracks, inserted plugins, and inserted media items. |

All `track_index` values are zero-based.

## Curl tests

With REAPER running and the extension loaded:

```sh
./tests/curl_commands.sh
```

The script always prints initialization, `tools/list`, plugin listing, track listing, and media-file listing. Mutating calls are opt-in:

```sh
RUN_CREATE_TRACK=1 ./tests/curl_commands.sh
PLUGIN_QUERY=reasynth ./tests/curl_commands.sh
PLUGIN_NAME="ReaSynth (Cockos)" TRACK_INDEX=0 ./tests/curl_commands.sh
MIDI_FILE=/path/to/file.mid TRACK_INDEX=0 POSITION_SECONDS=0 ./tests/curl_commands.sh
RUN_UNDO=1 PREFER_NATIVE_UNDO=false ./tests/curl_commands.sh
```

Useful environment variables:

| Variable | Default | Purpose |
| --- | --- | --- |
| `MCP_URL` | `http://127.0.0.1:8766` | Server URL for curl tests. |
| `TRACK_INDEX` | `0` | Target track for track-specific tools. |
| `PLUGIN_LIST_LIMIT` | `50` | Number of installed plugins to request. |
| `PLUGIN_QUERY` | empty | Optional plugin list filter. |
| `PLUGIN_NAME` | empty | Enables the plugin insertion test when set. |
| `PLUGIN_TYPE` | empty | Optional plugin type prefix for insertion, such as `VSTi`. |
| `MIDI_FILE` | empty | Enables the MIDI insertion test when set. |
| `POSITION_SECONDS` | `0` | Position for MIDI insertion. |
| `RUN_CREATE_TRACK` | empty | Set to `1` to create a track. |
| `RUN_UNDO` | empty | Set to `1` to test undo. |
| `PREFER_NATIVE_UNDO` | `true` | Set to `false` to force direct rollback where possible. |

Diagnostic lines are prefixed with `[reaper-mcp:diag]`.
