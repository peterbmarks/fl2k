#fl2k
Using cheap VGA dongles as a digital to analog converter for ham radio

Based on great work from https://osmocom.org/projects/osmo-fl2k/wiki

#Objective

I'm stripping back the supplied FM example and statically linking
the library code to make it easy to play with.

Longer term I'd like to be able to generate a decent HF signal that
can be modulated for things like WSPR.

#Build

Just type:
make

#Run
Run locally.

./vgaplay  -s 130e6 -c 7e6 

-s is the sample rate of the software DDS. 
The higher the sample rate the better the sine wave output.
You can probably go up to about 150MS/s

-c is the carrier frequency to generate.
The closer the carrier frequency gets to half the sample rate,
the more the signal becomes a square wave.

