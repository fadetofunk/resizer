# resizer
A small C++ program that uses ffmpeg source code to take a video and resize it with a few useful options

1. Drop the video file you want to re-size.
2. Specify the size in Megabytes
3. Optionally trim the video
4. Choose the resolution you want to process the file at. (the longer the video the smaller you probably want the resolution.)
5. Press "Start Processing"
6. The processed file will show up in the location of the original video with the suffix "RESIZED" which is also optional to change in the program. If a resized video already exists, it will append a 1 and so on.
