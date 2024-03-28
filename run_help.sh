#bin/bash

gst-launch-1.0 udpsrc port=5600 caps='application/x-rtp, payload=(int)96, clock-rate=(int)90000, media=(string)video, encoding-name=(string)H264' ! \
rtph264depay ! h264parse config-interval=1 ! video/x-h264,stream-format=byte-stream,alignment=au ! \
fdsink fd=1 | ./build2/test/openhd_vid_test