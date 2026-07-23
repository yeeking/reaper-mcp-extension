#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define REAPERAPI_IMPLEMENT
#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_CountTracks
#define REAPERAPI_WANT_DeleteTrack
#define REAPERAPI_WANT_DeleteTrackMediaItem
#define REAPERAPI_WANT_EnumInstalledFX
#define REAPERAPI_WANT_GetActiveTake
#define REAPERAPI_WANT_GetCursorPosition
#define REAPERAPI_WANT_GetMediaItemInfo_Value
#define REAPERAPI_WANT_GetMediaItemNumTakes
#define REAPERAPI_WANT_GetMediaItemTake
#define REAPERAPI_WANT_GetMediaItemTake_Source
#define REAPERAPI_WANT_GetMediaSourceFileName
#define REAPERAPI_WANT_GetMediaSourceType
#define REAPERAPI_WANT_GetSetMediaTrackInfo_String
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_GetTrackMediaItem
#define REAPERAPI_WANT_GetTrackName
#define REAPERAPI_WANT_GetTrackNumMediaItems
#define REAPERAPI_WANT_InsertMedia
#define REAPERAPI_WANT_InsertTrackAtIndex
#define REAPERAPI_WANT_plugin_register
#define REAPERAPI_WANT_PreventUIRefresh
#define REAPERAPI_WANT_SelectAllMediaItems
#define REAPERAPI_WANT_SetEditCurPos
#define REAPERAPI_WANT_SetMediaItemInfo_Value
#define REAPERAPI_WANT_SetOnlyTrackSelected
#define REAPERAPI_WANT_SetTrackSelected
#define REAPERAPI_WANT_ShowConsoleMsg
#define REAPERAPI_WANT_TrackFX_AddByName
#define REAPERAPI_WANT_TrackFX_Delete
#define REAPERAPI_WANT_TrackFX_GetCount
#define REAPERAPI_WANT_TrackFX_GetFXName
#define REAPERAPI_WANT_TrackFX_GetInstrument
#define REAPERAPI_WANT_Undo_BeginBlock
#define REAPERAPI_WANT_Undo_CanUndo2
#define REAPERAPI_WANT_Undo_DoUndo2
#define REAPERAPI_WANT_Undo_EndBlock
#define REAPERAPI_WANT_UpdateArrange
#include "reaper_plugin_functions.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#endif

namespace {

constexpr int kDefaultServerPort = 8766;
constexpr const char *kServerHost = "127.0.0.1";

std::atomic<bool> g_running{false};
std::thread g_serverThread;
socket_t g_listenSocket = -1;
int g_serverPort = kDefaultServerPort;

struct MainThreadJob {
  std::function<std::string()> run;
  std::string result;
  bool done = false;
  std::mutex mutex;
  std::condition_variable cv;
};

enum class DirectUndoKind {
  DeleteTrack,
  DeleteFx,
  DeleteMediaItems
};

struct DirectUndoAction {
  DirectUndoKind kind;
  std::string label;
  int trackIndex = -1;
  int fxIndex = -1;
  std::vector<int> mediaItemIndexes;
};

std::mutex g_queueMutex;
std::deque<std::shared_ptr<MainThreadJob>> g_jobs;
std::mutex g_directUndoMutex;
std::vector<DirectUndoAction> g_directUndoStack;
std::mutex g_logFileMutex;
std::string g_logFilePath;

std::string logFilePath() {
  if (!g_logFilePath.empty()) return g_logFilePath;
  const char *envPath = std::getenv("REAPER_MCP_LOG");
  g_logFilePath = envPath && *envPath ? envPath : "/tmp/reaper-mcp.log";
  return g_logFilePath;
}

void appendLogFile(const std::string &line) {
  std::lock_guard<std::mutex> lock(g_logFileMutex);
  std::ofstream out(logFilePath().c_str(), std::ios::app);
  if (out.good()) out << line << '\n';
}

void log(const std::string &message) {
  appendLogFile("[reaper-mcp] " + message);
  if (ShowConsoleMsg) {
    ShowConsoleMsg(("[reaper-mcp] " + message + "\n").c_str());
  }
}

void diag(const std::string &message) {
  appendLogFile("[reaper-mcp:diag] " + message);
  if (ShowConsoleMsg) {
    ShowConsoleMsg(("[reaper-mcp:diag] " + message + "\n").c_str());
  }
}

std::string lastSocketError() {
#if defined(_WIN32)
  return "WSA error " + std::to_string(WSAGetLastError());
#else
  return std::strerror(errno);
#endif
}

int configuredServerPort() {
  const char *value = std::getenv("REAPER_MCP_PORT");
  if (!value || !*value) return kDefaultServerPort;

  char *end = nullptr;
  const long port = std::strtol(value, &end, 10);
  if (!end || *end != '\0' || port <= 0 || port > 65535) {
    log("ignoring invalid REAPER_MCP_PORT=" + std::string(value));
    return kDefaultServerPort;
  }
  return static_cast<int>(port);
}

std::string jsonEscape(const std::string &input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (char ch : input) {
    switch (ch) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (static_cast<unsigned char>(ch) < 0x20) {
        char buf[7];
        std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
        out += buf;
      } else {
        out += ch;
      }
    }
  }
  return out;
}

