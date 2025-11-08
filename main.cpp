#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <glib.h>
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <vector>
#include <filesystem>
#include <string>
#include <algorithm>
#include <limits>
#include <hiredis/hiredis.h>

namespace fs = std::filesystem;
static guint64 frame_counter = 0;
static guint64 audio_frame_counter = 0;

// Remove "static const" to make these configurable
static guint64 current_index = 0;
static guint TARGET_FPS = 0;
static double FrameIntervalMs = 0.0;

static guint64 initial_pts_base = 0;
static guint64 pts_increment = 0;

static std::string FRAME_FOLDER;
static std::ofstream csv_output;
static std::ofstream csv_output_audio;
static std::ofstream csv_output_summary;

std::string camera_prefix;

// Helper struct to pass into probes
struct ProbeData {
    std::ofstream *csv;           // main csv (video)
    std::ofstream *csv_summary;   // summary csv
    redisContext *redis;          // redis context (may be nullptr)
};

// === Helper Functions ===
guint64 find_first_index_fast(const std::string& folder) {
    for (const auto& entry : fs::directory_iterator(folder)) {
        if (entry.is_regular_file()) {
            
            std::string fname = entry.path().filename().string();
            if (fname.find("frame_camera01_") == 0 && fname.find(".hevc") != std::string::npos) {
                std::string number_str = fname.substr(
                    std::string("frame_camera01_").size(),
                    fname.size() - std::string("frame_camera01_").size() - 5
                );
                try {
                    return std::stoull(number_str);
                } catch (...) {
                    throw std::runtime_error("[error] Invalid file name: " + fname);
                }
            }
        }
    }
    throw std::runtime_error("[error] No valid files found in folder: " + folder);
}

std::string make_frame_filename(guint64 idx) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "frame_%s_%09" G_GUINT64_FORMAT ".hevc", camera_prefix.c_str(), idx);
    return std::string(buf);
}

bool is_file_ready(const fs::path& path, int max_attempts = 5, int delay_ms = 2) {
    if (!fs::exists(path)) return false;
    auto last_size = fs::file_size(path);
    for (int i = 0; i < max_attempts; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        if (!fs::exists(path)) return false;
        auto new_size = fs::file_size(path);
        if (new_size == last_size) return true;
        last_size = new_size;
    }
    return false;
}

static constexpr std::size_t IFRAME_MIN_SIZE = 30 * 1024; // 30 KB

bool is_iframe(const fs::path& path) {
    try {
        if (!fs::exists(path)) return false;
        return fs::file_size(path) >= IFRAME_MIN_SIZE;
    } catch (...) {
        return false;
    }
}

