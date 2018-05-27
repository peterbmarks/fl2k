//
//  main.c
//  Generate AM samples to send with fl2k_file
//
//  Created by Peter Marks on 26/5/18.
//  Copyright © 2018 Peter Marks. All rights reserved.
//

// fl2k_file samples.dat
#include <stdio.h>
#include <math.h>
#include <stdint.h>

const char *outFileName = "samples.dat";

double radian(double degree) {
    return degree * M_PI / 180.0;
}
/*
 * // produce a sine wave suitable for fl2k_file
int main(int argc, const char * argv[]) {
    FILE *outfile = fopen(outFileName, "wb");
    int8_t byte;
    for(int degree = 0; degree < (360 * 1); degree++) {
        double carrier_sin_value = sin(radian(degree % 360));
        byte = (int8_t)(carrier_sin_value * 127.0);
        printf("%03d, val = %f, byte = %d\n", degree, carrier_sin_value, (int)byte);
        fwrite(&byte, sizeof(byte), 1, outfile);
    }
    fclose(outfile);
    return 0;
}
*/

// Produces an AM'd signal suitable for fl2k_file
int main(int argc, const char * argv[]) {
    // ratio of the carrier to the modulating sine wave
    int ratio = 100;

    FILE *outfile = fopen(outFileName, "wb");

    for(int degree = 0; degree < (360 * ratio); degree++) {
        double carrier_degree = (double)degree / (double)ratio;
        double mod_degree = (double)(degree % 360);
        double carrier_sin_value = sin(radian(carrier_degree));
        double mod_sin_value = sin(radian(mod_degree));
        double am_sample = carrier_sin_value * mod_sin_value;
        //printf("%f\t%f\t%f\n", carrier_sin_value, mod_sin_value, am_samples);
        //printf("%f\n", am_samples);

        // turn -1 to +1 into 0 to 255
        int8_t byte = (int8_t)(am_sample * 127.0);
        printf("byte = %d\n", byte);
        fwrite(&byte, sizeof(byte), 1, outfile);
    }
    fclose(outfile);

    return 0;
}

