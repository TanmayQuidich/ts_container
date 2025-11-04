#include <gst/gst.h>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>

//
// ==== Globals (no CLI) ====
static const char*  kOutTsFile = "audio.ts";
static const int    kRate      = 48000;   // Hz
static const int    kChannels  = 1;       // mono
static const int    kBitrate   = 192000;   // Opus target bps
static const double kFrameMs   = 2.5;     // Opus frame (2.5 fits 1–3 ms)
//
// ==========================

static std::atomic<bool> g_quit{false};
static std::atomic<uint64_t> g_buf_count{0};

static void log_env_hint(const char* name) {
  const char* v = std::getenv(name);
  std::cout << "ENV " << name << " = " << (v ? v : "<unset>") << "\n";
}

static bool check_factory(const char* name) {
  GstElementFactory* f = gst_element_factory_find(name);
  if (!f) {
    std::cerr << "[FATAL] Missing element factory: " << name << "\n";
    return false;
  }
  std::cout << "[OK] Found element: " << name << "\n";
  gst_object_unref(f);
  return true;
}

static void log_state_change_msg(GstMessage* msg) {
  if (GST_MESSAGE_TYPE(msg) != GST_MESSAGE_STATE_CHANGED) return;
  if (!GST_IS_ELEMENT(GST_MESSAGE_SRC(msg))) return;

  GstState old_s, new_s, pending_s;
  gst_message_parse_state_changed(msg, &old_s, &new_s, &pending_s);
  const gchar* name = GST_OBJECT_NAME(GST_MESSAGE_SRC(msg));
  std::cout << "[STATE] " << name << ": "
            << gst_element_state_get_name(old_s) << " -> "
            << gst_element_state_get_name(new_s)
            << (pending_s != GST_STATE_VOID_PENDING
                ? (" (pending " + std::string(gst_element_state_get_name(pending_s)) + ")")
                : "")
            << "\n";
}

static gboolean on_bus(GstBus*, GstMessage* msg, gpointer user_data) {
  GMainLoop* loop = static_cast<GMainLoop*>(user_data);

  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError* err = nullptr; gchar* dbg = nullptr;
      gst_message_parse_error(msg, &err, &dbg);
      std::cerr << "[ERROR] " << (err ? err->message : "unknown") << "\n";
      if (dbg) { std::cerr << "[DEBUG] " << dbg << "\n"; g_free(dbg); }
      if (err) g_error_free(err);
      if (loop) g_main_loop_quit(loop);
      break;
    }
    case GST_MESSAGE_EOS:
      std::cout << "[BUS] EOS received – stopping.\n";
      if (loop) g_main_loop_quit(loop);
      break;
    case GST_MESSAGE_WARNING: {
      GError* err = nullptr; gchar* dbg = nullptr;
      gst_message_parse_warning(msg, &err, &dbg);
      std::cerr << "[WARN] " << (err ? err->message : "unknown") << "\n";
      if (dbg) { std::cerr << "[DEBUG] " << dbg << "\n"; g_free(dbg); }
      if (err) g_error_free(err);
      break;
    }
    case GST_MESSAGE_STATE_CHANGED:
      log_state_change_msg(msg);
      break;
    default:
      break;
  }
  return TRUE;
}

// Pad probe to count written buffers and log rate (~once/sec at typical rates).
static GstPadProbeReturn filesink_probe(GstPad*, GstPadProbeInfo*, gpointer) {
  uint64_t n = ++g_buf_count;
  if (n % 400 == 0) { // Opus 2.5ms ~= 400 buffers/sec if 1 frame per AU
    std::cout << "[IO] wrote ~" << n << " TS buffers\n";
  }
  return GST_PAD_PROBE_OK;
}

