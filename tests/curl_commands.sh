#!/usr/bin/env bash
set -euo pipefail

MCP_URL="${MCP_URL:-http://127.0.0.1:8766}"
TRACK_INDEX="${TRACK_INDEX:-0}"
PLUGIN_LIST_LIMIT="${PLUGIN_LIST_LIMIT:-50}"

post_rpc() {
  local label="$1"
  local payload="$2"

  printf '\n== %s ==\n' "$label"
  curl --silent --show-error \
    --header 'Content-Type: application/json' \
    --data "$payload" \
    "$MCP_URL"
  printf '\n'
}

json_escape() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  value="${value//$'\n'/\\n}"
  printf '%s' "$value"
}

post_rpc "initialize" '{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": {
    "protocolVersion": "2024-11-05",
    "capabilities": {},
    "clientInfo": {
      "name": "curl",
      "version": "0.1.0"
    }
  }
}'

post_rpc "initialized notification" '{
  "jsonrpc": "2.0",
  "method": "notifications/initialized",
  "params": {}
}'

post_rpc "tools/list" '{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "tools/list",
  "params": {}
}'

if [[ "${RUN_CREATE_TRACK:-0}" == "1" ]]; then
  post_rpc "tools/call reaper.create_track" '{
    "jsonrpc": "2.0",
    "id": 3,
    "method": "tools/call",
    "params": {
      "name": "reaper.create_track",
      "arguments": {}
    }
  }'
else
  printf '\n== skipping reaper.create_track; set RUN_CREATE_TRACK=1 to run ==\n'
fi

PLUGIN_QUERY_JSON="$(json_escape "${PLUGIN_QUERY:-}")"
post_rpc "tools/call reaper.list_plugins" "{
  \"jsonrpc\": \"2.0\",
  \"id\": 4,
  \"method\": \"tools/call\",
  \"params\": {
    \"name\": \"reaper.list_plugins\",
    \"arguments\": {
      \"limit\": ${PLUGIN_LIST_LIMIT},
      \"query\": \"${PLUGIN_QUERY_JSON}\"
    }
  }
}"

post_rpc "tools/call reaper.list_tracks" '{
  "jsonrpc": "2.0",
  "id": 5,
  "method": "tools/call",
  "params": {
    "name": "reaper.list_tracks",
    "arguments": {}
  }
}'

post_rpc "tools/call reaper.get_track_media_files" "{
  \"jsonrpc\": \"2.0\",
  \"id\": 6,
  \"method\": \"tools/call\",
  \"params\": {
    \"name\": \"reaper.get_track_media_files\",
    \"arguments\": {
      \"track_index\": ${TRACK_INDEX},
      \"media_type\": \"all\"
    }
  }
}"

if [[ -n "${PLUGIN_NAME:-}" ]]; then
  PLUGIN_NAME_JSON="$(json_escape "$PLUGIN_NAME")"
  PLUGIN_TYPE_JSON="$(json_escape "${PLUGIN_TYPE:-}")"
  post_rpc "tools/call reaper.insert_plugin" "{
    \"jsonrpc\": \"2.0\",
    \"id\": 7,
    \"method\": \"tools/call\",
    \"params\": {
      \"name\": \"reaper.insert_plugin\",
      \"arguments\": {
        \"track_index\": ${TRACK_INDEX},
        \"plugin\": \"${PLUGIN_NAME_JSON}\",
        \"plugin_type\": \"${PLUGIN_TYPE_JSON}\",
        \"always_create\": ${ALWAYS_CREATE_PLUGIN:-true}
      }
    }
  }"

  post_rpc "tools/call reaper.list_tracks after plugin insert" '{
    "jsonrpc": "2.0",
    "id": 10,
    "method": "tools/call",
    "params": {
      "name": "reaper.list_tracks",
      "arguments": {}
    }
  }'
else
  printf '\n== skipping reaper.insert_plugin; set PLUGIN_NAME to run ==\n'
fi

if [[ -n "${MIDI_FILE:-}" ]]; then
  MIDI_FILE_JSON="$(json_escape "$MIDI_FILE")"
  post_rpc "tools/call reaper.insert_midi_file" "{
    \"jsonrpc\": \"2.0\",
    \"id\": 8,
    \"method\": \"tools/call\",
    \"params\": {
      \"name\": \"reaper.insert_midi_file\",
      \"arguments\": {
        \"track_index\": ${TRACK_INDEX},
        \"file_path\": \"${MIDI_FILE_JSON}\",
        \"position_seconds\": ${POSITION_SECONDS:-0}
      }
    }
  }"
else
  printf '\n== skipping reaper.insert_midi_file; set MIDI_FILE to run ==\n'
fi

if [[ "${RUN_UNDO:-0}" == "1" ]]; then
  post_rpc "tools/call reaper.undo" "{
    \"jsonrpc\": \"2.0\",
    \"id\": 9,
    \"method\": \"tools/call\",
    \"params\": {
      \"name\": \"reaper.undo\",
      \"arguments\": {
        \"prefer_native\": ${PREFER_NATIVE_UNDO:-true}
      }
    }
  }"
else
  printf '\n== skipping reaper.undo; set RUN_UNDO=1 to run ==\n'
fi

# Useful one-liners:
#
# curl -sS -H 'Content-Type: application/json' \
#   -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"curl","version":"0.1.0"}}}' \
#   http://127.0.0.1:8766
#
# curl -sS -H 'Content-Type: application/json' \
#   -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' \
#   http://127.0.0.1:8766
#
# curl -sS -H 'Content-Type: application/json' \
#   -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"reaper.create_track","arguments":{}}}' \
#   http://127.0.0.1:8766
