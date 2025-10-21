// av_sync.cpp  (drop-in fixed)
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <glib.h>
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <limits>
#include <cstring>

namespace fs = std::filesystem;

/* ----------------------------- Utilities ----------------------------- */

guint64 find_first_index_fast(const std::string& folder) {
    auto it = fs::directory_iterator(folder);
    if (it != fs::end(it) && it->is_regular_file()) {
        std::string fname = it->path().filename().string();
        std::string prefix = "frame_camera05_";
        std::string suffix = ".hevc";
        if (fname.rfind(prefix, 0) == 0 && fname.size() > prefix.size() + suffix.size()) {
            std::string number_str = fname.substr(
                prefix.size(),
                fname.size() - prefix.size() - suffix.size()
            );
            try { return std::stoull(number_str); }
            catch (...) { throw std::runtime_error("[error] First file found is invalid: " + fname); }
        } else {
            throw std::runtime_error("[error] First file does not match pattern: " + fname);
        }
    }
    throw std::runtime_error("[error] No files found in folder: " + folder);
}

static guint64 frame_counter = 0;
static guint64 current_index = 0;
static guint  TARGET_FPS = 0;
static double FrameIntervalMs = 0.0;

static std::string FRAME_FOLDER = "/app/camera05/";
static std::ofstream csv_output;

std::string make_frame_filename(guint64 idx) {
    char buf[256];
    snprintf(buf, sizeof(buf), "frame_camera05_%09" G_GUINT64_FORMAT ".hevc", idx);
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

/* -------------------------- Video feeder thread -------------------------- */

void feed_frames(GstElement *appsrc) {
    using clock = std::chrono::steady_clock;
    static auto next_frame_time = clock::now();

    // simple rolling logger timer
    auto last_log = clock::now();

    while (true) {
        auto now = clock::now();
        if (now < next_frame_time) std::this_thread::sleep_until(next_frame_time);
        next_frame_time += std::chrono::microseconds(static_cast<int>(FrameIntervalMs * 1000));

        const std::string fname = make_frame_filename(current_index);
        fs::path fullpath = fs::path(FRAME_FOLDER) / fname;

        if (!is_file_ready(fullpath)) {
            std::cerr << "[feed] File not ready: " << fullpath << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        std::ifstream ifs(fullpath, std::ios::binary | std::ios::ate);
        if (!ifs) { std::cerr << "[feed] Open failed " << fullpath << "\n"; break; }

        std::streamsize size = ifs.tellg();
        ifs.seekg(0, std::ios::beg);
        std::vector<uint8_t> bufferdata(size);
        if (!ifs.read(reinterpret_cast<char*>(bufferdata.data()), size)) {
            std::cerr << "[feed] Read failed " << fullpath << "\n"; break;
        }
        ifs.close();

        GstBuffer *buffer = gst_buffer_new_allocate(NULL, size, NULL);
        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            std::cerr << "[feed] Buffer map failed\n";
            gst_buffer_unref(buffer);
            break;
        }
        std::memcpy(map.data, bufferdata.data(), size);
        gst_buffer_unmap(buffer, &map);

        GstClockTime pts = gst_util_uint64_scale(frame_counter, GST_SECOND, TARGET_FPS);
        GST_BUFFER_PTS(buffer) = pts;
        GST_BUFFER_DTS(buffer) = pts;
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, TARGET_FPS);

        GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
        if (ret != GST_FLOW_OK) {
            std::cerr << "[feed] appsrc_push_buffer ret=" << ret << "\n";
            gst_buffer_unref(buffer);
            break;
        }

        if (csv_output.is_open()) csv_output << frame_counter << "," << pts << "," << fname << "\n";

        if ((frame_counter % TARGET_FPS) == 0 && frame_counter > 0) {
            auto now2 = clock::now();
            auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now2 - last_log).count();
            std::cerr << "[stats] Last " << TARGET_FPS << " frames in " << delta
                      << " ms (FPS: " << (TARGET_FPS * 1000.0 / std::max<long long>(1, delta)) << ")\n";
            last_log = now2;
        }

        frame_counter++;
        current_index++;
    }
}

/* ------------------------------ GStreamer bus ------------------------------ */

