# SG200x_GC2083_Yolov8_11
Single cpp file for deploying yolov8/yolo11 algorithm to sg200x platform. Tested on Milk-v Duo-s(SG2000) w/ GC2083 camera and v2 firmware.

## Build
- Clone the cvitek SDK
  `git clone https://github.com/milkv-duo/cvitek-tdl-sdk-sg200x`
- Clone this repo
  `git clone https://github.com/Basicname/SG200x_GC2083_Yolov8_11`
- Set up environments. `build_env.sh` is my script for setting up env. Just edit <SDK_PATH> before sourcing it.
  `source build_env.sh`
- `make` and you will get `sample_yolov8` executable file.

## Feature
- Support VPSS Crop
- Support YoloV8 & Yolo11
- A more advance version, which supports RTSP streaming and fisheye len distortion is available, but its source won't be open to public.

## TODO
- Some other versions, using e.g. UVC camera or H.264/MJPEG video file as input will be uploaded soon.

## Performance
The input size of model is 640x640. Sensor size is 1920x1080. Model is quantized using cvitek's TPU toolchain.
| Crop Size | FPS |
| --------- | --- |
| 1920x1080 | ~21 |
|  640x640  | ~25 |

For model conversion, please visit [Chinese webpage](https://milkv.io/zh/docs/duo/application-development/tdl-sdk/tdl-sdk-yolo11) or [English webpage](https://milkv.io/docs/duo/application-development/tdl-sdk/tdl-sdk-yolo11)
Also, don't forget to edit number of classes in sample_yolov8.cpp, otherwise you'll get an error when opening model.
