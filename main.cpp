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

namespace fs = std::filesystem;

static guint64 frame_counter = 0;


// Remove "static const" to make these configurable
static guint64 current_index = 0;
static guint TARGET_FPS = 0;
static double FrameIntervalMs = 0.0;
// static const double FrameIntervalMs = 1000.0 / TARGET_FPS;

static guint64 initial_pts_base = 0;   // NEW
static guint64 pts_increment = 0;      // NEW

static std::string FRAME_FOLDER = "D:\\Tanmay\\deepstream-test1\\Camera\\Camera_1";
static std::ofstream csv_output;

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


void feed_frames(GstElement *appsrc) {


    using clock = std::chrono::steady_clock;
    static auto next_frame_time = clock::now();

    while (true) {
        auto now = clock::now();
        if (now < next_frame_time) {
            std::this_thread::sleep_until(next_frame_time);
        } else {
            // we are behind schedule — consider logging a warning if this happens too often
        }
        
        next_frame_time += std::chrono::microseconds(static_cast<int>(FrameIntervalMs * 1000));

        // === Everything from here remains unchanged ===
        std::string fname = make_frame_filename(current_index);
        fs::path fullpath = fs::path(FRAME_FOLDER) / fname;

        if (!is_file_ready(fullpath)) {
            std::cerr << "[feed] File not found yet: " << fullpath << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // if (frame_counter == 0) {
        //     std::cerr << "[feed] First frame ready. Waiting 8 seconds before processing...\n";
        //     std::this_thread::sleep_for(std::chrono::seconds(8));
        // }

        std::ifstream ifs(fullpath, std::ios::binary | std::ios::ate);
        if (!ifs) {
            std::cerr << "[feed] Failed to open " << fullpath << "\n";
            break;
        }

        std::streamsize size = ifs.tellg();
        ifs.seekg(0, std::ios::beg);
        std::vector<uint8_t> bufferdata(size);
        if (!ifs.read(reinterpret_cast<char*>(bufferdata.data()), size)) {
            std::cerr << "[feed] Failed reading " << fullpath << "\n";
            break;
        }
        ifs.close();

        GstBuffer *buffer = gst_buffer_new_allocate(NULL, size, NULL);
        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            std::cerr << "[feed] Buffer map failed\n";
            gst_buffer_unref(buffer);
            break;
        }
        memcpy(map.data, bufferdata.data(), size);
        gst_buffer_unmap(buffer, &map);

        GstClockTime pts = gst_util_uint64_scale(frame_counter, GST_SECOND, TARGET_FPS);
        GST_BUFFER_PTS(buffer) = pts;
        GST_BUFFER_DTS(buffer) = pts;
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, TARGET_FPS);

        GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
        if (ret != GST_FLOW_OK) {
            std::cerr << "[feed] appsrc_push_buffer returned " << ret << "\n";
            gst_buffer_unref(buffer);
            break;
        }

        std::cerr << "[feed] pushed frame " << frame_counter << " (" << fname << ")\n";


        static guint64 custom_pts = 324000000;

        // Patterned increments
        static const std::vector<guint64> increments = {299, 300, 301};
        static size_t increment_index = 0;

        if (csv_output.is_open()) {
            csv_output << frame_counter << "," << custom_pts << "," << fname << "\n";
        }

        // Update to next PTS in pattern
        custom_pts += increments[increment_index];
        increment_index = (increment_index + 1) % increments.size();  // cycle through 0→1→2→0→...


        frame_counter++;
        current_index++;

        static auto last_log = clock::now();
        if (frame_counter % TARGET_FPS == 0) {
            auto now = clock::now();
            auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count();
            std::cerr << "[stats] Last "<<TARGET_FPS<<" frames in " << delta << "ms (FPS: " << (TARGET_FPS * 1000.0 / delta) << ")\n";
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

    // Parse arguments
    current_index = std::stoull(argv[1]);    // e.g. 2379000
    TARGET_FPS = static_cast<guint>(std::stoi(argv[2]));  // e.g. 300
    FrameIntervalMs = 1000.0 / TARGET_FPS;

    // Calculate PTS values for MPEG-TS (90kHz clock)
    initial_pts_base = current_index * 100;
    pts_increment = 90000 / TARGET_FPS;


    FRAME_FOLDER = argv[3];                   // e.g. D:\path\to\Camera_1
    std::string output_ts_path = argv[4];     // e.g. E:\output.ts
    std::string csv_filename = argv[5];       // e.g. output.csv
    std::string camera_id = argv[6];          // e.g. camera02

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
        csv_output << "FrameIndex,PTS,Filename\n";
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

    std::thread feeder(feed_frames, appsrc);

    g_main_loop_run(loop);

    feeder.join();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    if (csv_output.is_open()) csv_output.close();

    gst_deinit();

    return 0;
}