// ---------------------- Video probe (writes actual buffer PTS -> 90kHz and Redis fields) ----------------------
static GstPadProbeReturn video_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    // user_data is a ProbeData*
    ProbeData* pdata = static_cast<ProbeData*>(user_data);
    static std::string prev_ball = "0", prev_over = "0", prev_innings = "0";

    if (!(info->type & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }

    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;

    // Get PTS
    GstClockTime pts = GST_BUFFER_PTS(buffer);

    // Read offset (we set it in feed_frames to the file index)
    std::string fname = make_frame_filename(frame_counter);
    std::string redis_key = fname.substr(0, fname.find_last_of('.'));

    // Prepare CSV fields with defaults
    std::string ball = "1", frame_name = "NA", innings = "1", isStart = "false", matchID = "123", over = "1", ptp_timestamp = "NA", received_at = "NA";

    // If we have a redisContext, try GET
    if (pdata && pdata->redis) {
        redisReply* reply = (redisReply*)redisCommand(pdata->redis, "GET %s", redis_key.c_str());
        if (reply && reply->type == REDIS_REPLY_STRING) {
            std::string json = reply->str;
            auto extract = [&](const std::string& key) -> std::string {
                size_t pos = json.find("\"" + key + "\":");
                if (pos == std::string::npos) return "NA";
                pos += key.size() + 3; // move past "key":
                if (pos >= json.size()) return "NA";
                // detect value type
                if (json[pos] == '"') {
                    size_t end = json.find('"', pos + 1);
                    if (end == std::string::npos) return "NA";
                    return json.substr(pos + 1, end - pos - 1);
                } else {
                    size_t end = json.find_first_of(",}", pos);
                    if (end == std::string::npos) return json.substr(pos);
                    return json.substr(pos, end - pos);
                }
            };

            ball          = extract("ball");
            innings       = extract("innings");
            isStart       = extract("isStart");
            matchID       = extract("matchID");
            over          = extract("over");
            frame_name    = extract("frame_name");
            ptp_timestamp = extract("ptp_timestamp");
            received_at   = extract("received_at");
        }
        if (reply) freeReplyObject(reply);
    }

    // Convert PTS (ns) to 90kHz ticks
    if (pts != GST_CLOCK_TIME_NONE) {
        guint64 pts_90k = gst_util_uint64_scale(pts, 90000, GST_SECOND);

        // Write one line to the main CSV
        if (pdata && pdata->csv && pdata->csv->is_open()) {
            *(pdata->csv) << frame_counter << "," << pts_90k << "," << fname << ","
                          << ball << "," << frame_name << "," << innings << "," << isStart << ","
                          << matchID << "," << over << "," << ptp_timestamp << "," << received_at << "\n";
            pdata->csv->flush();
        }

        // Write summary when values change
        if (pdata && pdata->csv_summary && pdata->csv_summary->is_open()) {
            if (ball != prev_ball || over != prev_over || innings != prev_innings) {
                *(pdata->csv_summary) << frame_counter << "," << pts_90k << "," << over << "," << ball << "," << innings << "," << matchID << "\n";
                pdata->csv_summary->flush();
            }
        }

        // Console log
        // std::cout << "[VIDEO] FrameIndex: " << frame_counter << " PTS_90k: " << pts_90k << " File: " << fname << std::endl;
    } else {
        // No PTS, still write NA entry for PTS
        if (pdata && pdata->csv && pdata->csv->is_open()) {
            *(pdata->csv) << frame_counter << ",NA," << fname << ","
                          << ball << "," << frame_name << "," << innings << "," << isStart << ","
                          << matchID << "," << over << "," << ptp_timestamp << "," << received_at << "\n";
            pdata->csv->flush();
        }
        // std::cout << "[VIDEO] FrameIndex: " << frame_counter << " PTS: NONE File: " << fname << std::endl;
    }

    // update previous-tracked values for summary
    prev_ball = ball;
    prev_over = over;
    prev_innings = innings;

    return GST_PAD_PROBE_OK;
}

// ---------------------- Audio probe (unchanged; logs actual PTS -> 90kHz) ----------------------
static GstPadProbeReturn audio_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        GstClockTime pts = GST_BUFFER_PTS(buffer);

        if (pts != GST_CLOCK_TIME_NONE) {
            guint64 pts_90k = gst_util_uint64_scale(pts, 90000, GST_SECOND); // Convert ns → 90kHz
            std::ofstream* csv_audio = static_cast<std::ofstream*>(user_data);

            // Log to CSV
            *csv_audio << audio_frame_counter<< ","<< pts_90k << "\n";
            csv_audio->flush();

            audio_frame_counter++;

            // Log to console
            // std::cout << "[AUDIO] Real PTS: " << pts_90k << std::endl;
        }
    }
    return GST_PAD_PROBE_OK;
}

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    GMainLoop *loop = (GMainLoop *)data;
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err;
        gchar *dbg;
        gst_message_parse_error(msg, &err, &dbg);
        g_printerr("ERROR from %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
        if (dbg) g_printerr("Debug details: %s\n", dbg);
        g_error_free(err);
        g_free(dbg);
        g_main_loop_quit(loop);
        break;
    }
    case GST_MESSAGE_EOS:
        g_print("EOS received\n");
        g_main_loop_quit(loop);
        break;
    default:
        break;
    }
    return TRUE;
}

