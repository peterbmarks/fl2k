/*
 * vgaplay, stripped down version of...
 * 
 * osmo-fl2k, turns FL2000-based USB 3.0 to VGA adapters into
 * low cost DACs
 * 
 * On Ubunutu: sudo sh -c 'echo 1000 > /sys/module/usbcore/parameters/usbfs_memory_mb'
 *
 * ./vgaplay -s 130e6 -c 71e5
 * 
 * Copyright below
 */ 

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>

#include <math.h>
#include <pthread.h>
#include <sys/time.h>

#include "osmo-fl2k.h"

#define BUFFER_SAMPLES_SHIFT	16
#define BUFFER_SAMPLES		(1 << BUFFER_SAMPLES_SHIFT)
#define BUFFER_SAMPLES_MASK	((1 << BUFFER_SAMPLES_SHIFT)-1)

void dds_start(double frequency);
void dds_stop();

typedef struct {
	double sample_freq;
	double freq;
	double fslope;
	unsigned long int phase;
	unsigned long int phase_step;
	//unsigned long int phase_slope;
} dds_t;

fl2k_dev_t *gFl2kDevicePtr = NULL;
dds_t gCarrierDds;

int gUserCancelled = 0;
int gTransmitTimeExpired = 0;

pthread_t gWorkerThread;
pthread_mutex_t cb_mutex;
pthread_mutex_t fm_mutex;
pthread_cond_t cb_cond;

int8_t *gTransmitBuffer = NULL;

uint32_t gSampleRate = 150000000;
int gCarrierFrequency = 7159000;
double gDurationOfEachTx;
int gDidSpecifyTime = 0;
long long gStartTimeMs;
uint32_t gFl2kDeviceIndex = 0;

void usage(void)
{
	fprintf(stderr,
		"vgaplay, stripped down code for FL2K VGA dongles\n\n"
		"Usage:"
		"\t[-d device index (default: 0)]\n"
		"\t[-c carrier frequency (default: 7.159 MHz)]\n"
		"\t[-s samplerate in Hz (default: 150 MS/s)]\n"
		"\t[-t time in seconds\n"
		"\t[-f <filename> read frequency list from a file\n"
		"./vgaplay -s 100e6 -c 10e6\n"
	);
	exit(1);
}

// Catches ^C and stops
static void sighandler(int signum)
{
	fprintf(stderr, "Signal caught, exiting!\n");
	dds_stop();
	gUserCancelled = 1;
	exit(0);
}

/* DDS Functions */

#ifndef M_PI
# define M_PI		3.14159265358979323846	/* pi */
# define M_PI_2		1.57079632679489661923	/* pi/2 */
# define M_PI_4		0.78539816339744830962	/* pi/4 */
# define M_1_PI		0.31830988618379067154	/* 1/pi */
# define M_2_PI		0.63661977236758134308	/* 2/pi */
#endif

#define DDS_2PI		(M_PI * 2)		/* 2 * Pi */
#define DDS_3PI2	(M_PI_2 * 3)		/* 3/2 * pi */

#define SIN_TABLE_ORDER	8	// 8 gives 256 values
#define SIN_TABLE_SHIFT	(32 - SIN_TABLE_ORDER)
#define SIN_TABLE_LEN	(1 << SIN_TABLE_ORDER)
#define ANG_INCR	(0xffffffff / DDS_2PI)

int8_t gSineTable[SIN_TABLE_LEN];	// big table of sine values for DDS
int gSineTableInitialised = 0;


// was inline 
void dds_set_freq(dds_t *dds, double freq, double fslope)
{
	fprintf(stderr, "dds_set_freq(%f\n", freq);
	dds->fslope = fslope;
	dds->phase_step = (freq / dds->sample_freq) * 2 * M_PI * ANG_INCR;
	fprintf(stderr, "dds->sample_freq = %f, dds->phase_step = %lu\n", dds->sample_freq, dds->phase_step);
	dds->freq = freq;
}

