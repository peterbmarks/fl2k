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

#define AUDIO_BUF_SIZE		1024

fl2k_dev_t *dev = NULL;
int do_exit = 0;

pthread_t fm_thread;
pthread_mutex_t cb_mutex;
pthread_mutex_t fm_mutex;
pthread_cond_t cb_cond;
pthread_cond_t fm_cond;

int8_t *txbuf = NULL;
int8_t *fmbuf = NULL;
int8_t *buf1 = NULL;
int8_t *buf2 = NULL;

uint32_t samp_rate = 150000000;
int carrier_freq = 7159000;

int delta_freq = 75000;
int carrier_per_signal;
int input_freq = 44100;

double *freqbuf; 
double *slopebuf; 
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
	fl2k_stop_tx(dev);
	do_exit = 1;
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

#define SIN_TABLE_ORDER	8
#define SIN_TABLE_SHIFT	(32 - SIN_TABLE_ORDER)
#define SIN_TABLE_LEN	(1 << SIN_TABLE_ORDER)
#define ANG_INCR	(0xffffffff / DDS_2PI)

int8_t sine_table[SIN_TABLE_LEN];	// big table of sin values for DDS
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
	register double freq;
	register double tmp;
	dds_t carrier;
	int8_t *tmp_ptr;
	uint32_t len = 0;
	uint32_t readlen, remaining;
	int buf_prefilled = 0;

	/* Prepare the oscillators */
	carrier = dds_init(samp_rate, carrier_freq, 0);

	while (!do_exit) {
		dds_set_freq(&carrier, carrier_freq, 0.0);
		
		// void dds_set_freq(dds_t *dds, double freq, double fslope)
		//dds_set_freq(&carrier, freqbuf[gBufferReadpos], slopebuf[gBufferReadpos]);
		gBufferReadpos++;
		gBufferReadpos &= BUFFER_SAMPLES_MASK;

		// check if we reach the end of the buffer 
		if ((len + carrier_per_signal) > FL2K_BUF_LEN) {
			readlen = FL2K_BUF_LEN - len;
			remaining = carrier_per_signal - readlen;
			dds_real_buf(&carrier, &fmbuf[len], readlen);

			if (buf_prefilled) {
				// swap fmbuf and txbuf buffers
				tmp_ptr = fmbuf;
				fmbuf = txbuf;
				txbuf = tmp_ptr;
				pthread_cond_wait(&cb_cond, &cb_mutex);
			}

			dds_real_buf(&carrier, fmbuf, remaining);
			len = remaining;

			buf_prefilled = 1;
		} else {
			dds_real_buf(&carrier, &fmbuf[len], carrier_per_signal);
			len += carrier_per_signal;
		}

		pthread_cond_signal(&fm_cond);
	}

	pthread_exit(NULL);
}

int writelen(int maxlen)
{
	int rp = gBufferReadpos;
	int len;
	int r;

	if (rp < gBufferWritepos)
		rp += BUFFER_SAMPLES;

	len = rp - gBufferWritepos;

	r = len > maxlen ? maxlen : len;

	return r;
}

double modulate_sample(int lastwritepos, double lastfreq, double sample)
{
	double freq, slope;

	/* Calculate modulator frequency at this point to lessen
	 * the calculations needed in the signal generator */
	freq = sample * delta_freq;
	freq += carrier_freq;

	/* What we do here is calculate a linear "slope" from
	the previous sample to this one. This is then used by
	the modulator to gently increase/decrease the frequency
	with each sample without the need to recalculate
	the dds parameters. In fact this gives us a very
	efficient and pretty good interpolation filter. */
	slope = freq - lastfreq;
	slope /= carrier_per_signal;
	slopebuf[lastwritepos] = slope;
	freqbuf[gBufferWritepos] = freq;

	return freq;
}

// The main loop during transmission
// I'm hacking this to no longer do FM, just a nice clean carrier (I hope)
void fm_modulator_mono()
{
	unsigned int i;
	size_t len;
	double freq;
	double lastfreq = carrier_freq;
	int16_t audio_buf[AUDIO_BUF_SIZE];
	uint32_t lastwritepos = gBufferWritepos;
	double sample;

	while (!do_exit) {
		// Modulate and buffer the sample 
		lastfreq = carrier_freq;
		lastwritepos = gBufferWritepos++;
		gBufferWritepos %= BUFFER_SAMPLES;
	}
	
}

void fl2k_callback(fl2k_data_info_t *data_info)
{
	if (data_info->device_error) {
		do_exit = 1;
		pthread_cond_signal(&fm_cond);
	}

	pthread_cond_signal(&cb_cond);

	data_info->sampletype_signed = 1;
	data_info->r_buf = (char *)txbuf;	// in to red channel buffer
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
			carrier_freq = (uint32_t)atof(optarg);
			break;
		case 's':
			samp_rate = (uint32_t)atof(optarg);
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
	buf1 = malloc(FL2K_BUF_LEN);
	buf2 = malloc(FL2K_BUF_LEN);
	if (!buf1 || !buf2) {
		fprintf(stderr, "malloc error!\n");
		exit(1);
	}

	fmbuf = buf1;
	txbuf = buf2;

	/* Decoded audio */
	freqbuf = malloc(BUFFER_SAMPLES * sizeof(double));
	slopebuf = malloc(BUFFER_SAMPLES * sizeof(double));
	gBufferReadpos = 0;
	gBufferWritepos = 1;

	fprintf(stderr, "Samplerate:\t%3.2f MHz\n", (double)samp_rate/1000000);
	fprintf(stderr, "Carrier:\t%3.2f MHz\n", (double)carrier_freq/1000000);

	pthread_mutex_init(&cb_mutex, NULL);
	pthread_mutex_init(&fm_mutex, NULL);
	pthread_cond_init(&cb_cond, NULL);
	pthread_cond_init(&fm_cond, NULL);
	pthread_attr_init(&attr);

	fl2k_open(&dev, (uint32_t)dev_index);
	if (NULL == dev) {
		fprintf(stderr, "Failed to open fl2k device #%d.\n", dev_index);
		goto out;
	}

	r = pthread_create(&fm_thread, &attr, fm_worker, NULL);
	if (r < 0) {
		fprintf(stderr, "Error spawning FM worker thread!\n");
		goto out;
	}

	pthread_attr_destroy(&attr);
	r = fl2k_start_tx(dev, fl2k_callback, NULL, 0);

	/* Set the sample rate */
	r = fl2k_set_sample_rate(dev, samp_rate);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set sample rate. %d\n", r);

	/* read back actual frequency */
	samp_rate = fl2k_get_sample_rate(dev);

	/* Calculate needed constants */
	carrier_per_signal = samp_rate / input_freq;

	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigign.sa_handler = SIG_IGN;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigign, NULL);

	fm_modulator_mono();

out:
	fl2k_close(dev);

	free(freqbuf);
	free(slopebuf);
	free(buf1);
	free(buf2);

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