std::string makeJsonResult(const std::string &id, const std::string &resultJson) {
  return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + resultJson + "}";
}

std::string makeJsonError(const std::string &id, int code, const std::string &message) {
  return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":" +
         std::to_string(code) + ",\"message\":\"" + jsonEscape(message) + "\"}}";
}

std::string findJsonString(const std::string &json, const std::string &key) {
  const std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return {};
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return {};
  pos = json.find('"', pos + 1);
  if (pos == std::string::npos) return {};

  std::string value;
  bool escaped = false;
  for (size_t i = pos + 1; i < json.size(); ++i) {
    const char ch = json[i];
    if (escaped) {
      value += ch;
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else if (ch == '"') {
      return value;
    } else {
      value += ch;
    }
  }
  return {};
}

std::string findJsonObject(const std::string &json, const std::string &key) {
  const std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return {};
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return {};
  pos = json.find('{', pos + 1);
  if (pos == std::string::npos) return {};

  int depth = 0;
  bool inString = false;
  bool escaped = false;
  for (size_t i = pos; i < json.size(); ++i) {
    const char ch = json[i];
    if (inString) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') inString = false;
      continue;
    }
    if (ch == '"') inString = true;
    else if (ch == '{') ++depth;
    else if (ch == '}' && --depth == 0) return json.substr(pos, i - pos + 1);
  }
  return {};
}

bool findJsonInt(const std::string &json, const std::string &key, int *out) {
  const std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return false;
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  ++pos;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
  char *end = nullptr;
  const long value = std::strtol(json.c_str() + pos, &end, 10);
  if (end == json.c_str() + pos) return false;
  *out = static_cast<int>(value);
  return true;
}

bool findJsonDouble(const std::string &json, const std::string &key, double *out) {
  const std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return false;
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  ++pos;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
  char *end = nullptr;
  const double value = std::strtod(json.c_str() + pos, &end);
  if (end == json.c_str() + pos) return false;
  *out = value;
  return true;
}

bool findJsonBool(const std::string &json, const std::string &key, bool *out) {
  const std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return false;
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  ++pos;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
  if (json.compare(pos, 4, "true") == 0) {
    *out = true;
    return true;
  }
  if (json.compare(pos, 5, "false") == 0) {
    *out = false;
    return true;
  }
  return false;
}