// write sine values to the gSineTable
dds_t dds_init(double sample_freq, double freq, double phase)
{
	dds_t dds;
	int i;

	dds.sample_freq = sample_freq;
	dds.phase = phase * ANG_INCR;
	dds_set_freq(&dds, freq, 0);

	// Initialize sine table, prescaled for 8 bit signed integer
	if (!gSineTableInitialised) {
		double incr = 1.0 / (double)SIN_TABLE_LEN;
		for (i = 0; i < SIN_TABLE_LEN; i++) {
			gSineTable[i] = sin(incr * i * DDS_2PI) * 127;
      // fprintf(stderr, "sine table value %d = %d\n", i, gSineTable[i]);
		}
		gSineTableInitialised = 1;
	}

	return dds;
}

// return the next value from the sine table and increment the step
int8_t dds_real(dds_t *dds)
{
	int tmp;

	tmp = dds->phase >> SIN_TABLE_SHIFT;
	dds->phase += dds->phase_step;
	dds->phase &= 0xffffffff;

	return gSineTable[tmp];
}

// copy count sine samples from sine table into buf 
void dds_real_buf(dds_t *dds, int8_t *buf, int count) {
	for (int i = 0; i < count; i++) {
		buf[i] = dds_real(dds);
	}
}

/* Signal generation and some helpers */

/* Generate the radio signal using the pre-calculated frequency information
 * in the freq buffer */
 // This runs in the gWorkerThread and modulates the carrier frequency
static void *tx_worker_thread(void *arg)
{
	// Prepare the DDS oscillator
	gCarrierDds = dds_init(gSampleRate, gCarrierFrequency, 0);
	// fill the transmit buffer with sine values
	dds_real_buf(&gCarrierDds, gTransmitBuffer, FL2K_BUF_LEN);
	
	while (!gUserCancelled && !gTransmitTimeExpired) {
		// stay in this thread until they ^C out
		if(gTransmitTimeExpired) {
			fprintf(stderr, "tx_worker_thread transmit time expired\n");
		}
	}
	fprintf(stderr, "tx_worker_thread ending\n");
	pthread_exit(NULL);
}

// USB calls back to get the next buffer of data
void fl2k_callback(fl2k_data_info_t *data_info)
{
	if (data_info->device_error) {
		gUserCancelled = 1;
	}

	pthread_cond_signal(&cb_cond);
	// unblock at least one of the threads that are blocked on the 
	// specified condition variable cond (if any threads are blocked on cond).

	data_info->sampletype_signed = 1;
	data_info->r_buf = (char *)gTransmitBuffer;	// in to red channel buffer
}

long long current_miliseconds() {
    struct timeval te; 
    gettimeofday(&te, NULL); // get current time
    long long milliseconds = te.tv_sec*1000LL + te.tv_usec/1000; // calculate milliseconds
    // printf("milliseconds: %lld\n", milliseconds);
    return milliseconds;
}

void dds_start(double frequency) {
	int r;
	pthread_attr_t attr;
	struct sigaction sigact, sigign;
	
	fl2k_open(&gFl2kDevicePtr, gFl2kDeviceIndex);
	
	if (NULL == gFl2kDevicePtr) {
		fprintf(stderr, "Failed to open fl2k device #%d.\n", gFl2kDeviceIndex);
		exit(0);
	}
	fprintf(stderr, "Opened device\n");

	fprintf(stderr, "dds_start(%f)\n", frequency);
	pthread_mutex_init(&cb_mutex, NULL);
	pthread_cond_init(&cb_cond, NULL);
	pthread_attr_init(&attr);
	
	r = pthread_create(&gWorkerThread, &attr, tx_worker_thread, NULL);
	if (r < 0) {
		fprintf(stderr, "Error spawning TX worker thread!\n");
		return;
	}

	pthread_attr_destroy(&attr);
	r = fl2k_start_tx(gFl2kDevicePtr, fl2k_callback, NULL, 0);

	// Set the sample rate
	r = fl2k_set_sample_rate(gFl2kDevicePtr, gSampleRate);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to set sample rate. %d\n", r);
	}

	/* read back actual frequency */
	gSampleRate = fl2k_get_sample_rate(gFl2kDevicePtr);
	fprintf(stderr, "Actual sample rate = %d\n", gSampleRate);

	//dds_set_freq(&gCarrierDds, frequency, 0.0);
	
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigign.sa_handler = SIG_IGN;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigign, NULL);
}

void dds_stop() {
	fprintf(stderr, "dds_stop()\n");
	fl2k_stop_tx(gFl2kDevicePtr);
	fl2k_close(gFl2kDevicePtr);
}

