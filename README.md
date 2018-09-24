# Raspberry PI MMAL Examples

This repository contains a bunch of examples for the MMAL (Multimedia Abstraction Layer) API.
MMAL is a C library designed by Broadcom for use with the Videocore IV GPU found on the Raspberry Pi.

Everything written in this document is a personal opinion from myself (t-moe). Feel free to contribute to the respository via Pull-Requests, but please don't send me general MMAL questions, as I am not an expert on the Topic.


The goal of this repository is to help others getting started with MMAL. MMAL can be a real pain, because it's poorly documented and the relevant GPU Source-Code is closed source.


## Official resources


* [MMAL Doxygen](http://www.jvcref.com/files/PI/documentation/html/index.html) (Warning: Out of date, see below)
* [Raspberry PI Community Forum](https://www.raspberrypi.org/forums/viewforum.php?f=67)
* [Official MMAL examples](https://github.com/raspberrypi/userland/tree/master/interface/mmal/test/examples) (Warning: Partially broken, see below)
* [MMAL Source](https://github.com/raspberrypi/userland/tree/master/interface/mmal) (client only)
* [OpenMAX Il Components Documentation](http://www.jvcref.com/files/PI/documentation/ilcomponents/index.html) (Openmax is used in the backend of MMAL)



## Examples in this repository

File | Description | Known Working rpi-firmware versions
------------ | ------------- | ---------------------
example_basic_2.c | Copied from the official userland repo. Takes a video-filename as argument and decodes that video | [a5b781c](https://github.com/Hexxeh/rpi-firmware/commit/a5b781c7a761664226ff9654416776d372f8bbf0)
graph_decode_render.c | Decodes test.h264_2 and renders it to the gpu output. Uses the graph api | [a5b781c](https://github.com/Hexxeh/rpi-firmware/commit/a5b781c7a761664226ff9654416776d372f8bbf0)
connection_decode_encode.c | Decodes test.h264_t and re-encodes it again. Uses the connection api | [a5b781c](https://github.com/Hexxeh/rpi-firmware/commit/a5b781c7a761664226ff9654416776d372f8bbf0)


Just type make to build them to individual programms.


## Debugging Notes

* `export VC_LOGLEVEL="mmal:trace"` will enable log output of the MMAL client library (CPU)
* `sudo vcdbg log msg` will dump the server-side messages (GPU)
* `vcgencmd version` will print the GPU firmware version.
* Ensure that you have enough GPU Memory allocated (check `/boot/config.txt`) and the required license keys, for what you're trying to achieve.
* The people over at the [Raspberry PI Community Forum](https://www.raspberrypi.org/forums/viewforum.php?f=67) are really helpful. Provide them a [Minimal, Complete and Verifiable Example ](https://stackoverflow.com/help/mcve) along with the log outputs from the commands listed above and they may be able to help you.

## Problem Notes
* The Doxygen documentation of the MMAL library is out of date.
  * The container reader and writer components do not work. They were initially designed for a project where the GPU had access to the filesystem. This is no longer the case. So you have to read/write media containers on the CPU and send the data via buffers to the GPU
  * The MMAL Library provides a generic way to access "components". The general concept is documented in doxygen but the individual components are not documented. You have to guess the inputs, outputs & parameters for every component by looking at examples.
* The Offical MMAL examples are partially broken.
  * example_connections.c and example_graph.c use the reader component, which will not work
  * example_basic_2.c has a small typo. A fixed version is in this repository.
  * I didn't look at example_basic_1.c
  * All examples lack Makefiles and the input data to test it with.
* Whether your MMAL Program will work depends a lot on the GPU firmware. What may have worked once, will not nececessarily work now.
  * Run `sudo rpi-update` to update to the latest firmware.
  * Run `sudo rpi-update <version>` to install a specific version.
  * Always include the used firmware version in your problem description.
* The video decoder component expects a h264 raw stream without audio as input.
  * The file `test.h264_2` in this repository is a suitable example.
  * You can convert to the requested format using ffmpeg. `ffmpeg -i input_720p.mp4 -bsf h264_mp4toannexb -an -f h264 output.h262`
* The video encoder component expects a specific input format:
  * encoding: I420
  * the width must be a multiple of 32 and the height a multiple of 16. Use the `VCOS_ALIGN_UP` Macros to calculate the aligned lengths and provide the effective size via the "crop" parameter.
* The video encoder component opens the codec when you enable the output port. At this point the input format must be commited. Changing it afterwards will result in an error.
* If you use the Graph API, you cannot use "Zero Copy" on inputs and outputs.
  * This is probably because mmal_graph is using mmal_pool_create, whilst to get zero_copy working correctly requires the use of mmal_port_pool_create.
  * Connect your components manually or do not use "Zero copy"

