/*
 * vgaplay, stripped down version of...
 * 
 * osmo-fl2k, turns FL2000-based USB 3.0 to VGA adapters into
 * low cost DACs
 * 
 * On Ubunutu: sudo sh -c 'echo 1000 > /sys/module/usbcore/parameters/usbfs_memory_mb'
 *
 * ./vgaplay  -s 130e6 -c 71e5
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

#include "osmo-fl2k.h"

#define BUFFER_SAMPLES_SHIFT	16
#define BUFFER_SAMPLES		(1 << BUFFER_SAMPLES_SHIFT)
#define BUFFER_SAMPLES_MASK	((1 << BUFFER_SAMPLES_SHIFT)-1)

fl2k_dev_t *gFl2kDevice = NULL;

int gUserCancelled = 0;

pthread_t fm_thread;
pthread_mutex_t cb_mutex;
pthread_mutex_t fm_mutex;
pthread_cond_t cb_cond;
pthread_cond_t fm_cond;

int8_t *gTransmitBuffer = NULL;
int8_t *gFmBuffer = NULL;
int8_t *gBuffer1 = NULL;
int8_t *gBuffer2 = NULL;

uint32_t gSampleRate = 150000000;
int gCarrierFrequency = 7159000;

double *gFrequencyBuffer; 
int gBufferWritepos, gBufferReadpos;

void usage(void)
{
	fprintf(stderr,
		"vgaplay, stripped down code for FL2K VGA dongles\n\n"
		"Usage:"
		"\t[-d device index (default: 0)]\n"
		"\t[-c carrier frequency (default: 7.159 MHz)]\n"
		"\t[-s samplerate in Hz (default: 150 MS/s)]\n"
		"./vgaplay -s 100e6 -c 10e6\n"
	);
	exit(1);
}

// Catches ^C and stops
static void sighandler(int signum)
{
	fprintf(stderr, "Signal caught, exiting!\n");
	fl2k_stop_tx(gFl2kDevice);
	gUserCancelled = 1;
	pthread_cond_signal(&fm_cond);
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

int8_t sine_table[SIN_TABLE_LEN];	// big table of sine values for DDS
int sine_table_init = 0;

typedef struct {
	double sample_freq;
	double freq;
	double fslope;
	unsigned long int phase;
	unsigned long int phase_step;
	unsigned long int phase_slope;
} dds_t;

inline void dds_setphase(dds_t *dds, double phase)
{
	dds->phase = phase * ANG_INCR;
}

inline double dds_getphase(dds_t *dds)
{
	return dds->phase / ANG_INCR;
}

// was inline 
void dds_set_freq(dds_t *dds, double freq, double fslope)
{
	// printf("dds_set_freq(%f, slope=%f)\n", freq, fslope);
	dds->fslope = fslope;
	dds->phase_step = (freq / dds->sample_freq) * 2 * M_PI * ANG_INCR;

	/* The slope parameter is used with the FM modulator to create
	 * a simple but very fast and effective interpolation filter.
	 * See the fm modulator for details */
	dds->freq = freq;
	dds->phase_slope = (fslope / dds->sample_freq) * 2 * M_PI * ANG_INCR;
}

dds_t dds_init(double sample_freq, double freq, double phase)
{
	dds_t dds;
	int i;

	dds.sample_freq = sample_freq;
	dds.phase = phase * ANG_INCR;
	dds_set_freq(&dds, freq, 0);

	// Initialize sine table, prescaled for 8 bit signed integer
	if (!sine_table_init) {
		double incr = 1.0 / (double)SIN_TABLE_LEN;
		for (i = 0; i < SIN_TABLE_LEN; i++)
			sine_table[i] = sin(incr * i * DDS_2PI) * 127;

		sine_table_init = 1;
	}

	return dds;
}

// return the next value from the sine table
int8_t dds_real(dds_t *dds)
{
	int tmp;

	tmp = dds->phase >> SIN_TABLE_SHIFT;
	dds->phase += dds->phase_step;
	dds->phase &= 0xffffffff;

	dds->phase_step += dds->phase_slope;

	return sine_table[tmp];
}

// copy count sine samples from sine table into buf 
void dds_real_buf(dds_t *dds, int8_t *buf, int count)
{
	int i;
	for (i = 0; i < count; i++)
		buf[i] = dds_real(dds);
}