void dds_change_frequency(double frequency) {
	fprintf(stderr, "dds_change_frequency(%f)\n", frequency);
	dds_set_freq(&gCarrierDds, frequency, 0);
	// rebuild the transmit buffer
	dds_real_buf(&gCarrierDds, gTransmitBuffer, FL2K_BUF_LEN);
}

int main(int argc, char **argv)
{
	int opt;
	
	int option_index = 0;
	FILE *frequencyFile = NULL;
	char frequencyFileName[FILENAME_MAX + 1];
	frequencyFileName[0] = '\0';
	
	struct sigaction sigact, sigign;

	static struct option long_options[] =
	{
		{0, 0, 0, 0}
	};

	while (1) {
		opt = getopt_long(argc, argv, "d:c:f:s:t:", long_options, &option_index);

		/* end of options reached */
		if (opt == -1)
			break;

		switch (opt) {
		case 0:
			break;
		case 'd':
			gFl2kDeviceIndex = (uint32_t)atoi(optarg);
			break;
		case 'c':
			gCarrierFrequency = (uint32_t)atof(optarg);
			break;
		case 's':
			gSampleRate = (uint32_t)atof(optarg);
			break;
		case 't':
			gDurationOfEachTx = (double)atof(optarg);
			gDidSpecifyTime = 1;
			break;
		case 'f':
			strcpy(frequencyFileName, optarg);
			break;
		default:
			usage();
			break;
		}
	}

	if (argc < optind) {
		usage();
	}

	/* allocate buffer */
	gTransmitBuffer = malloc(FL2K_BUF_LEN);
	if (!gTransmitBuffer) {
		fprintf(stderr, "malloc error!\n");
		exit(1);
	}

	fprintf(stderr, "Sine table length: %d\n", SIN_TABLE_LEN);
	fprintf(stderr, "Samplerate:\t%3.2f MHz\n", (double)gSampleRate/1000000);
	fprintf(stderr, "Carrier:\t%3.2f MHz\n", (double)gCarrierFrequency/1000000);
	if(gDidSpecifyTime) {
		fprintf(stderr, "Time of TX:\t%f seconds\n", gDurationOfEachTx);
	}
	if(strlen(frequencyFileName) > 3) {
		fprintf(stderr, "Frequency file: %s\n", frequencyFileName);
		frequencyFile = fopen(frequencyFileName, "r");
		if(frequencyFile == NULL) {
			fprintf(stderr, "Error opening file: %s\n", frequencyFileName);
		}
	}
		
	//if(frequencyFile == 0) {
		dds_start((double)gCarrierFrequency);
	//}

	gStartTimeMs = current_miliseconds();
	long long finishMs = gStartTimeMs + (gDurationOfEachTx * 1000.0);
	if(gDidSpecifyTime) {
		fprintf(stderr, "start ms = %lld until: %lld\n", gStartTimeMs, finishMs);
	}
	
	char * line = NULL;
    size_t len = 0;
    ssize_t read;
	
	while (!gUserCancelled) {
		if(frequencyFile) {
			while((read = getline(&line, &len, frequencyFile)) != -1 && !gUserCancelled) {
				gCarrierFrequency = atof(line);
				fprintf(stderr, "Read frequency = %f from file.\n", (double)gCarrierFrequency);
				gTransmitTimeExpired = 0;
				dds_change_frequency(gCarrierFrequency);
				// keep going until cancelled or time expired
				if(gDidSpecifyTime) {
					long long nowMs = current_miliseconds();
					// fprintf(stderr, "now ms = %lld end = %lld\n", nowMs, finishMs);
					while(nowMs < finishMs && !gUserCancelled) {
						nowMs = current_miliseconds();
					}
					gTransmitTimeExpired = 1;
					fprintf(stderr, "time expired\n");
					gStartTimeMs = current_miliseconds();
					finishMs = gStartTimeMs + (gDurationOfEachTx * 1000.0);
				}
			} 
			fprintf(stderr, "End of TX file\n");
			gUserCancelled = 1;
		}
	}
out:
	dds_stop();

	return 0;
}

/* 
 * Copyright (C) 2016-2018 by Steve Markgraf <steve@steve-m.de>
 *
 * based on FM modulator code from VGASIG:
 * Copyright (C) 2009 by Bartek Kania <mbk@gnarf.org>
 *
 * SPDX-License-Identifier: GPL-2.0+
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
