@echo off

start "" .\build\Release\appsrc_feeder.exe 300 "R:\camera01" "E:\ts_container\output_300fps_01.ts" frame_pts_300fps_01.csv camera01
start "" .\build\Release\appsrc_feeder.exe 300 "R:\camera02" "E:\ts_container\output_300fps_02.ts" frame_pts_300fps_02.csv camera02
start "" .\build\Release\appsrc_feeder.exe 300 "R:\camera03" "E:\ts_container\output_300fps_03.ts" frame_pts_300fps_03.csv camera03

echo Launched all 3 programs.
pause