/* Signal generation and some helpers */

/* Generate the radio signal using the pre-calculated frequency information
 * in the freq buffer */
 // This runs in the fm_thread and modulates the carrier frequency
static void *fm_worker(void *arg)
{
	dds_t carrier;

	// Prepare the DDS oscillator
	carrier = dds_init(gSampleRate, gCarrierFrequency, 0);
	// fill the transmit buffer with sine values
	dds_real_buf(&carrier, gTransmitBuffer, FL2K_BUF_LEN);
	
	while (!gUserCancelled) {
		// stay in this thread until they ^C out
	}

	pthread_exit(NULL);
}

// The main loop during transmission
// I'm hacking this to no longer do FM, just a nice clean carrier (I hope)
// Now just sits here wrapping the buffer write position back to the start
void wrap_sine_buffer_position_until_exit()
{
	while (!gUserCancelled) {
		gBufferWritepos %= BUFFER_SAMPLES;
	}
	
}

// USB calls back to get the next buffer of data
void fl2k_callback(fl2k_data_info_t *data_info)
{
	if (data_info->device_error) {
		gUserCancelled = 1;
		pthread_cond_signal(&fm_cond);
	}

	pthread_cond_signal(&cb_cond);

	data_info->sampletype_signed = 1;
	data_info->r_buf = (char *)gTransmitBuffer;	// in to red channel buffer
}

int main(int argc, char **argv)
{
	int r, opt;
	int dev_index = 0;
	pthread_attr_t attr;
	int option_index = 0;

	struct sigaction sigact, sigign;

	static struct option long_options[] =
	{
		{0, 0, 0, 0}
	};

	while (1) {
		opt = getopt_long(argc, argv, "d:c:f:i:s:", long_options, &option_index);

		/* end of options reached */
		if (opt == -1)
			break;

		switch (opt) {
		case 0:
			break;
		case 'd':
			dev_index = (uint32_t)atoi(optarg);
			break;
		case 'c':
			gCarrierFrequency = (uint32_t)atof(optarg);
			break;
		case 's':
			gSampleRate = (uint32_t)atof(optarg);
			break;
		default:
			usage();
			break;
		}
	}

	if (argc < optind) {
		usage();
	}

	if (dev_index < 0) {
		exit(1);
	}

	/* allocate buffer */
	gTransmitBuffer = malloc(FL2K_BUF_LEN);
	if (!gTransmitBuffer) {
		fprintf(stderr, "malloc error!\n");
		exit(1);
	}

	gBufferReadpos = 0;
	gBufferWritepos = 1;

	fprintf(stderr, "Sine table length: %d\n", SIN_TABLE_LEN);
	fprintf(stderr, "Samplerate:\t%3.2f MHz\n", (double)gSampleRate/1000000);
	fprintf(stderr, "Carrier:\t%3.2f MHz\n", (double)gCarrierFrequency/1000000);

	pthread_mutex_init(&cb_mutex, NULL);
	pthread_mutex_init(&fm_mutex, NULL);
	pthread_cond_init(&cb_cond, NULL);
	pthread_cond_init(&fm_cond, NULL);
	pthread_attr_init(&attr);

	fl2k_open(&gFl2kDevice, (uint32_t)dev_index);
	if (NULL == gFl2kDevice) {
		fprintf(stderr, "Failed to open fl2k device #%d.\n", dev_index);
		goto out;
	}

	r = pthread_create(&fm_thread, &attr, fm_worker, NULL);
	if (r < 0) {
		fprintf(stderr, "Error spawning FM worker thread!\n");
		goto out;
	}

	pthread_attr_destroy(&attr);
	r = fl2k_start_tx(gFl2kDevice, fl2k_callback, NULL, 0);

	/* Set the sample rate */
	r = fl2k_set_sample_rate(gFl2kDevice, gSampleRate);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to set sample rate. %d\n", r);
	}

	/* read back actual frequency */
	gSampleRate = fl2k_get_sample_rate(gFl2kDevice);
	fprintf(stderr, "Actual sample rate = %d\n", gSampleRate);

	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigign.sa_handler = SIG_IGN;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigign, NULL);

	wrap_sine_buffer_position_until_exit();	// loops here until signal

out:
	fl2k_close(gFl2kDevice);

	free(gFrequencyBuffer);
	free(gBuffer1);
	free(gBuffer2);

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
