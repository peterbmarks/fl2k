# fl2k
Use cheap VGA dongles as a digital to analog converter for ham radio

Based on great work from https://osmocom.org/projects/osmo-fl2k/wiki

This version is from https://github.com/peterbmarks/fl2k
Blog post: http://blog.marxy.org/2018/04/first-play-with-osmo-fl2k-compatible.html

# Objective

I'm stripping back the supplied FM example and statically linking
the library code to make it easy to play with.

Longer term I'd like to be able to generate a decent HF signal that
can be modulated for things like WSPR.

# Build

Just type:
make

# USB memory
On ubunut you probably need increase the USB memory buffer by running:

sudo sh -c 'echo 1000 > /sys/module/usbcore/parameters/usbfs_memory_mb'

See https://importgeek.wordpress.com/2017/02/26/increase-usbfs-memory-limit-in-ubuntu/
for more info and how to add this to the grub command line so it's permanent.

# Run
Run locally.

./vgaplay  -s 130e6 -c 7e6 

-s is the sample rate of the software DDS. 
The higher the sample rate the better the sine wave output.
You can probably go up to about 150MS/s

-c is the carrier frequency to generate.
The closer the carrier frequency gets to half the sample rate,
the more the signal becomes a square wave.

Here's how the waveform looks at 7.159MHz with 150Ms/s
![Beautiful CRO capture](https://raw.githubusercontent.com/peterbmarks/fl2k/master/defaultwaveform.png)