void feed_frames(GstElement *appsrc, redisContext* context){
    using clock = std::chrono::steady_clock;
    auto start_time = clock::now();
    auto frame_duration = std::chrono::microseconds(static_cast<int>(FrameIntervalMs * 1000));
    // custom PTS removed — we rely on actual buffer PTS as set below
    static const std::vector<guint64> increments =
        (TARGET_FPS == 150) ? std::vector<guint64>{599, 600, 601}
                            : std::vector<guint64>{299, 300, 301};

    std::string prev_ball="0", prev_over="0", prev_innings="0";

    while (true) {
        // Calculate the expected time for the current frame
        auto expected_time = start_time + frame_counter * frame_duration;
        auto now = clock::now();

        // Sleep until the expected time for the next frame
        if (now < expected_time) {
            std::this_thread::sleep_until(expected_time);
        } else {
            // Log if we're significantly behind schedule
            auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - expected_time).count();
            if (delta > FrameIntervalMs) {
                std::cerr << "[feed] Warning: Behind schedule by " << delta << " ms at frame " << frame_counter << "\n";
            }
        }

        // Construct the frame filename (use current_index)
        std::string fname = make_frame_filename(current_index);
        fs::path fullpath = fs::path(FRAME_FOLDER) / fname;

        // Check if file is ready
        if (!is_file_ready(fullpath)) {
            std::cerr << "[feed] File not found or not ready: " << fullpath << ". Waiting...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Short wait before retry
            continue; // Retry the same frame
        }

        // === SKIP NON-I-FRAMES ===
        if (!is_iframe(fullpath)) {
            std::cerr << "[feed] SKIP P/B-frame: " << fname
                      << " (" << fs::file_size(fullpath)/1024 << " KB)\n";
            current_index++;
            continue;
        }
        // === END SKIP ===

        std::ifstream ifs(fullpath, std::ios::binary | std::ios::ate);
        if (!ifs) {
            std::cerr << "[feed] Failed to open " << fullpath << ". Retrying...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue; // Retry the same frame
        }
        std::streamsize size = ifs.tellg();
        ifs.seekg(0, std::ios::beg);
        std::vector<uint8_t> bufferdata(size);
        if (!ifs.read(reinterpret_cast<char*>(bufferdata.data()), size)) {
            std::cerr << "[feed] Failed reading " << fullpath << ". Retrying...\n";
            ifs.close();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue; // Retry the same frame
        }
        ifs.close();

        // Create and fill GStreamer buffer
        GstBuffer *buffer = gst_buffer_new_allocate(NULL, size, NULL);
        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            std::cerr << "[feed] Buffer map failed\n";
            gst_buffer_unref(buffer);
            break; // Exit on critical error
        }
        memcpy(map.data, bufferdata.data(), size);
        gst_buffer_unmap(buffer, &map);

        // Set buffer timestamps (use clock-based PTS derived from frame_counter)
        // GstClockTime pts = gst_util_uint64_scale(frame_counter, GST_SECOND, TARGET_FPS);
        // GST_BUFFER_PTS(buffer) = pts;
        // GST_BUFFER_DTS(buffer) = pts;
        // GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, TARGET_FPS);

        // Attach the file index to the buffer so the probe can reconstruct filename
        GST_BUFFER_OFFSET(buffer) = current_index;

        // Push buffer to appsrc
        GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
        if (ret != GST_FLOW_OK) {
            std::cerr << "[feed] appsrc_push_buffer returned " << ret << "\n";
            gst_buffer_unref(buffer);
            break; // Exit on critical error
        }

        std::cerr << "[feed] Pushed frame " << frame_counter << " (" << fname << ")\n";

        // Increment counters AFTER setting offset and pushing
        frame_counter++;
        current_index++;

        // Log FPS statistics
        static auto last_log = clock::now();
        if (frame_counter % TARGET_FPS == 0) {
            auto now2 = clock::now();
            auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now2 - last_log).count();
            std::cerr << "[stats] Last " << TARGET_FPS << " frames in " << delta << " ms (FPS: " << (TARGET_FPS * 1000.0 / delta) << ")\n";
            last_log = now2;
        }
    }
}