std::string lowerAscii(std::string value) {
  for (char &ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return value;
}

bool startsWith(const std::string &value, const std::string &prefix) {
  return value.compare(0, prefix.size(), prefix) == 0;
}

bool containsAny(const std::string &haystack, const std::initializer_list<const char *> needles) {
  for (const char *needle : needles) {
    if (haystack.find(needle) != std::string::npos) return true;
  }
  return false;
}

std::string inferPluginFormat(const std::string &name, const std::string &ident) {
  const std::string probe = name + " " + ident;
  const size_t colon = probe.find(':');
  if (colon != std::string::npos && colon < 8) return probe.substr(0, colon);
  if (startsWith(probe, "VST3i")) return "VST3i";
  if (startsWith(probe, "VSTi")) return "VSTi";
  return "unknown";
}

std::string inferPluginRole(const std::string &name, const std::string &ident) {
  const std::string probe = lowerAscii(name + " " + ident);
  if (startsWith(probe, "vsti:") || startsWith(probe, "vst3i:") ||
      startsWith(probe, "aui:") || containsAny(probe, {"synth", "sampler", "instrument"})) {
    return "instrument";
  }
  if (startsWith(probe, "vst:") || startsWith(probe, "vst3:") || startsWith(probe, "au:") ||
      startsWith(probe, "js:") || startsWith(probe, "clap:") || startsWith(probe, "dx:")) {
    return "effect";
  }
  return "unknown";
}

std::string inferPluginCategory(const std::string &name, const std::string &ident) {
  const std::string probe = lowerAscii(name + " " + ident);
  if (containsAny(probe, {"synth", "instrument"})) return "synth";
  if (containsAny(probe, {"sampler", "sample", "reasamplomatic"})) return "sampler";
  if (containsAny(probe, {"compress", "limiter", "gate", "expander", "dynamics"})) return "dynamics";
  if (containsAny(probe, {"eq", "equaliz", "filter"})) return "eq";
  if (containsAny(probe, {"reverb", "room", "plate"})) return "reverb";
  if (containsAny(probe, {"delay", "echo"})) return "delay";
  if (containsAny(probe, {"surround", "spatial", "pan", "ambisonic"})) return "spatial";
  if (containsAny(probe, {"meter", "analy", "scope", "utility"})) return "utility";
  return "unknown";
}

std::string nullableJsonString(const std::string &value) {
  return value.empty() ? "null" : "\"" + jsonEscape(value) + "\"";
}

MediaTrack *trackFromIndex(int trackIndex) {
  if (trackIndex < 0 || trackIndex >= CountTracks(nullptr)) return nullptr;
  return GetTrack(nullptr, trackIndex);
}

void pushDirectUndo(const DirectUndoAction &action) {
  std::lock_guard<std::mutex> lock(g_directUndoMutex);
  g_directUndoStack.push_back(action);
  diag("direct_undo push label=\"" + action.label + "\" stack_size=" +
       std::to_string(g_directUndoStack.size()));
}

bool popDirectUndo(DirectUndoAction *action) {
  std::lock_guard<std::mutex> lock(g_directUndoMutex);
  if (g_directUndoStack.empty()) return false;
  *action = g_directUndoStack.back();
  g_directUndoStack.pop_back();
  diag("direct_undo pop label=\"" + action->label + "\" stack_size=" +
       std::to_string(g_directUndoStack.size()));
  return true;
}

void discardDirectUndoIfLabelMatches(const std::string &label) {
  std::lock_guard<std::mutex> lock(g_directUndoMutex);
  if (!g_directUndoStack.empty() && g_directUndoStack.back().label == label) {
    diag("direct_undo discard_after_native label=\"" + label + "\"");
    g_directUndoStack.pop_back();
  }
}

std::string runDirectUndoAction(const DirectUndoAction &action) {
  diag("direct_undo run label=\"" + action.label + "\"");
  Undo_BeginBlock();
  PreventUIRefresh(1);

  bool ok = false;
  if (action.kind == DirectUndoKind::DeleteTrack) {
    MediaTrack *track = trackFromIndex(action.trackIndex);
    if (track) {
      DeleteTrack(track);
      ok = true;
    }
  } else if (action.kind == DirectUndoKind::DeleteFx) {
    MediaTrack *track = trackFromIndex(action.trackIndex);
    if (track && action.fxIndex >= 0 && action.fxIndex < TrackFX_GetCount(track)) {
      ok = TrackFX_Delete(track, action.fxIndex);
    }
  } else if (action.kind == DirectUndoKind::DeleteMediaItems) {
    MediaTrack *track = trackFromIndex(action.trackIndex);
    if (track) {
      ok = true;
      for (auto it = action.mediaItemIndexes.rbegin(); it != action.mediaItemIndexes.rend(); ++it) {
        MediaItem *item = GetTrackMediaItem(track, *it);
        const bool deleted = item && DeleteTrackMediaItem(track, item);
        diag("direct_undo delete_media_item track=" + std::to_string(action.trackIndex) +
             " item=" + std::to_string(*it) + " deleted=" + std::to_string(deleted));
        ok = ok && deleted;
      }
    }
  }

  PreventUIRefresh(-1);
  UpdateArrange();
  Undo_EndBlock(("MCP direct undo: " + action.label).c_str(), -1);

  return "{\"ok\":" + std::string(ok ? "true" : "false") +
         ",\"direct\":true,\"undone\":\"" + jsonEscape(action.label) + "\"}";
}

std::string findJsonId(const std::string &json) {
  size_t pos = json.find("\"id\"");
  if (pos == std::string::npos) return "null";
  pos = json.find(':', pos + 4);
  if (pos == std::string::npos) return "null";
  ++pos;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
  if (pos >= json.size()) return "null";
  if (json[pos] == '"') {
    const std::string id = findJsonString(json, "id");
    return "\"" + jsonEscape(id) + "\"";
  }
  size_t end = pos;
  while (end < json.size() && std::string(",}\r\n\t ").find(json[end]) == std::string::npos) ++end;
  return json.substr(pos, end - pos);
}

std::string runOnMainThread(const std::function<std::string()> &fn) {
  auto job = std::make_shared<MainThreadJob>();
  job->run = fn;
  {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_jobs.push_back(job);
  }

  std::unique_lock<std::mutex> lock(job->mutex);
  job->cv.wait(lock, [&] { return job->done || !g_running.load(); });
  if (!job->done) return "{\"ok\":false,\"error\":\"server is shutting down\"}";
  return job->result;
}

void processMainThreadJobs() {
  std::deque<std::shared_ptr<MainThreadJob>> jobs;
  {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    jobs.swap(g_jobs);
  }

  for (const auto &job : jobs) {
    std::string result;
    try {
      result = job->run ? job->run() : "{\"ok\":false,\"error\":\"empty job\"}";
    } catch (const std::exception &e) {
      result = std::string("{\"ok\":false,\"error\":\"") + jsonEscape(e.what()) + "\"}";
    } catch (...) {
      result = "{\"ok\":false,\"error\":\"unknown exception\"}";
    }

    {
      std::lock_guard<std::mutex> lock(job->mutex);
      job->result = result;
      job->done = true;
    }
    job->cv.notify_one();
  }
}

std::string mcp_createTrack() {
  Undo_BeginBlock();
  PreventUIRefresh(1);

  const int index = CountTracks(nullptr);
  InsertTrackAtIndex(index, true);
  MediaTrack *track = GetTrack(nullptr, index);
  if (track) {
    char name[] = "MCP Track";
    GetSetMediaTrackInfo_String(track, "P_NAME", name, true);
  }

  PreventUIRefresh(-1);
  UpdateArrange();
  Undo_EndBlock("MCP: create track", -1);

  if (!track) return "{\"ok\":false,\"error\":\"InsertTrackAtIndex did not return a track\"}";
  DirectUndoAction undo;
  undo.kind = DirectUndoKind::DeleteTrack;
  undo.label = "MCP: create track";
  undo.trackIndex = index;
  pushDirectUndo(undo);
  return "{\"ok\":true,\"track_index\":" + std::to_string(index) + ",\"name\":\"MCP Track\"}";
}

std::string mcp_listPlugins(const std::string &args) {
  int limit = 100;
  findJsonInt(args, "limit", &limit);
  if (limit < 1) limit = 1;
  if (limit > 1000) limit = 1000;
  const std::string query = lowerAscii(findJsonString(args, "query"));

  diag("tool=reaper.list_plugins limit=" + std::to_string(limit) + " query=\"" + query + "\"");
  std::ostringstream json;
  json << "{\"ok\":true,\"plugins\":[";
  int emitted = 0;
  int scanned = 0;
  for (int i = 0; emitted < limit; ++i) {
    const char *name = nullptr;
    const char *ident = nullptr;
    if (!EnumInstalledFX(i, &name, &ident)) break;
    ++scanned;
    const std::string fxName = name ? name : "";
    const std::string fxIdent = ident ? ident : "";
    if (!query.empty() && lowerAscii(fxName + " " + fxIdent).find(query) == std::string::npos) continue;

    const std::string format = inferPluginFormat(fxName, fxIdent);
    const std::string role = inferPluginRole(fxName, fxIdent);
    const std::string category = inferPluginCategory(fxName, fxIdent);
    diag("EnumInstalledFX index=" + std::to_string(i) + " name=\"" + fxName + "\" ident=\"" +
         fxIdent + "\" format=" + format + " role=" + role + " category=" + category);

    if (emitted++) json << ',';
    json << "{\"index\":" << i << ",\"name\":\"" << jsonEscape(fxName)
         << "\",\"ident\":\"" << jsonEscape(fxIdent)
         << "\",\"format\":\"" << jsonEscape(format)
         << "\",\"role\":\"" << role
         << "\",\"category\":\"" << category << "\"}";
  }
  json << "],\"count\":" << emitted << ",\"scanned\":" << scanned << "}";
  diag("tool=reaper.list_plugins result count=" + std::to_string(emitted) +
       " scanned=" + std::to_string(scanned));
  return json.str();
}

std::string mediaFilesForTrack(MediaTrack *track, const std::string &mediaType, bool logItems) {
  std::ostringstream json;
  json << '[';
  int emitted = 0;
  const int itemCount = GetTrackNumMediaItems(track);
  for (int itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
    MediaItem *item = GetTrackMediaItem(track, itemIndex);
    if (!item) continue;
    const double position = GetMediaItemInfo_Value(item, "D_POSITION");
    const double length = GetMediaItemInfo_Value(item, "D_LENGTH");
    const int takeCount = GetMediaItemNumTakes(item);
    for (int takeIndex = 0; takeIndex < takeCount; ++takeIndex) {
      MediaItem_Take *take = GetMediaItemTake(item, takeIndex);
      if (!take) continue;
      PCM_source *source = GetMediaItemTake_Source(take);
      if (!source) continue;
      char typeBuf[64] = {};
      char fileBuf[4096] = {};
      GetMediaSourceType(source, typeBuf, sizeof(typeBuf));
      GetMediaSourceFileName(source, fileBuf, sizeof(fileBuf));
      const std::string type = typeBuf;
      const std::string file = fileBuf;
      const std::string typeLower = lowerAscii(type);
      if (mediaType == "midi" && typeLower != "midi") continue;
      if (mediaType == "audio" && typeLower == "midi") continue;

      if (logItems) {
        diag("media item=" + std::to_string(itemIndex) + " take=" + std::to_string(takeIndex) +
             " type=" + type + " file=\"" + file + "\" pos=" + std::to_string(position) +
             " len=" + std::to_string(length));
      }
      if (emitted++) json << ',';
      json << "{\"item_index\":" << itemIndex << ",\"take_index\":" << takeIndex
           << ",\"source_type\":\"" << jsonEscape(type) << "\",\"file_path\":"
           << nullableJsonString(file) << ",\"embedded\":" << (file.empty() ? "true" : "false")
           << ",\"position_seconds\":" << position << ",\"length_seconds\":" << length << "}";
    }
  }
  json << ']';
  return json.str();
}

std::string mcp_listTracks() {
  const int trackCount = CountTracks(nullptr);
  diag("tool=reaper.list_tracks CountTracks=" + std::to_string(trackCount));
  std::ostringstream json;
  json << "{\"ok\":true,\"tracks\":[";
  for (int i = 0; i < trackCount; ++i) {
    MediaTrack *track = GetTrack(nullptr, i);
    if (!track) continue;
    char nameBuf[256] = {};
    GetTrackName(track, nameBuf, sizeof(nameBuf));
    const int itemCount = GetTrackNumMediaItems(track);
    const int fxCount = TrackFX_GetCount(track);
    const int instrumentIndex = TrackFX_GetInstrument(track);
    int midiCount = 0;
    int audioCount = 0;

    for (int itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
      MediaItem *item = GetTrackMediaItem(track, itemIndex);
      if (!item) continue;
      MediaItem_Take *take = GetActiveTake(item);
      if (!take) continue;
      PCM_source *source = GetMediaItemTake_Source(take);
      if (!source) continue;
      char typeBuf[64] = {};
      char fileBuf[4096] = {};
      GetMediaSourceType(source, typeBuf, sizeof(typeBuf));
      GetMediaSourceFileName(source, fileBuf, sizeof(fileBuf));
      if (lowerAscii(typeBuf) == "midi") ++midiCount;
      else ++audioCount;
      diag("track=" + std::to_string(i) + " item=" + std::to_string(itemIndex) +
           " active_take_type=" + std::string(typeBuf) + " file=\"" + std::string(fileBuf) + "\"");
    }

    std::string kind = "empty";
    if (midiCount > 0 && audioCount > 0) kind = "mixed";
    else if (midiCount > 0 || instrumentIndex >= 0) kind = "midi";
    else if (audioCount > 0) kind = "audio";

    diag("track=" + std::to_string(i) + " name=\"" + std::string(nameBuf) + "\" items=" +
         std::to_string(itemCount) + " fx=" + std::to_string(fxCount) +
         " instrument_index=" + std::to_string(instrumentIndex) + " kind=" + kind);

    if (i) json << ',';
    json << "{\"track_index\":" << i << ",\"track_number\":" << (i + 1)
         << ",\"name\":\"" << jsonEscape(nameBuf) << "\",\"kind\":\"" << kind
         << "\",\"media_items\":" << itemCount << ",\"midi_items\":" << midiCount
         << ",\"audio_items\":" << audioCount << ",\"fx_count\":" << fxCount
         << ",\"instrument_fx_index\":" << instrumentIndex << ",\"fx\":[";
    for (int fx = 0; fx < fxCount; ++fx) {
      char fxName[512] = {};
      TrackFX_GetFXName(track, fx, fxName, sizeof(fxName));
      diag("track=" + std::to_string(i) + " fx=" + std::to_string(fx) + " name=\"" + fxName + "\"");
      if (fx) json << ',';
      json << "{\"index\":" << fx << ",\"name\":\"" << jsonEscape(fxName) << "\"}";
    }
    json << "]}";
  }
  json << "],\"count\":" << trackCount << "}";
  return json.str();
}

std::string mcp_getTrackMediaFiles(const std::string &args) {
  int trackIndex = -1;
  findJsonInt(args, "track_index", &trackIndex);
  std::string mediaType = lowerAscii(findJsonString(args, "media_type"));
  if (mediaType.empty()) mediaType = "all";
  MediaTrack *track = trackFromIndex(trackIndex);
  diag("tool=reaper.get_track_media_files track_index=" + std::to_string(trackIndex) +
       " media_type=" + mediaType + " track_ptr=" + std::to_string(reinterpret_cast<uintptr_t>(track)));
  if (!track) return "{\"ok\":false,\"error\":\"invalid track_index\"}";
  if (mediaType != "all" && mediaType != "midi" && mediaType != "audio") {
    return "{\"ok\":false,\"error\":\"media_type must be all, midi, or audio\"}";
  }
  return "{\"ok\":true,\"track_index\":" + std::to_string(trackIndex) +
         ",\"files\":" + mediaFilesForTrack(track, mediaType, true) + "}";
}

std::string mcp_insertMidiFile(const std::string &args) {
  int trackIndex = -1;
  findJsonInt(args, "track_index", &trackIndex);
  const std::string filePath = findJsonString(args, "file_path");
  double position = 0.0;
  const bool hasPosition = findJsonDouble(args, "position_seconds", &position);
  MediaTrack *track = trackFromIndex(trackIndex);
  diag("tool=reaper.insert_midi_file track_index=" + std::to_string(trackIndex) +
       " file_path=\"" + filePath + "\" has_position=" + std::to_string(hasPosition) +
       " position=" + std::to_string(position) +
       " track_ptr=" + std::to_string(reinterpret_cast<uintptr_t>(track)));
  if (!track) return "{\"ok\":false,\"error\":\"invalid track_index\"}";
  if (filePath.empty()) return "{\"ok\":false,\"error\":\"missing file_path\"}";

  std::ifstream file(filePath.c_str());
  if (!file.good()) return "{\"ok\":false,\"error\":\"file_path is not readable\"}";

  Undo_BeginBlock();
  PreventUIRefresh(1);
  const int beforeItemCount = GetTrackNumMediaItems(track);
  const double oldCursor = GetCursorPosition();
  SetOnlyTrackSelected(track);
  SelectAllMediaItems(nullptr, false);
  if (hasPosition) SetEditCurPos(position, false, false);
  const int mode = 512 | (trackIndex << 16);
  const int rc = InsertMedia(filePath.c_str(), mode);
  if (hasPosition) SetEditCurPos(oldCursor, false, false);
  PreventUIRefresh(-1);
  UpdateArrange();
  Undo_EndBlock("MCP: insert MIDI file", -1);

  const int afterItemCount = GetTrackNumMediaItems(track);
  if (rc && hasPosition && afterItemCount > beforeItemCount) {
    MediaItem *firstNewItem = GetTrackMediaItem(track, beforeItemCount);
    const double firstPosition = firstNewItem ? GetMediaItemInfo_Value(firstNewItem, "D_POSITION") : position;
    for (int i = beforeItemCount; i < afterItemCount; ++i) {
      MediaItem *item = GetTrackMediaItem(track, i);
      if (!item) continue;
      const double oldPosition = GetMediaItemInfo_Value(item, "D_POSITION");
      const double newPosition = position + (oldPosition - firstPosition);
      const bool moved = SetMediaItemInfo_Value(item, "D_POSITION", newPosition);
      diag("InsertMedia reposition item=" + std::to_string(i) +
           " old_pos=" + std::to_string(oldPosition) +
           " new_pos=" + std::to_string(newPosition) +
           " moved=" + std::to_string(moved));
    }
  }
  if (rc && afterItemCount > beforeItemCount) {
    DirectUndoAction undo;
    undo.kind = DirectUndoKind::DeleteMediaItems;
    undo.label = "MCP: insert MIDI file";
    undo.trackIndex = trackIndex;
    for (int i = beforeItemCount; i < afterItemCount; ++i) undo.mediaItemIndexes.push_back(i);
    pushDirectUndo(undo);
  }

  diag("InsertMedia file=\"" + filePath + "\" mode=" + std::to_string(mode) +
       " rc=" + std::to_string(rc) + " old_cursor=" + std::to_string(oldCursor) +
       " before_items=" + std::to_string(beforeItemCount) +
       " after_items=" + std::to_string(afterItemCount));
  return "{\"ok\":" + std::string(rc ? "true" : "false") + ",\"track_index\":" +
         std::to_string(trackIndex) + ",\"file_path\":\"" + jsonEscape(filePath) +
         "\",\"insert_media_return\":" + std::to_string(rc) + "}";
}

std::string mcp_insertPlugin(const std::string &args) {
  int trackIndex = -1;
  findJsonInt(args, "track_index", &trackIndex);
  const std::string plugin = findJsonString(args, "plugin");
  const std::string pluginType = findJsonString(args, "plugin_type");
  bool alwaysCreate = true;
  findJsonBool(args, "always_create", &alwaysCreate);
  MediaTrack *track = trackFromIndex(trackIndex);
  if (!track) return "{\"ok\":false,\"error\":\"invalid track_index\"}";
  if (plugin.empty()) return "{\"ok\":false,\"error\":\"missing plugin\"}";

  std::string search = plugin;
  if (!pluginType.empty()) search = pluginType + ":" + plugin;
  const int instantiate = alwaysCreate ? -1 : 1;
  diag("tool=reaper.insert_plugin track_index=" + std::to_string(trackIndex) +
       " plugin=\"" + plugin + "\" plugin_type=\"" + pluginType + "\" search=\"" + search +
       "\" instantiate=" + std::to_string(instantiate));

  Undo_BeginBlock();
  PreventUIRefresh(1);
  const int fxIndex = TrackFX_AddByName(track, search.c_str(), false, instantiate);
  char resolvedName[512] = {};
  if (fxIndex >= 0) TrackFX_GetFXName(track, fxIndex, resolvedName, sizeof(resolvedName));
  PreventUIRefresh(-1);
  UpdateArrange();
  Undo_EndBlock("MCP: insert plugin", -1);

  if (fxIndex >= 0) {
    DirectUndoAction undo;
    undo.kind = DirectUndoKind::DeleteFx;
    undo.label = "MCP: insert plugin";
    undo.trackIndex = trackIndex;
    undo.fxIndex = fxIndex;
    pushDirectUndo(undo);
  }

  diag("TrackFX_AddByName search=\"" + search + "\" fx_index=" + std::to_string(fxIndex) +
       " resolved_name=\"" + std::string(resolvedName) + "\"");
  if (fxIndex < 0) return "{\"ok\":false,\"error\":\"TrackFX_AddByName failed\",\"requested\":\"" +
                         jsonEscape(search) + "\"}";
  return "{\"ok\":true,\"track_index\":" + std::to_string(trackIndex) +
         ",\"fx_index\":" + std::to_string(fxIndex) + ",\"requested\":\"" + jsonEscape(search) +
         "\",\"name\":\"" + jsonEscape(resolvedName) + "\"}";
}

std::string mcp_undo(const std::string &args) {
  bool preferNative = true;
  findJsonBool(args, "prefer_native", &preferNative);
  const char *undoName = Undo_CanUndo2(nullptr);
  const std::string nativeLabel = undoName ? undoName : "";
  diag("tool=reaper.undo prefer_native=" + std::to_string(preferNative) +
       " Undo_CanUndo2=\"" + nativeLabel + "\"");
  if (preferNative && startsWith(nativeLabel, "MCP:")) {
    const int rc = Undo_DoUndo2(nullptr);
    UpdateArrange();
    diag("Undo_DoUndo2 rc=" + std::to_string(rc));
    if (rc) discardDirectUndoIfLabelMatches(nativeLabel);
    return "{\"ok\":" + std::string(rc ? "true" : "false") + ",\"native\":true,\"undone\":\"" +
           jsonEscape(nativeLabel) + "\",\"undo_return\":" + std::to_string(rc) + "}";
  }

  DirectUndoAction action;
  if (popDirectUndo(&action)) return runDirectUndoAction(action);
  return "{\"ok\":false,\"error\":\"nothing to undo\",\"native_top\":\"" + jsonEscape(nativeLabel) + "\"}";
}

std::string toolListJson() {
  return "{\"tools\":["
         "{\"name\":\"reaper.create_track\",\"description\":\"Create a new track in the current REAPER project.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
         "{\"name\":\"reaper.insert_midi_file\",\"description\":\"Insert a MIDI file onto a track.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"track_index\":{\"type\":\"integer\"},\"file_path\":{\"type\":\"string\"},\"position_seconds\":{\"type\":\"number\"}},\"required\":[\"track_index\",\"file_path\"]}},"
         "{\"name\":\"reaper.list_plugins\",\"description\":\"List installed REAPER plugins with compact inferred roles/categories.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":\"integer\"},\"query\":{\"type\":\"string\"}}}},"
         "{\"name\":\"reaper.list_tracks\",\"description\":\"List current tracks with media and FX summaries.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
         "{\"name\":\"reaper.insert_plugin\",\"description\":\"Insert a plugin on a track.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"track_index\":{\"type\":\"integer\"},\"plugin\":{\"type\":\"string\"},\"plugin_type\":{\"type\":\"string\"},\"always_create\":{\"type\":\"boolean\"}},\"required\":[\"track_index\",\"plugin\"]}},"
         "{\"name\":\"reaper.get_track_media_files\",\"description\":\"List media source files on a track.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"track_index\":{\"type\":\"integer\"},\"media_type\":{\"type\":\"string\"}},\"required\":[\"track_index\"]}},"
         "{\"name\":\"reaper.undo\",\"description\":\"Undo the most recent MCP action, using native undo for MCP undo blocks or a direct rollback stack as fallback.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"prefer_native\":{\"type\":\"boolean\"}}}}"
         "]}";
}

std::string toolCallResultJson(const std::string &payload) {
  return "{\"content\":[{\"type\":\"text\",\"text\":\"" + jsonEscape(payload) + "\"}]}";
}

std::string handleJsonRpc(const std::string &body) {
  const std::string id = findJsonId(body);
  const std::string method = findJsonString(body, "method");

  if (method == "initialize") {
    return makeJsonResult(id, "{\"protocolVersion\":\"2024-11-05\",\"serverInfo\":{\"name\":\"reaper-cpp-mcp\",\"version\":\"0.1.0\"},\"capabilities\":{\"tools\":{}}}");
  }
  if (method == "notifications/initialized") {
    return {};
  }
  if (method == "tools/list") {
    return makeJsonResult(id, toolListJson());
  }
  if (method == "tools/call") {
    const std::string toolName = findJsonString(body, "name");
    const std::string args = findJsonObject(body, "arguments");
    if (toolName == "reaper.create_track") {
      const std::string result = runOnMainThread(mcp_createTrack);
      return makeJsonResult(id, toolCallResultJson(result));
    }
    if (toolName == "reaper.insert_midi_file") {
      const std::string result = runOnMainThread([args] { return mcp_insertMidiFile(args); });
      return makeJsonResult(id, toolCallResultJson(result));
    }
    if (toolName == "reaper.list_plugins") {
      const std::string result = runOnMainThread([args] { return mcp_listPlugins(args); });
      return makeJsonResult(id, toolCallResultJson(result));
    }
    if (toolName == "reaper.list_tracks") {
      const std::string result = runOnMainThread(mcp_listTracks);
      return makeJsonResult(id, toolCallResultJson(result));
    }
    if (toolName == "reaper.insert_plugin") {
      const std::string result = runOnMainThread([args] { return mcp_insertPlugin(args); });
      return makeJsonResult(id, toolCallResultJson(result));
    }
    if (toolName == "reaper.get_track_media_files") {
      const std::string result = runOnMainThread([args] { return mcp_getTrackMediaFiles(args); });
      return makeJsonResult(id, toolCallResultJson(result));
    }
    if (toolName == "reaper.undo") {
      const std::string result = runOnMainThread([args] { return mcp_undo(args); });
      return makeJsonResult(id, toolCallResultJson(result));
    }
    return makeJsonError(id, -32602, "unknown tool: " + toolName);
  }

  return makeJsonError(id, -32601, "method not found: " + method);
}

void closeSocket(socket_t sock) {
#if defined(_WIN32)
  closesocket(sock);
#else
  close(sock);
#endif
}

bool isValidSocket(socket_t sock) {
#if defined(_WIN32)
  return sock != INVALID_SOCKET;
#else
  return sock >= 0;
#endif
}

void shutdownSocket(socket_t sock) {
  if (!isValidSocket(sock)) return;
#if defined(_WIN32)
  shutdown(sock, SD_BOTH);
#else
  shutdown(sock, SHUT_RDWR);
#endif
}

void setSocketTimeouts(socket_t sock) {
#if defined(_WIN32)
  DWORD timeoutMs = 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));
