cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DGST_ROOT="C:/Program Files/gstreamer/1.0/msvc_x86_64"

 cmake --build build --config Release

 .\build\Release\audio-lockstep.exe 