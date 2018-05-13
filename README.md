# fl2k
Use cheap VGA dongles as a digital to analog converter for ham radio

Based on great work from [Steve Markgraf](https://osmocom.org/projects/osmo-fl2k/wiki)

This version is on [Github](https://github.com/peterbmarks/fl2k)
I have a [Blog post](http://blog.marxy.org/2018/04/first-play-with-osmo-fl2k-compatible.html) about my experiments so far

# Objective

I'm stripping back the supplied FM example and statically linking
the library code to make it easy to play with.

Longer term I'd like to be able to generate a decent HF signal that
can be modulated for things like WSPR.

# Build

On Ubuntu you might need:
```
sudo apt install git build-essential libusb-1.0-0-dev
```

To build just type:
```
make
```

You'll need to install the software this is derived from or you'll get this
error:

```
libusb: error [_get_usbfs_fd] libusb couldn't open USB device /dev/bus/usb/002/014: Permission denied
libusb: error [_get_usbfs_fd] libusb requires write access to USB device nodes.
usb_open error -3
Please fix the device permissions, e.g. by installing the udev rules file
```

Build and install the software as documented [here](https://osmocom.org/projects/osmo-fl2k/wiki)

# USB memory

You'll get this error:
```
libusb: error [op_dev_mem_alloc] alloc dev mem failed errno 12
Failed to allocate zerocopy buffer for transfer 4
libusb: error [submit_bulk_transfer] submiturb failed error -1 errno=12
Failed to submit transfer 0
Please increase your allowed usbfs buffer size with the following command:
echo 0 > /sys/module/usbcore/parameters/usbfs_memory_mb
```

On Ubuntu you'll need increase the USB memory buffer by running:

```
sudo sh -c 'echo 1000 > /sys/module/usbcore/parameters/usbfs_memory_mb'
```

See [this article](https://importgeek.wordpress.com/2017/02/26/increase-usbfs-memory-limit-in-ubuntu/)
for more info and how to add this to the grub command line so it's permanent.

# Run
Run locally.

```
./vgaplay  -s 130e6 -c 7e6 
```

-s is the sample rate of the software DDS. 
The higher the sample rate the better the sine wave output.
You can probably go up to about 150MS/s

-c is the carrier frequency to generate.
The closer the carrier frequency gets to half the sample rate,
the more the signal becomes a square wave.

Here's how the waveform looks at 7.159MHz with 150Ms/s:

![Beautiful CRO capture](/defaultwaveform.png)