#else
  timeval timeout {};
  timeout.tv_sec = 1;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

std::string readHttpRequest(socket_t client) {
  std::string request;
  char buffer[4096];
  while (request.find("\r\n\r\n") == std::string::npos) {
    const int n = recv(client, buffer, sizeof(buffer), 0);
    if (n <= 0) return request;
    request.append(buffer, buffer + n);
    if (request.size() > 1024 * 1024) return request;
  }

  size_t contentLength = 0;
  const std::string header = request.substr(0, request.find("\r\n\r\n"));
  const std::string marker = "Content-Length:";
  size_t pos = header.find(marker);
  if (pos != std::string::npos) {
    pos += marker.size();
    while (pos < header.size() && std::isspace(static_cast<unsigned char>(header[pos]))) ++pos;
    contentLength = static_cast<size_t>(std::strtoul(header.c_str() + pos, nullptr, 10));
  }

  const size_t bodyStart = request.find("\r\n\r\n") + 4;
  while (request.size() - bodyStart < contentLength) {
    const int n = recv(client, buffer, sizeof(buffer), 0);
    if (n <= 0) break;
    request.append(buffer, buffer + n);
  }
  return request;
}

void sendHttpResponse(socket_t client, int status, const std::string &body) {
  const char *reason = status == 200 ? "OK" : "Bad Request";
  std::ostringstream response;
  response << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
           << "Content-Type: application/json\r\n"
           << "Access-Control-Allow-Origin: *\r\n"
           << "Connection: close\r\n"
           << "Content-Length: " << body.size() << "\r\n\r\n"
           << body;
  const std::string text = response.str();
  send(client, text.data(), static_cast<int>(text.size()), 0);
}