static gboolean bus_call(GstBus *, GstMessage *msg, gpointer data) {
    GMainLoop *loop = (GMainLoop *)data;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = nullptr; gchar *dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            g_printerr("ERROR from %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
            if (dbg) g_printerr("Debug: %s\n", dbg);
            if (err) g_error_free(err);
            if (dbg) g_free(dbg);
            g_main_loop_quit(loop);
            break;
        }
        case GST_MESSAGE_EOS:
            g_print("EOS received\n");
            g_main_loop_quit(loop);
            break;
        default: break;
    }
    return TRUE;
}

/* ------------------------- Safe property helper ------------------------- */

static void safe_set(GstElement *elem, const char *prop, GValue *val) {
    if (!elem) return;
    GObjectClass *klass = G_OBJECT_GET_CLASS(elem);
    if (!g_object_class_find_property(klass, prop)) {
        g_printerr("[warn] element %s lacks property '%s' — skipping\n",
                   GST_ELEMENT_NAME(elem), prop);
        return;
    }
    g_object_set_property(G_OBJECT(elem), prop, val);
}

template<typename T>
static void safe_set_simple(GstElement *elem, const char *prop, T value) {
    GValue v = G_VALUE_INIT;
    if constexpr (std::is_same_v<T, gint> || std::is_same_v<T, int>) { g_value_init(&v, G_TYPE_INT); g_value_set_int(&v, (gint)value); }
    else if constexpr (std::is_same_v<T, guint>) { g_value_init(&v, G_TYPE_UINT); g_value_set_uint(&v, (guint)value); }
    else if constexpr (std::is_same_v<T, gboolean>){ g_value_init(&v, G_TYPE_BOOLEAN); g_value_set_boolean(&v, value); }
    else if constexpr (std::is_same_v<T, gdouble> || std::is_same_v<T, double>) { g_value_init(&v, G_TYPE_DOUBLE); g_value_set_double(&v, value); }
    else if constexpr (std::is_same_v<T, const char*> || std::is_same_v<T, char*>) { g_value_init(&v, G_TYPE_STRING); g_value_set_string(&v, value); }
    else if constexpr (std::is_same_v<T, std::string>) { g_value_init(&v, G_TYPE_STRING); g_value_set_string(&v, value.c_str()); }
    safe_set(elem, prop, &v);
    g_value_unset(&v);
}

/* ---------------------------------- main ---------------------------------- */

