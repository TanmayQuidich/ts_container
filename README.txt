BT Machine & WD Machine - Workflow Guide (Improved)

================================= ON BT MACHINE =================================

1. Camera Setup
   - Operator Console (POC: Neeraj)
   - ESDK Emergent (POC: Prashant)

   Commands:
     cd /opt/EVT/eCapture
     sudo ./eCapture

   Steps:
     1. Go to 'Devices'.
     2. Select the required camera.
     3. Click 'Start Acquisition' to begin and 'Stop Acquisition' to end.
     4. Set "Nearest Neighbour" at top-left.

   Recommended Parameters:
     - Frame Mode: Continuous
     - Width: 2560
     - Height: 1440
     - offsetX: 720
     - offsetY: 128
     - Exposure: Up to 2500
     - FPS: 300
     - Gain: 700–1500
     - PGA Gain: 15–30
     - White Balance: Hold
     - Adjust Colour Temperature as required.

2. Ingestion Setup
   Path: /home/quidich/dockerVolume/data/basicBT/david/integration_august/sensor-ingestion/dd-ingest-musician/Utility

   Steps:
     a. Edit config.yaml:
        - Ensure camera parameters match.
        - Confirm camera name and RAMDisk path (folder must exist and preferably be empty).

     b. Navigate to:
        /home/quidich/dockerVolume/data/basicBT/david/watchdog

     c. Edit watchdog.sh → Set number of cameras.

     d. Edit buffer_runner.sh → Set:
        - Folder Path
        - Max Files
        - Check Interval
        (Ensure count matches camera count)

     e. Start Order (Important):
        1) buffer_runner.sh
        2) watchdog.sh

3. NeatBeam File Transfer to Windows Machine
   Use:
     cd /home/quidich/Desktop/DirMirror
     ./build/netbeam_sender /mnt/EyeQ_disk1/ring_buffer_hevc/camera01 192.168.5.23 26100 26200 --conns=64 --workers=128 --stable-ms=50

   Note:
     - IP address corresponds to Windows machine.
     - Ensure camera network uses Mellanox cable (IP typically x.x.5.x)

4. Audio Setup
   Path: /home/quidich/Audio

   Run:
     python3 aes67_http_bridge.py --mcast 239.168.227.217 --port 5004 --iface-ip 192.168.25.50 --http 0.0.0.0 --http-port 53354

   Verify with:
     ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -f s16le -ac 2 -ar 48000 http://192.168.25.50:53354/audio


================================= ON WD MACHINE =================================

1. Start NetBeam Receiver
   Path: D:\Jaydeep\NetBeam

   Steps:
     a. Delete previous images from the target folder.
     b. Run:
        .\build\Release\netbeam_receiver.exe E:\images 26100 26200 128

     - E:\images → Target folder
     - 26100 26200 → Must match sender machine
     - 128 → Workers (Recommended)

2. Audio Receiver Check
   Run:
     ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -f s16le -ac 2 -ar 48000 http://192.168.5.100:53354/audio

3. Ensure DragonflyDB is Running
   POC: Kanishk / Neeraj

4. Run Streaming Pipeline
   Path: D:\Tanmay\gstream (PowerShell)

   Build:
     cmake --build build --config Release

   Run:
     $env:GST_DEBUG=3; .\build\Release\appsrc_feeder.exe 0 300 "E:\images" "E:\camera01_video.ts" "E:\camera01_video.csv" "camera01"

   Parameters:
     - Start-index: 0 → Start from last file
     - FPS: 300
     - Output TS File + CSV Logs + Camera Name

5. Code Component Overview
   - find_first_index_fast → Gets first available frame index
   - make_frame_filename → Generates frame filename
   - is_file_ready → Ensures file availability before processing
   - video_probe → Logs video PTS to CSV
   - audio_probe → Logs audio PTS to CSV
   - feed_frames → Pushes frames into GStreamer pipeline

6. TODO List (Improvements)
   a. Add audio offset handling (High Priority)
   b. Validate I-Frame handling logic (POC: Kanishk & Jaideep)
   c. Allow config-driven parameters instead of CLI args
   d. Create .bat automation script
   e. Implement structured logging
   f. Move remaining hardcoded parameters to config file
   g. Explore parallel pipeline (Not Recommended)


End of Document