void handleHttpClient(socket_t client) {
  setSocketTimeouts(client);
  const std::string request = readHttpRequest(client);
  const size_t headerEnd = request.find("\r\n\r\n");
  if (headerEnd == std::string::npos) {
    sendHttpResponse(client, 400, "{\"error\":\"invalid HTTP request\"}");
    closeSocket(client);
    return;
  }

  if (request.compare(0, 7, "OPTIONS") == 0) {
    sendHttpResponse(client, 200, "{}");
    closeSocket(client);
    return;
  }

  const std::string body = request.substr(headerEnd + 4);
  const std::string rpcResponse = handleJsonRpc(body);
  sendHttpResponse(client, 200, rpcResponse.empty() ? "{}" : rpcResponse);
  closeSocket(client);
}

void serverLoop() {
  g_listenSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (!isValidSocket(g_listenSocket)) {
    log("could not create server socket: " + lastSocketError());
    g_running = false;
    return;
  }

  int reuse = 1;
  setsockopt(g_listenSocket, SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char *>(&reuse), sizeof(reuse));

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(g_serverPort));
  inet_pton(AF_INET, kServerHost, &addr.sin_addr);

  if (bind(g_listenSocket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    log("could not bind " + std::string(kServerHost) + ":" + std::to_string(g_serverPort) +
        ": " + lastSocketError());
    closeSocket(g_listenSocket);
    g_listenSocket = -1;
    g_running = false;
    return;
  }

  if (listen(g_listenSocket, 8) < 0) {
    log("could not listen on server socket: " + lastSocketError());
    closeSocket(g_listenSocket);
    g_listenSocket = -1;
    g_running = false;
    return;
  }

  log("MCP HTTP server listening on http://127.0.0.1:" + std::to_string(g_serverPort));
  while (g_running.load()) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(g_listenSocket, &readSet);
    timeval timeout {};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    const int ready = select(static_cast<int>(g_listenSocket + 1), &readSet, nullptr, nullptr, &timeout);
    if (ready <= 0 || !FD_ISSET(g_listenSocket, &readSet)) continue;

    sockaddr_in clientAddr {};
    socklen_t clientLen = sizeof(clientAddr);
    const socket_t client = accept(g_listenSocket, reinterpret_cast<sockaddr *>(&clientAddr), &clientLen);
    if (isValidSocket(client)) handleHttpClient(client);
  }

  if (isValidSocket(g_listenSocket)) closeSocket(g_listenSocket);
  g_listenSocket = -1;
}