int main(int argc, char *argv[]) {
    TARGET_FPS = (argc >= 2) ? static_cast<guint>(std::stoi(argv[1])) : 300;
    if (TARGET_FPS == 0) { std::cerr << "[error] TARGET_FPS cannot be 0, setting 300\n"; TARGET_FPS = 300; }
    FrameIntervalMs = 1000.0 / TARGET_FPS;

    try {
        current_index = find_first_index_fast(FRAME_FOLDER);
        std::cout << "[config] Starting from first available index: " << current_index << "\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n"; return 1;
    }

    gst_init(&argc, &argv);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    /* ---------------------- Elements: video branch ---------------------- */
    GstElement *pipeline   = gst_pipeline_new("av-ts-pipeline");
    GstElement *appsrc     = gst_element_factory_make("appsrc",     "v-appsrc");
    GstElement *h265parser = gst_element_factory_make("h265parse",  "v-parse");
    GstElement *vqueue     = gst_element_factory_make("queue",      "v-queue");
    GstElement *mpegtsmux  = gst_element_factory_make("mpegtsmux",  "ts-mux");
    GstElement *filesink   = gst_element_factory_make("filesink",   "ts-out");

    if (!pipeline || !appsrc || !h265parser || !vqueue || !mpegtsmux || !filesink) {
        std::cerr << "Element creation failed (video)\n"; return -1;
    }

    g_object_set(G_OBJECT(appsrc),
                 "format", GST_FORMAT_TIME,
                 "is-live", TRUE,
                 "stream-type", GST_APP_STREAM_TYPE_STREAM,
                 NULL);

    GstCaps *v_caps = gst_caps_new_simple(
        "video/x-h265",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment",     G_TYPE_STRING, "au",
        NULL);
    g_object_set(G_OBJECT(appsrc), "caps", v_caps, NULL);
    gst_caps_unref(v_caps);

    g_object_set(G_OBJECT(filesink), "location", "/app/data/audio_output_300fps.ts", NULL);

    if (!csv_output.is_open()) {
        csv_output.open("/app/data/audio_frame_pts_300fps.csv");
        csv_output << "FrameIndex,PTS,Filename\n";
    }

    // Try to set muxer properties only if they exist on this build
    safe_set_simple(mpegtsmux, "pat-interval", 100);   // ms (if present)
    safe_set_simple(mpegtsmux, "pcr-interval", 40);    // ms
    safe_set_simple(mpegtsmux, "program-number", 1);   // may not exist → safely skipped
    safe_set_simple(mpegtsmux, "pcr-pid",  0x100);
    safe_set_simple(mpegtsmux, "video-pid",0x101);
    safe_set_simple(mpegtsmux, "audio-pid",0x102);

    /* ---------------------- Elements: audio (AES67) ---------------------- */
    GstElement *aud_src   = gst_element_factory_make("udpsrc",            "a-rtp-src");
    GstElement *rtpjbuf   = gst_element_factory_make("rtpjitterbuffer",   "a-jbuf");
    GstElement *depay     = gst_element_factory_make("rtpL24depay",       "a-depay");
    GstElement *aconv     = gst_element_factory_make("audioconvert",      "a-conv");
    GstElement *ares      = gst_element_factory_make("audioresample",     "a-res");
    // Encoder will be selected below
    GstElement *aacenc    = gst_element_factory_make("avenc_aac",         "a-enc-aac");   // may be NULL
    GstElement *aacparse  = gst_element_factory_make("aacparse",          "a-parse");     // may be used only with AAC
    GstElement *mp2enc    = nullptr;                                                     // fallback
    GstElement *aqueue    = gst_element_factory_make("queue",             "a-queue");

    if (!aud_src || !rtpjbuf || !depay || !aconv || !ares || !aqueue) {
        std::cerr << "Audio element creation failed (pre-encoder)\n"; return -1;
    }

    // Bind to multicast + caps (from SDP)
    g_object_set(G_OBJECT(aud_src),
                 "address", "0.0.0.0",
                 "port",    5004,
                 "auto-multicast", TRUE,
                 "multicast-group", "239.168.227.217",
                 // "multicast-iface", "eno1", // uncomment if you must force NIC
                 NULL);

    GstCaps *rtp_caps = gst_caps_from_string(
        "application/x-rtp, "
        "media=(string)audio, "
        "clock-rate=(int)48000, "
        "encoding-name=(string)L24, "
        "channels=(int)2, "
        "payload=(int)97, "
        "ptime=(string)1"
    );
    g_object_set(G_OBJECT(aud_src), "caps", rtp_caps, NULL);
    gst_caps_unref(rtp_caps);

    g_object_set(G_OBJECT(rtpjbuf),
                 "latency", 100,   // tune 50–200ms if needed
                 "mode", 0,        // time-based
                 NULL);

    /* ---------------------------- Build pipeline ---------------------------- */

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, bus_call, loop);
    gst_object_unref(bus);

    gst_bin_add_many(GST_BIN(pipeline),
                     // video path
                     appsrc, h265parser, vqueue,
                     // audio pre-enc
                     aud_src, rtpjbuf, depay, aconv, ares,
                     // mux + sink
                     mpegtsmux, filesink,
                     NULL);

    // Link video -> mux
    if (!gst_element_link_many(appsrc, h265parser, vqueue, mpegtsmux, NULL)) {
        std::cerr << "Failed to link video branch\n";
        gst_object_unref(pipeline); return -1;
    }

    // Link audio up to ares
    if (!gst_element_link_many(aud_src, rtpjbuf, depay, aconv, ares, NULL)) {
        std::cerr << "Failed to link audio pre-encoder\n";
        gst_object_unref(pipeline); return -1;
    }

    /* -------- Try AAC first; if unavailable, fall back to MP2 -------- */
    bool using_aac = false;

    if (aacenc) {
        gst_bin_add_many(GST_BIN(pipeline), aacenc, NULL);
        // Configure encoder if property exists
        safe_set_simple(aacenc, "bit_rate", 192000);  // may be "bitrate" on some encoders; safe_set won’t crash
        if (aacparse) gst_bin_add(GST_BIN(pipeline), aacparse);

        // Link ares -> aacenc [-> aacparse] -> queue -> mux
        if (aacparse) {
            gst_bin_add(GST_BIN(pipeline), aqueue);
            if (!gst_element_link_many(ares, aacenc, aacparse, aqueue, NULL)) {
                std::cerr << "[warn] AAC link failed; trying MP2 fallback\n";
            } else if (!gst_element_link(aqueue, mpegtsmux)) {
                std::cerr << "[warn] AAC queue->mux failed; trying MP2 fallback\n";
            } else {
                using_aac = true;
            }
        } else {
            // Some builds require ADTS; if aacparse missing, attempt direct
            gst_bin_add(GST_BIN(pipeline), aqueue);
            if (!gst_element_link_many(ares, aacenc, aqueue, NULL) ||
                !gst_element_link(aqueue, mpegtsmux)) {
                std::cerr << "[warn] AAC (no parser) link failed; trying MP2 fallback\n";
            } else {
                using_aac = true;
            }
        }
    }

    if (!using_aac) {
        // Remove any half-added AAC bits from bin (safe if null)
        if (aacparse) gst_bin_remove(GST_BIN(pipeline), aacparse);
        if (aacenc)   gst_bin_remove(GST_BIN(pipeline), aacenc);

        // MP2 fallback (needs gstreamer1.0-plugins-ugly)
        mp2enc = gst_element_factory_make("twolamemp2enc", "a-enc-mp2");
        if (!mp2enc) { std::cerr << "No AAC and no MP2 encoder available.\n"; gst_object_unref(pipeline); return -1; }
        gst_bin_add_many(GST_BIN(pipeline), mp2enc, aqueue, NULL);

        // Common bitrate property name is "bitrate" (bps)
        safe_set_simple(mp2enc, "bitrate", 192000);

        if (!gst_element_link_many(ares, mp2enc, aqueue, NULL)) {
            std::cerr << "Failed to link MP2 encoder branch\n"; gst_object_unref(pipeline); return -1;
        }
        if (!gst_element_link(aqueue, mpegtsmux)) {
            std::cerr << "Failed to link audio queue to mux (MP2)\n"; gst_object_unref(pipeline); return -1;
        }
        std::cerr << "[info] Using MP2 audio (AAC unavailable)\n";
    } else {
        std::cerr << "[info] Using AAC audio\n";
    }

    // mux -> filesink
    if (!gst_element_link(mpegtsmux, filesink)) {
        std::cerr << "Failed to link mux to filesink\n"; gst_object_unref(pipeline); return -1;
    }

    // Use system clock explicitly
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), gst_system_clock_obtain());

    // Start pipeline
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // Start feeder thread for video
    std::thread feeder(feed_frames, appsrc);

    // Run main loop
    g_main_loop_run(loop);

    // Cleanup
    feeder.join();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    if (csv_output.is_open()) csv_output.close();
    gst_deinit();
    return 0;
}