int main(int argc, char** argv) {
  gst_debug_set_default_threshold(GST_LEVEL_INFO);

  std::cout << "=== audio-lockstep ===\n";
  std::cout << "Mic -> Opus(" << kFrameMs << " ms) -> MPEG-TS file: " << kOutTsFile << "\n";
  std::cout << "Config: rate=" << kRate << " Hz, channels=" << kChannels
            << ", bitrate=" << kBitrate << " bps\n";
  log_env_hint("GSTREAMER_1_0_ROOT_X86_64");
  log_env_hint("PATH");
  log_env_hint("GST_PLUGIN_PATH_1_0");
  log_env_hint("GST_PLUGIN_SYSTEM_PATH_1_0");

  gst_init(&argc, &argv);

  // Check required plugins first.
  bool ok = true;
  ok &= check_factory("wasapisrc");
  ok &= check_factory("audioconvert");
  ok &= check_factory("audioresample");
  ok &= check_factory("opusenc");
  ok &= check_factory("mpegtsmux");
  ok &= check_factory("filesink");
  if (!ok) {
    std::cerr << "Missing plugins. Ensure GStreamer MSVC runtime/dev are installed and PATH includes <gstreamer>\\bin.\n";
    return 2;
  }

  // IMPORTANT FIX: Convert/resample BEFORE forcing caps to S16LE/48k/mono.
  // This avoids not-negotiated on devices that don't natively output S16LE@48k.
  char pipeline_str[1024];
  std::snprintf(
    pipeline_str, sizeof(pipeline_str),
    "wasapisrc low-latency=true do-timestamp=true ! "
    "audioconvert ! audioresample ! "
    "audio/x-raw,layout=interleaved,format=S16LE,rate=%d,channels=%d ! "
    "opusenc frame-size=%g bitrate=%d bandwidth=fullband "
    "! mpegtsmux name=mux "
    "! filesink name=outsink location=\"%s\"",
    kRate, kChannels, kFrameMs, kBitrate, kOutTsFile
  );

  std::cout << "[PIPELINE]\n  " << pipeline_str << "\n";

  GError* err = nullptr;
  GstElement* pipeline = gst_parse_launch(pipeline_str, &err);
  if (!pipeline) {
    std::cerr << "[FATAL] Failed to create pipeline: " << (err ? err->message : "unknown") << "\n";
    if (err) g_error_free(err);
    return 1;
  }

  // Create main loop then add ONE bus watch (fixed).
  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  GstBus* bus = gst_element_get_bus(pipeline);
  gst_bus_add_watch(bus, on_bus, loop);
  gst_object_unref(bus);

  // Add a probe on filesink to show progress.
  GstElement* outsink = gst_bin_get_by_name(GST_BIN(pipeline), "outsink");
  if (outsink) {
    GstPad* sinkpad = gst_element_get_static_pad(outsink, "sink");
    if (sinkpad) {
      gst_pad_add_probe(sinkpad, GST_PAD_PROBE_TYPE_BUFFER, filesink_probe, nullptr, nullptr);
      gst_object_unref(sinkpad);
    }
    gst_object_unref(outsink);
  }

  // Stop on ENTER from a side thread (so the app won’t exit immediately).
  std::thread stopper([&](){
    std::cout << "\n[RUN] Writing TS to: " << kOutTsFile << "\n";
    std::cout << "[RUN] Press ENTER to stop...\n";
    std::string dummy; std::getline(std::cin, dummy);
    g_quit = true;
    g_main_loop_quit(loop);
  });

  std::cout << "[STATE] Setting pipeline -> PLAYING\n";
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "[FATAL] Failed to set pipeline to PLAYING.\n";
    if (stopper.joinable()) stopper.join();
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    return 3;
  }

  g_main_loop_run(loop);

  std::cout << "[STATE] Stopping pipeline...\n";
  gst_element_set_state(pipeline, GST_STATE_NULL);
  if (stopper.joinable()) stopper.join();
  gst_object_unref(pipeline);
  g_main_loop_unref(loop);

  std::cout << "[DONE] Exiting.\n";
  return 0;
}
