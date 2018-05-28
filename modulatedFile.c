//
//  main.c
//  Generate AM samples to send with fl2k_file
//
//  Created by Peter Marks on 26/5/18.
//  Copyright © 2018 Peter Marks. All rights reserved.
//
// Default sample rate for fl2k_file is 100e6 or 100MHz
// so 100e6 / 60 samples gives a carrier on 1.66MHz
// 80 samples gives a carrier on 1.25MHz
// 

// fl2k_file samples.dat
#include <stdio.h>
#include <math.h>
#include <stdint.h>

const char *outFileName = "samples.dat";

void makeCarrier(int samplesPerCycle) {
    FILE *outfile = fopen(outFileName, "wb");
    int8_t byte;
    for(int sample = 0; sample < samplesPerCycle; sample++) {
		double current_radian = M_PI * sample * 2 / samplesPerCycle;
        double carrier_sin_value = sin(current_radian);
        byte = (int8_t)(carrier_sin_value * 127.0);
        printf("%f, val = %f, byte = %d\n", current_radian, carrier_sin_value, (int)byte);
        fwrite(&byte, sizeof(byte), 1, outfile);
    }
    fclose(outfile);
}



// Produces an AM'd signal suitable for fl2k_file
void makeAm(int samplesPerCycle) {
    // ratio of the carrier to the modulating sine wave
    int ratio = 3000;

    FILE *outfile = fopen(outFileName, "wb");
    // make sure we get enough samples for a full wave of the modulation
	int totalSamples = samplesPerCycle * ratio;
    for(int sample = 0; sample < totalSamples; sample++) {
		double carrier_radian = fmod((M_PI * sample * 2 / samplesPerCycle),(M_PI * 2));
        double mod_radian = fmod((M_PI * sample * 2 / samplesPerCycle) / ratio,(M_PI * 2));
        
		//printf("%03d carrier r = %f, mod r = %f\n", sample, carrier_radian, mod_radian);

        double carrier_sin_value = sin(carrier_radian);
        double mod_sin_value = sin(mod_radian);
        
        double am_sample = carrier_sin_value * mod_sin_value;
        //printf("%f\t%f\t%f\n", carrier_sin_value, mod_sin_value, am_sample);

        int8_t byte = (int8_t)(am_sample * 127.0);
        //printf("byte = %d\n", byte);
        fwrite(&byte, sizeof(byte), 1, outfile);
    }
    fclose(outfile);
}

// produce a sine wave suitable for fl2k_file
int main(int argc, const char * argv[]) {
	//makeCarrier(80);
	makeAm(80);
	return 0;
}