int main(int argc, char *argv[]) {
    // Check for proper usage
    if (argc < 7) {
        std::cerr << "Usage: " << argv[0]
                  << " <start_index> <target_fps> <input_folder> <output_ts_file> <output_csv_file> <camera_id>\n";
        return 1;
    }
    // Connect to DragonflyDB (Redis-compatible)
    redisContext* context = redisConnect("192.168.5.102", 6379);
    if (context == nullptr || context->err) {
        if (context) {
            std::cerr << "Redis connection error: " << context->errstr << std::endl;
            redisFree(context);
        } else {
            std::cerr << "Cannot allocate Redis context" << std::endl;
        }
        // Not fatal if Redis unavailable — we can proceed without Redis lookups if desired.
        context = nullptr;
    } else {
        std::cout << "[redis] Connected successfully to DragonflyDB\n";
    }

    // Parse arguments
    
 
    FRAME_FOLDER = argv[3];                   // e.g. D:\path\to\Camera_1

    current_index = std::stoull(argv[1]);    // e.g. 2379000
    if(current_index == 0){
        current_index = find_first_index_fast(FRAME_FOLDER) + 6000;
    }
    std::cout <<current_index;

    // Parse start_index safely

    TARGET_FPS = static_cast<guint>(std::stoi(argv[2]));  // e.g. 300
    FrameIntervalMs = 1000.0 / TARGET_FPS;

    // Calculate PTS values for MPEG-TS (90kHz clock)
    initial_pts_base = current_index * 100;
    pts_increment = 90000 / TARGET_FPS;


    std::string output_ts_path = argv[4];     // e.g. E:\output.ts
    std::string csv_filename = argv[5];       // e.g. output_full.csv
    std::string camera_id = argv[6];          // e.g. camera02

    // Second CSV file (summary)
    std::string csv_filename_summary = "summary_" + camera_id + ".csv";
    std::string csv_filename_audio = "audio_" + camera_id + ".csv";
    camera_prefix = camera_id;
    std::cout << "[config] Starting from index: " << current_index << "\n";
    std::cout << "[config] Target FPS: " << TARGET_FPS << "\n";
    std::cout << "[config] Frame Interval (ms): " << FrameIntervalMs << "\n";

    gst_init(&argc, &argv);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    //===============Video-Pipeline===========================//
    GstElement *pipeline = gst_pipeline_new("appsrc-pipeline");
    GstElement *appsrc    = gst_element_factory_make("appsrc", "my-appsrc");
    GstElement *h265parser = gst_element_factory_make("h265parse", "parser");
    GstElement *queue1 = gst_element_factory_make("queue", "queue1");  // add queue
    GstElement *mpegtsmux = gst_element_factory_make("mpegtsmux", "ts-muxer");
    GstElement *filesink = gst_element_factory_make("filesink", "ts-output");

    //=======================Audio-pipeline (OPUS)=============================//
    GstElement *a_src          = gst_element_factory_make("souphttpsrc", "a-http");
    GstElement *a_caps         = gst_element_factory_make("capsfilter", "a-caps");
    GstElement *a_queue1       = gst_element_factory_make("queue", "a-queue1");
    GstElement *a_convert      = gst_element_factory_make("audioconvert", "a-convert");
    GstElement *a_resample     = gst_element_factory_make("audioresample", "a-resample");
    GstElement *a_rate         = gst_element_factory_make("audiorate", "a-rate");
    GstElement *a_split        = gst_element_factory_make("audiobuffersplit", "a-split");
    GstElement *a_enc          = gst_element_factory_make("opusenc", "a-opusenc");   // OPUS
    GstElement *a_parse        = gst_element_factory_make("opusparse", "a-opusparse"); // OPUS
    GstElement *a_queue3       = gst_element_factory_make("queue", "a-queue3");
    GstElement *a_queue2       = gst_element_factory_make("queue", "a-queue2");

    if (!pipeline || !appsrc || !h265parser || !queue1 || !mpegtsmux ||
        !a_src || !a_caps || !a_queue1 || !a_convert || !a_resample ||
        !a_rate || !a_split || !a_enc  || !a_parse ||
        !a_queue3 || !a_queue2 || !filesink) {
        std::cerr << "[error] Failed to create elements\n";
        if (context) redisFree(context);
        return -1;
    }

    // Configure appsrc
    g_object_set(G_OBJECT(appsrc),
                 "format", GST_FORMAT_TIME,
                 "is-live", TRUE,
                "do-timestamp", TRUE,   // <--- IMPORTANT
                 "stream-type", GST_APP_STREAM_TYPE_STREAM,
                 NULL);

    GstCaps * caps = gst_caps_new_simple(
        "video/x-h265",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment",    G_TYPE_STRING, "au",
        "framerate",    GST_TYPE_FRACTION, TARGET_FPS, 1,
        NULL);
    g_object_set(G_OBJECT(appsrc), "caps", caps, NULL);
    gst_caps_unref(caps);

    g_object_set(G_OBJECT(a_src),
             "location", "http://192.168.5.100:53354/audio",
             "is-live", TRUE, "do-timestamp", TRUE, NULL);

    GstCaps *a_capsfilter = gst_caps_new_simple("audio/x-raw",
        "format", G_TYPE_STRING, "S16LE", "channels", G_TYPE_INT, 2,
        "rate", G_TYPE_INT, 48000, "layout", G_TYPE_STRING, "interleaved", NULL);
    g_object_set(G_OBJECT(a_caps), "caps", a_capsfilter, NULL);
    gst_caps_unref(a_capsfilter);

    g_object_set(G_OBJECT(a_rate), "skip-to-first", TRUE, NULL);
    g_object_set(G_OBJECT(a_split), "output-buffer-samples", 120, NULL);

    g_object_set(G_OBJECT(a_enc),
                "frame-size", 2.5, "bitrate", 128000, NULL);

    // Set output
    g_object_set(G_OBJECT(filesink), "location", output_ts_path.c_str(), NULL);

    if (!csv_output.is_open()) {
        csv_output.open(csv_filename);
        csv_output << "FrameIndex,PTS_90k,Filename,ball,frame_name,innings,isStart,matchID,over,ptp_timestamp,received_at\n";
    }

    if (!csv_output_audio.is_open()) {
        csv_output_audio.open(csv_filename_audio);
        csv_output_audio << "FrameIndex,AudioPTS_90k\n";
    }

    if (!csv_output_summary.is_open()) {
        csv_output_summary.open(csv_filename_summary);
        csv_output_summary << "FrameIndex,PTS_90k,over,ball,innings,matchID\n";
    }

    gst_bin_add_many(GST_BIN(pipeline), appsrc, h265parser, queue1,
                a_src, a_caps, a_queue1, a_convert, a_resample, a_rate,
                a_split, a_enc, a_parse, a_queue3, a_queue2,
                mpegtsmux, filesink, NULL);

    // Link video branch (appsrc -> parser -> queue -> mpegtsmux)
    if (!gst_element_link_many(appsrc, h265parser, queue1, mpegtsmux, NULL)) {
        std::cerr << "Failed to link video elements\n";
        if (context) redisFree(context);
        gst_object_unref(pipeline);
        return -1;
    }

    // Link audio branch
    if (!gst_element_link_many(a_src, a_caps, a_queue1, a_convert, a_resample, a_rate,
                           a_split, a_enc, a_parse, a_queue3, a_queue2, mpegtsmux, NULL)) {
        std::cerr << "[error] Failed to link audio branch (Opus)\n";
        if (context) redisFree(context);
        return -1;
    }

    // Link mux to sink
    if (!gst_element_link(mpegtsmux, filesink)) {
        std::cerr << "[error] Failed to link mux to sink\n";
        if (context) redisFree(context);
        return -1;
    }

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, bus_call, loop);

    // Prepare probe data
    ProbeData pdata;
    pdata.csv = &csv_output;
    pdata.csv_summary = &csv_output_summary;
    pdata.redis = context;

    // Add audio pad probe (existing)
    GstPad *audio_pad = gst_element_get_static_pad(a_parse, "src");
    gst_pad_add_probe(audio_pad, GST_PAD_PROBE_TYPE_BUFFER, audio_probe, &csv_output_audio, NULL);
    gst_object_unref(audio_pad);

    // Add video pad probe - attach to parser src so we see parsed h265 buffers with their PTS
    GstPad *video_pad = gst_element_get_static_pad(h265parser, "src");
    gst_pad_add_probe(video_pad, GST_PAD_PROBE_TYPE_BUFFER, video_probe, &pdata, NULL);
    gst_object_unref(video_pad);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // Start feeder thread (pass redis context so feeder can also read redis if needed)
    std::thread feeder(feed_frames, appsrc, context ? context : nullptr);

    // Run main loop
    g_main_loop_run(loop);

    // Cleanup
    feeder.join();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    if (csv_output.is_open()) csv_output.close();
    if (csv_output_summary.is_open()) csv_output_summary.close();
    if (csv_output_audio.is_open()) csv_output_audio.close();

    if (context) redisFree(context);
    gst_deinit();

    return 0;
}
