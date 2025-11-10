# BT Machine & WD Machine - Workflow Guide (Improved)

---

## 🟢 On BT Machine

### 1. Camera Setup
**POCs:**  
- Operator Console: Neeraj  
- ESDK Emergent: Prashant  

**Commands:**
```bash
cd /opt/EVT/eCapture
sudo ./eCapture
```

**Steps:**
1. Go to **Devices**
2. Select required camera
3. Click **Start Acquisition** to start & **Stop Acquisition** to stop
4. Set **Nearest Neighbour** at **Top-Left**

**Recommended Parameters:**
| Parameter | Value |
|----------|-------|
| Frame Mode | Continuous |
| Width | 2560 |
| Height | 1440 |
| offsetX | 720 |
| offsetY | 128 |
| Exposure | Up to 2500 |
| FPS | 300 |
| Gain | 700–1500 |
| PGA Gain | 15–30 |
| White Balance | Hold |
| Colour Temp | Adjust as required |

---

### 2. Ingestion Setup
**Config Path:**
```
/home/quidich/dockerVolume/data/basicBT/david/integration_august/sensor-ingestion/dd-ingest-musician/Utility
```

**Steps:**
- Edit `config.yaml` → Check camera parameters + RAMDisk path (folder must exist & ideally empty)

Navigate:
```
/home/quidich/dockerVolume/data/basicBT/david/watchdog
```

Modify:
- `watchdog.sh` → Set camera count
- `buffer_runner.sh` → Set:
  - Folder Path
  - Max Files
  - Check Interval  

**Start Order (Important):**
1. `buffer_runner.sh`
2. `watchdog.sh`

---

### 3. NeatBeam File Transfer to Windows
```bash
cd /home/quidich/Desktop/DirMirror
./build/netbeam_sender /mnt/EyeQ_disk1/ring_buffer_hevc/camera01 192.168.5.23 26100 26200 --conns=64 --workers=128 --stable-ms=50
```

**Note**
- IP = Windows machine IP  
- Use **Mellanox cable** (typical subnet `x.x.5.x`)

---

### 4. Audio Setup
```bash
cd /home/quidich/Audio
python3 aes67_http_bridge.py --mcast 239.168.227.217 --port 5004 --iface-ip 192.168.25.50 --http 0.0.0.0 --http-port 53354
```

**Verify Audio:**
```bash
ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 \
-f s16le -ac 2 -ar 48000 http://192.168.25.50:53354/audio
```

---

## 🔵 On WD Machine

### 1. Start NetBeam Receiver
```
D:\\Jaydeep\\NetBeam
```

**Run:**
```bash
.\build\Release\netbeam_receiver.exe E:\\images 26100 26200 128
```

### 2. Audio Receiver Check
```bash
ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 \
-f s16le -ac 2 -ar 48000 http://192.168.5.100:53354/audio
```

### 3. Ensure DragonflyDB is Running
**POCs:** Kanishk / Neeraj

---

### 4. Run Streaming Pipeline
```
D:\\Tanmay\\gstream
```

**Build:**
```powershell
cmake --build build --config Release
```

**Run:**
```powershell
$env:GST_DEBUG=3; .\build\Release\appsrc_feeder.exe 0 300 "E:\\images" "E:\\camera01_video.ts" "E:\\camera01_video.csv" "camera01"
```

---

### 5. Code Component Overview
| Function | Purpose |
|---------|---------|
| `find_first_index_fast` | Finds first frame index |
| `make_frame_filename` | Generates frame names |
| `is_file_ready` | Ensures frame exists before using it |
| `video_probe` | Logs video PTS to CSV |
| `audio_probe` | Logs audio PTS to CSV |
| `feed_frames` | Pushes frames into GStreamer pipeline |

---

### 6. TODO Improvements
- [ ] Add **audio offset handling** (High Priority)
- [ ] Verify **I-Frame handling** logic (POC: Kanishk / Jaideep)
- [ ] Convert CLI args → Config driven
- [ ] Create `.bat` automation script
- [ ] Implement **structured logging**
- [ ] Move hardcoded values → Config file
- [ ] (Optional) Parallel pipeline research *(not recommended)*

---

**End of Document**