//=================================================//
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
#include <hiredis/hiredis.h>

namespace fs = std::filesystem;
static guint64 frame_counter = 0;

// Remove "static const" to make these configurable
static guint64 current_index = 0;
static guint TARGET_FPS = 0;
static double FrameIntervalMs = 0.0;

static guint64 initial_pts_base = 0;   
static guint64 pts_increment = 0;     

static std::string FRAME_FOLDER ;
static std::ofstream csv_output;
static std::ofstream csv_output_summary;



std::string camera_prefix;

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

void feed_frames(GstElement *appsrc, redisContext* context){
    using clock = std::chrono::steady_clock;
    auto start_time = clock::now();
    auto frame_duration = std::chrono::microseconds(static_cast<int>(FrameIntervalMs * 1000));
    static guint64 custom_pts = 324000000;
    static const std::vector<guint64> increments =
        (TARGET_FPS == 150) ? std::vector<guint64>{599, 600, 601}
                            : std::vector<guint64>{299, 300, 301};

    int over_counter=1;
    int ball_counter_over=0;
    int ball_counter =1000;
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

        // Construct the frame filename
        std::string fname = make_frame_filename(current_index);
        fs::path fullpath = fs::path(FRAME_FOLDER) / fname;

        // Check if file is ready
        if (!is_file_ready(fullpath)) {
            std::cerr << "[feed] File not found or not ready: " << fullpath << ". Waiting...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Short wait before retry
            continue; // Retry the same frame
        }

        // Read the file
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

        // Set buffer timestamps
        GstClockTime pts = gst_util_uint64_scale(frame_counter, GST_SECOND, TARGET_FPS);
        GST_BUFFER_PTS(buffer) = pts;
        GST_BUFFER_DTS(buffer) = pts;
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, TARGET_FPS);

        // Push buffer to appsrc
        GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
        if (ret != GST_FLOW_OK) {
            std::cerr << "[feed] appsrc_push_buffer returned " << ret << "\n";
            gst_buffer_unref(buffer);
            break; // Exit on critical error
        }

        std::cerr << "[feed] Pushed frame " << frame_counter << " (" << fname << ")\n";
        static size_t increment_index = 0;

        // std::cerr << " Ball number:\n";

        if (csv_output.is_open()) {
            std::string redis_key = fname.substr(0, fname.find_last_of('.'));
            redisReply* reply = (redisReply*)redisCommand(context, "GET %s", redis_key.c_str());

            std::string ball = "1", frame_name = "NA", innings = "1", isStart = "false", matchID = "123", over = "1", ptp_timestamp = "NA", received_at = "NA";
            


            if (frame_counter%750==0){
                    ball_counter_over=ball_counter_over+1;
                    if(ball_counter_over==7)
                    {
                        ball_counter_over=1;
                        over_counter=over_counter+1;

                    }
            }
            ball = std::to_string(ball_counter_over);
            innings       = "1";
            if (frame_counter % 750 <= 300)
                isStart = "true";
            else
                isStart = "false";
            matchID       = "1234";
            over = std::to_string(over_counter); 


            if (reply && reply->type == REDIS_REPLY_STRING) {
                std::string json = reply->str;

                auto extract = [&](const std::string& key) -> std::string {
                    size_t pos = json.find("\"" + key + "\":");
                    if (pos == std::string::npos) return "NA";
                    pos += key.size() + 3; // move past "key":
                    // detect value type
                    if (json[pos] == '"') {
                        size_t end = json.find('"', pos + 1);
                        return json.substr(pos + 1, end - pos - 1);
                    } else {
                        size_t end = json.find_first_of(",}", pos);
                        return json.substr(pos, end - pos);
                    }
                };
                // if (frame_counter%750==0){
                //     ball_counter_over=ball_counter_over+1;
                //     if(ball_counter_over==7)
                //     {
                //         ball_counter_over=1;
                //         over_counter=over_counter+1;

                //     }
                // }

                // ball          = extract("ball");
                // innings       = extract("innings");
                // isStart       = extract("isStart");
                // matchID       = extract("matchID");
                // over          = extract("over");

                // ball = std::to_string(ball_counter_over);
                // innings       = "1";
                // if (frame_counter % 750 <= 300)
                //     isStart = "true";
                // else
                //     isStart = "false";
                // matchID       = "1234";
                // over = std::to_string(over_counter); 

                frame_name    = extract("frame_name");
                ptp_timestamp = extract("ptp_timestamp");
                received_at   = extract("received_at");
            }
            csv_output << frame_counter << "," << custom_pts << "," << fname << ","
                    << ball << "," << frame_name << "," << innings << "," << isStart << ","
                    << matchID << "," << over << "," << ptp_timestamp << "," << received_at << "\n";

            if(ball!=prev_ball || over!=prev_over || innings!=prev_innings){

                csv_output_summary << frame_counter << "," 
                            << custom_pts << "," 
                            << over << "," 
                            << ball << "," 
                            << innings << "," 
                            << matchID << "\n";
            }

            prev_ball=ball;
            prev_over=over;
            prev_innings=innings;
            


            if (reply) freeReplyObject(reply);
        }
        // Update custom PTS
        custom_pts += increments[increment_index];
        increment_index = (increment_index + 1) % increments.size();

        // Increment counters
        frame_counter++;
        current_index++;

        // Log FPS statistics
        static auto last_log = clock::now();
        if (frame_counter % TARGET_FPS == 0) {
            auto now = clock::now();
            auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count();
            std::cerr << "[stats] Last " << TARGET_FPS << " frames in " << delta << " ms (FPS: " << (TARGET_FPS * 1000.0 / delta) << ")\n";
            last_log = now;
        }
    }
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
        return 1;
    }
    std::cout << "[redis] Connected successfully to DragonflyDB\n";

    // Parse arguments
    current_index = std::stoull(argv[1]);    // e.g. 2379000
    TARGET_FPS = static_cast<guint>(std::stoi(argv[2]));  // e.g. 300
    FrameIntervalMs = 1000.0 / TARGET_FPS;

    // Calculate PTS values for MPEG-TS (90kHz clock)
    initial_pts_base = current_index * 100;
    pts_increment = 90000 / TARGET_FPS;


    FRAME_FOLDER = argv[3];                   // e.g. D:\path\to\Camera_1
    std::string output_ts_path = argv[4];     // e.g. E:\output.ts
    std::string csv_filename = argv[5];       // e.g. output_full.csv
    std::string camera_id = argv[6];          // e.g. camera02

    // Second CSV file (summary)
    std::string csv_filename_summary = "summary_" + camera_id + ".csv";


    camera_prefix = camera_id;


    std::cout << "[config] Starting from index: " << current_index << "\n";
    std::cout << "[config] Target FPS: " << TARGET_FPS << "\n";
    std::cout << "[config] Frame Interval (ms): " << FrameIntervalMs << "\n";

    gst_init(&argc, &argv);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    GstElement *pipeline = gst_pipeline_new("appsrc-pipeline");
    GstElement *appsrc    = gst_element_factory_make("appsrc", "my-appsrc");
    GstElement *h265parser = gst_element_factory_make("h265parse", "parser");
    GstElement *queue1 = gst_element_factory_make("queue", "queue1");  // add queue
    GstElement *mpegtsmux = gst_element_factory_make("mpegtsmux", "ts-muxer");
    GstElement *filesink = gst_element_factory_make("filesink", "ts-output");

    if (!pipeline || !appsrc || !h265parser || !queue1 || !mpegtsmux || !filesink) {
        std::cerr << "Element creation failed\n";
        return -1;
    }

    // Configure appsrc
    g_object_set(G_OBJECT(appsrc),
                 "format", GST_FORMAT_TIME,
                 "is-live", TRUE,
                 "stream-type", GST_APP_STREAM_TYPE_STREAM,  // might help
                 NULL);

    GstCaps * caps = gst_caps_new_simple(
        "video/x-h265",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "au",
        NULL);
    g_object_set(G_OBJECT(appsrc), "caps", caps, NULL);
    gst_caps_unref(caps);

    // Set output
    g_object_set(G_OBJECT(filesink), "location", output_ts_path.c_str(), NULL);

    if (!csv_output.is_open()) {
        csv_output.open(csv_filename);
        csv_output << "FrameIndex,PTS_90k,Filename,ball, frame_name, innings, isStart, matchID, over, ptp_timestamp, received_at\n";
    }
    if (!csv_output_summary.is_open()) {
    csv_output_summary.open(csv_filename_summary);
    csv_output_summary << "FrameIndex,PTS_90k,over,ball,innings,matchID\n";
    }


    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, bus_call, loop);

    gst_bin_add_many(GST_BIN(pipeline), appsrc, h265parser, queue1, mpegtsmux, filesink, NULL);

    // Link with queue
    if (!gst_element_link_many(appsrc, h265parser, queue1, mpegtsmux, filesink, NULL)) {
        std::cerr << "Failed to link elements\n";
        gst_object_unref(pipeline);
        return -1;
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    std::thread feeder(feed_frames, appsrc, context);

    g_main_loop_run(loop);

    feeder.join();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    if (csv_output.is_open()) csv_output.close();
    if (csv_output_summary.is_open()) csv_output_summary.close();

    redisFree(context);
    gst_deinit();

    return 0;
}