bool startServer() {
  if (g_running.exchange(true)) return true;
  g_serverPort = configuredServerPort();
  g_serverThread = std::thread(serverLoop);
  return true;
}

void stopServer() {
  g_running = false;
  shutdownSocket(g_listenSocket);

  {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    for (const auto &job : g_jobs) {
      {
        std::lock_guard<std::mutex> jobLock(job->mutex);
        job->done = true;
        job->result = "{\"ok\":false,\"error\":\"server stopped\"}";
      }
      job->cv.notify_one();
    }
    g_jobs.clear();
  }

  if (g_serverThread.joinable()) g_serverThread.join();
}

} // namespace

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t *rec) {
  if (!rec) {
    if (plugin_register) plugin_register("-timer", reinterpret_cast<void *>(processMainThreadJobs));
    stopServer();
    return 0;
  }

  if (rec->caller_version != REAPER_PLUGIN_VERSION || !rec->GetFunc) return 0;

  const int loadError = REAPERAPI_LoadAPI(rec->GetFunc);
  if (loadError) {
    std::fprintf(stderr, "[reaper-mcp] REAPERAPI_LoadAPI failed: %d missing functions\n", loadError);
    return 0;
  }

  plugin_register("timer", reinterpret_cast<void *>(processMainThreadJobs));
  startServer();
  log("extension loaded");

  return 1;
}
