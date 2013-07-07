/*
 *  VPS-based TS recoder
 *
 *  Copyright (C) 2008 Ondrej Caletka
 *
 *  based on network.c from zvbi package
 *  Copyright (C) 2006 Michael H. Schimek
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*    gcc -o network network.c `pkg-config zvbi-0.2 --cflags --libs` */

#undef NDEBUG

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <locale.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#define STDIN_FILENO 0
#define STDOUT_FILENO 1

#ifdef HAVE_GETOPT_LONG
#  include <getopt.h>
#endif

#include <libzvbi.h>

#ifndef CLEAR
#  define CLEAR(var) memset (&(var), 0, sizeof (var))
#endif

#ifndef N_ELEMENTS
#  define N_ELEMENTS(array) (sizeof (array) / sizeof (*(array)))
#endif

int delay_after_stop = 5;
int mi_delay = 30;

#define LCI_VPS 4		/* Fictionous LCI to identify VPS messages */

enum {
	FLAGS_VPS_AND_PDC = 0,
	FLAGS_NO_PDC,
	FLAGS_NO_VPS,
};

typedef enum {
	VPS_DATETIME,
	VPS_TCC,		/* Timer Control Code */
	VPS_RIT,		/* Record Inhibit/Terminate */
	VPS_ITC,		/* Interuption Code */
	VPS_CTC,		/* Continuation Code */
	VPS_OTHER		/* Unknown code */
} vps_status;

typedef struct {
	vbi_audio_mode audio;
	vps_status status;
	uint8_t day;
	uint8_t month;
	uint8_t hour;
	uint8_t min;
	uint16_t cni;
	uint8_t pty;
	uint8_t lci;
	vbi_bool luf;
	vbi_bool prf;
	vbi_bool mi;
} vps_data;

static int verbosity = 2;
static int ts_fd, out_fd;
static vbi_dvb_demux *dx;
static vbi_bool quit;
static vbi_bool should_quit = 1;
static vps_data record_time;
static vbi_bool record_time_date;
static vbi_bool record_time_time;
static vbi_bool record;
static int record_limit;
static int vpsflag;

int logger(int level, const char *format, ...)
{
	va_list ap, aq;
	int r;

	if (verbosity >= level) {
		va_start(ap, format);
		r = vfprintf(stderr, format, ap);
		va_end(ap);
		return r;
	}
}

void alarmhandler(int signum)
{
	if (signum == SIGALRM) {
		signal(SIGALRM, &alarmhandler);
		if (should_quit) {
			record = 0;
			quit = 1;
		} else {
			record = 0;
			should_quit = 1;
			alarm(record_limit);
		}
		logger(1, "Stopped.\n");
	}
}

void checkrecord(vps_data * vps)
{
	static int stored_lci = -1;
	static int mi_flag = 0;
	static int stopping = 0;

	if (stored_lci != -1 && vps->lci != stored_lci)
		return;

	if (vps->status == VPS_DATETIME && vps->prf == 0) {
		if ((!record_time_time ||
		     (vps->hour == record_time.hour &&
		      vps->min == record_time.min)) &&
		    (!record_time_date ||
		     (vps->day == record_time.day
		      && vps->month == record_time.month))) {

			mi_flag = vps->mi;
			if (!record) {
				stopping = 0;
				stored_lci = vps->lci;
				record = 1;
				logger(1, "Starting record...\n");
				alarm(0);	//Deactivate alarm
			}
			return;
		}
	}

	if (record && !stopping) {
		stopping = 1;
		logger(1, "Stopping record...\n");
		if (vps->status == VPS_RIT)	/*program ended */
			should_quit = 1;
		else
			should_quit = 0;

		/*postponed stop */
		if (mi_flag == 0)
			alarm(delay_after_stop + mi_delay);
		else
			alarm(delay_after_stop);

	}
}

static void get_vps_data(vps_data * data, uint8_t * buffer)
{

	assert(buffer != NULL);
	data->day = (buffer[8] & 0x3e) >> 1;
	data->month = ((buffer[8] & 0x01) << 3) | ((buffer[9] & 0xe0) >> 5);
	data->hour = buffer[9] & 0x1f;
	data->min = (buffer[10] & 0xfc) >> 2;
	data->cni = ((buffer[10] & 0x03) << 8) | buffer[11];
	data->pty = buffer[12];
	data->lci = LCI_VPS;
	data->luf = 0;
	data->prf = 0;
	data->mi = 1;  // 1 - Transmissions ends exactly with the label.

	switch (buffer[2] & 0xC0) {
	case 0x00:
		data->audio = VBI_AUDIO_MODE_UNKNOWN;
		break;
	case 0x40:
		data->audio = VBI_AUDIO_MODE_MONO;
		break;
	case 0x80:
		data->audio = VBI_AUDIO_MODE_STEREO;
		break;
	case 0xC0:
		data->audio = VBI_AUDIO_MODE_VIDEO_DESCRIPTIONS;
		break;
	}

	if (data->day == 0 && data->month == 15 && data->min == 63) {
		/*Service Code */
		switch (data->hour) {
		case 31:
			data->status = VPS_TCC;
			break;
		case 30:
			data->status = VPS_RIT;
			break;
		case 29:
			data->status = VPS_ITC;
			break;
		case 28:
			data->status = VPS_CTC;
			break;
		default:
			assert(0);
		}
		return;
	}
	if (data->day > 0 && data->day <= 31 &&
	    data->month > 0 && data->month <= 12 &&
	    data->hour >= 0 && data->hour < 24 && data->min >= 0
	    && data->min < 60) {
		data->status = VPS_DATETIME;
		return;
	}
	data->status = VPS_OTHER;
}

static int get_pdc_data(vps_data * data, uint8_t * buffer)
{
	const static uint8_t bit_invert[] = {	//bit-inverse mapping LUT
		0x00, 0x08, 0x04, 0x0c, 0x02, 0x0a, 0x06, 0x0e,
		0x01, 0x09, 0x05, 0x0d, 0x03, 0x0b, 0x07, 0x0f
	};

	static uint8_t buf[13];

	int i, j;
	assert(buffer != NULL);

	for (i = 0; i < 13; i++) {
		j = vbi_unham8(buffer[i + 9]);
		if (j < 0 || j > 15)
			return -1;
		buf[i] = bit_invert[j];
		//  printf("%02x ",j);
	}
	//puts("\n\n");

	data->lci = (buf[0] & 0x0c) >> 2;
	data->luf = buf[0] & 0x02;
	data->prf = buf[0] & 0x01;
	data->mi = buf[1] & 0x02;
	data->day = ((buf[3] & 0x03) << 3) | (buf[4] >> 1);
	data->month = ((buf[4] & 0x01) << 3) | (buf[5] >> 1);
	data->hour = ((buf[5] & 0x01) << 4) | buf[6];
	data->min = (buf[7] << 2) | ((buf[8] & 0x0c) >> 2);
	data->cni = (buf[2] << 12) | ((buf[8] & 0x03) << 10) |
	    ((buf[9] & 0x0c) << 8) | ((buf[3] & 0x0c) << 6) |
	    ((buf[9] & 0x03) << 4) | buf[10];
	data->pty = (buf[11] << 4) | buf[12];

	switch (buf[1] & 0xC0) {
	case 0x00:
		data->audio = VBI_AUDIO_MODE_UNKNOWN;
		break;
	case 0x40:
		data->audio = VBI_AUDIO_MODE_MONO;
		break;
	case 0x80:
		data->audio = VBI_AUDIO_MODE_STEREO;
		break;
	case 0xC0:
		data->audio = VBI_AUDIO_MODE_VIDEO_DESCRIPTIONS;
		break;
	}

	if (data->day == 0 && data->month == 15 && data->min == 63) {
		/*Service Code */
		switch (data->hour) {
		case 31:
			data->status = VPS_TCC;
			break;
		case 30:
			data->status = VPS_RIT;
			break;
		case 29:
			data->status = VPS_ITC;
			break;
		case 28:
			data->status = VPS_CTC;
			break;
		default:
			assert(0);
		}
		goto success;
	}
	if (data->day > 0 && data->day <= 31 &&
	    data->month > 0 && data->month <= 12 &&
	    data->hour >= 0 && data->hour < 24 && data->min >= 0
	    && data->min < 60) {
		data->status = VPS_DATETIME;
		goto success;
	}
	data->status = VPS_OTHER;
 success:
	return 0;
}

static void print_vps_state(vps_data * vpsdata)
{
	time_t rt;
	struct tm *cas;

	time(&rt);
	cas = localtime(&rt);
	logger(2, "%04d-%02d-%02d %02d:%02d:%02d: ",
	       1900 + cas->tm_year,
	       1 + cas->tm_mon,
	       cas->tm_mday, cas->tm_hour, cas->tm_min, cas->tm_sec);

#ifdef LOG_PTYAUDIO
	logger(2, "PTY: %02x ", vpsdata->pty);
	switch (vpsdata->audio) {
	case VBI_AUDIO_MODE_MONO:
		logger(2, "MONO ");
		break;
	case VBI_AUDIO_MODE_STEREO:
		logger(2, "STEREO ");
		break;
	case VBI_AUDIO_MODE_VIDEO_DESCRIPTIONS:
		logger(2, "DUAL ");
		break;
	}
#endif				/* LOG_PTYAUDIO */
	if (vpsdata->lci < 4)
		logger(2, "LCI=%x ", vpsdata->lci);
	else
		logger(2, "*VPS* ");
	if (vpsdata->luf)
		logger(2, "LUF=1 ");
	if (vpsdata->prf)
		logger(2, "PRF=1 ");
	if (vpsdata->mi)
		logger(2, "MI=1 ");

	switch (vpsdata->status) {
	case VPS_DATETIME:
		logger(2, "%2d.%d. %02d:%02d NORMAL\n",
		       vpsdata->day, vpsdata->month, vpsdata->hour,
		       vpsdata->min);
		break;
	case VPS_RIT:
		logger(2, "Record Inhibit/Terminate\n");
		break;
	case VPS_TCC:
		logger(2, "Time Control Code\n");
		break;
	case VPS_ITC:
		logger(2, "Interuption Code\n");
		break;
	case VPS_CTC:
		logger(2, "Continuation Code\n");
		break;
	 /*XXX*/ default:
		break;
	}
}

static ssize_t safe_read(int fd, void *buf, size_t count)
{
	ssize_t actual;

	actual = read(fd, buf, count);

	if (actual < 0) {
		perror("File read failed");
		exit(EXIT_FAILURE);
	}
	if (actual == 0) {
		logger(1, "File read failed (EOF?)\n");
		exit(EXIT_FAILURE);
	}
	return actual;
}

static void mainloop(void)
{
	vbi_sliced sliced_buffer[64];
	uint8_t tsbuffer[3948];	/* 21x188 Bytes */

	vps_data oldvps[5], newvps;

	while (!quit) {
		unsigned int n_lines;
		double sample_time;
		uint8_t *readstart;
		unsigned int readlen;
		int64_t pts;
		int i;
		int actual;
		int fail;

		/* First, align to the TS start - first byte must be 0x47 */
		do {
			safe_read(ts_fd, tsbuffer, 1);
		}
		while (tsbuffer[0] != 0x47);

		readlen =
		    1 + safe_read(ts_fd, tsbuffer + 1, sizeof(tsbuffer) - 1);

		/* ensure buffer is aligned */
		while ((readlen % 188) != 0 && readlen < sizeof(tsbuffer)) {
			readlen +=
			    safe_read(ts_fd, tsbuffer + readlen,
				      sizeof(tsbuffer) - readlen);
		}

		fail = 0;
		for (i = 0; i < readlen; i += 188) {
			if (tsbuffer[i] != 0x47) {
				fail = 1;
				break;
			}
		}
		if (fail) {	/* Bad sync, drop this buffer and try again */
			continue;
		}

		if (record) {
			unsigned int written = 0;
			while (written < readlen) {
				actual =
				    write(out_fd, tsbuffer + written,
					  readlen - written);
				if (actual <= 0) {
					logger(0, "Write ERROR: %s\n",
					       strerror(errno));
					exit(EXIT_FAILURE);
				}
				written += actual;
			}
		}

		readstart = tsbuffer;
		while (readlen > 0) {

			n_lines = vbi_dvb_demux_cor(dx,
						    sliced_buffer,
						    N_ELEMENTS(sliced_buffer),
						    &pts,
						    (const uint8_t **)
						    &readstart, &readlen);

			for (i = 0; i < n_lines; i++) {
				vbi_sliced *slice = sliced_buffer + i;
				if (slice->id == VBI_SLICED_VPS ||
				    slice->id == VBI_SLICED_VPS_F2) {

					get_vps_data(&newvps, slice->data);

					if (memcmp
					    (&newvps, &(oldvps[4]),
					     sizeof(vps_data)) != 0) {
						print_vps_state(&newvps);
						oldvps[4] = newvps;
						if (vpsflag != FLAGS_NO_VPS)
							checkrecord(&newvps);
					}
				}

				if (slice->id == VBI_SLICED_TELETEXT_B) {
					int pmag = vbi_unham16p(slice->data);
					int mag = pmag & 0x07;
					int packet = pmag >> 3;
					int dcode = vbi_unham8(slice->data[2]);

					if (mag == 0 && packet == 30
					    && (dcode & 0x0e) == 0x02) {
						if (get_pdc_data
						    (&newvps, slice->data) == 0
						    && memcmp(&newvps,
							      &(oldvps
								[newvps.lci]),
							      sizeof(vps_data))
						    != 0) {
							print_vps_state
							    (&newvps);
							oldvps[newvps.lci] =
							    newvps;
							if (vpsflag !=
							    FLAGS_NO_PDC)
								checkrecord
								    (&newvps);
						}
					}
				}
			}
		}
	}

}

static void usage(FILE * fp)
{
	fprintf(fp, "\
%s %s -- VPS-based TS recorder\n\n\
Copyright (C) 2008, 2009 Ondrej Caletka\n\n\
based on decode.c from zvbi\n\
Copyright (C) 2004, 2006, 2007 Michael H. Schimek\n\
This program is licensed under GPLv2. NO WARRANTIES.\n\n\
Usage: %s [options] < MPEG TS\n\
-h | --help | --usage  Print this message and exit\n\
-q | --quiet           Suppress progress and error messages\n\
-v | --verbose         Increase verbosity\n\
-V | --version         Print the program version and exit\n\
Input options:\n\
-i | --input name      Read the TS from this file instead of\n\
                       standard input\n\
-p | --pid pid         Specify VBI stream PID (289)\n\
Output options:\n\
-o | --output name     Write recorded TS to this file instead of\n\
                       standard output\n\
\n\
-t | --time hh:mm      Record this PIL time\n\
-d | --date dd.mm.     Record this PIL date\n\
-l | --limit sec       Wait at most sec seconds, then quit\n\
     --no-pdc          Don't record by PDC data\n\
     --no-vps          Don't record by VPS data\n\
-a | --afterstop <seconds> Add <seconds> of recording after\n\
                           stop signal (5)\n\
-m | --miwait <secs>   How many seconds to add to recording\n\
                       when MI flag is NOT set (30)\n\
\n\
", PACKAGE_NAME, VERSION, program_invocation_name);
}

static const char short_options[] = "hqvVi:p:o:t:d:l:a:m:";

#ifdef HAVE_GETOPT_LONG
static const struct option long_options[] = {
	{"help", no_argument, NULL, 'h'},
	{"usage", no_argument, NULL, 'h'},
	{"quiet", no_argument, NULL, 'q'},
	{"verbose", no_argument, NULL, 'v'},
	{"version", no_argument, NULL, 'V'},
	{"input", required_argument, NULL, 'i'},
	{"pid", required_argument, NULL, 'p'},
	{"output", required_argument, NULL, 'o'},
	{"time", required_argument, NULL, 't'},
	{"date", required_argument, NULL, 'd'},
	{"limit", required_argument, NULL, 'l'},
	{"no-pdc", no_argument, &vpsflag, FLAGS_NO_PDC},
	{"no-vps", no_argument, &vpsflag, FLAGS_NO_VPS},
	{"afterstop", required_argument, NULL, 'a'},
	{"miwait", required_argument, NULL, 'm'},
	{NULL, 0, 0, 0}
};
#else
#  define getopt_long(ac, av, s, l, i) getopt(ac, av, s)
#endif

static int option_index;

int main(int argc, char *argv[])
{
	char *filename = NULL;
	char *outfilename = NULL;
	unsigned int tspid = 289;
	char *errstr;
	vbi_bool success;

	setlocale(LC_ALL, "");

	for (;;) {
		int c;

		c = getopt_long(argc, argv, short_options,
				long_options, &option_index);
		if (-1 == c)
			break;

		switch (c) {
		case 0:	/* getopt_long() flag */
			break;

		case 'i':
			filename = optarg;
			break;
		case 'p':
			tspid = atoi(optarg);
			break;
		case 'o':
			outfilename = optarg;
			break;
		case 't':
			c = sscanf(optarg, "%hhu:%hhu", &(record_time.hour),
				   &(record_time.min));
			if (c != 2) {
				fprintf(stderr, "ERROR: Garbled time\n");
				usage(stderr);
				exit(EXIT_FAILURE);
			}
			record_time_time = 1;
			break;
		case 'd':
			c = sscanf(optarg, "%hhu.%hhu", &(record_time.day),
				   &(record_time.month));
			if (c != 2) {
				fprintf(stderr, "ERROR: Garbled date\n");
				usage(stderr);
				exit(EXIT_FAILURE);
			}
			record_time_date = 1;
			break;
		case 'l':
			c = sscanf(optarg, "%d", &record_limit);
			if (c != 1) {
				fprintf(stderr, "ERROR: bad limit\n");
				usage(stderr);
				exit(EXIT_FAILURE);
			}
			break;

		case 'a':
			delay_after_stop = atoi(optarg);
			if (delay_after_stop < 1) {
				fprintf(stderr,
					"ERROR: delay after stop should be greater or equal to 1.\n");
				usage(stderr);
				exit(EXIT_FAILURE);
			}
			break;
		case 'm':
			mi_delay = atoi(optarg);
			if (mi_delay < 0) {
				fprintf(stderr,
					"ERROR: MI delay should be positive.\n");
				usage(stderr);
				exit(EXIT_FAILURE);
			}
			break;
		case 'v':
			verbosity++;
			break;
		case 'q':
			verbosity = 0;
			break;
		case 'h':
			usage(stdout);
			exit(EXIT_SUCCESS);
		case 'V':
			printf(PACKAGE_NAME " " VERSION "\n");
			exit(EXIT_SUCCESS);

		default:
			usage(stderr);
			exit(EXIT_FAILURE);
		}
	}

	if (filename == NULL || strcmp(filename, "-") == 0) {
		ts_fd = STDIN_FILENO;
		if (isatty(ts_fd)) {
			fprintf(stderr,
				"Input should be redirected from MPEG TS.\n");
			exit(EXIT_FAILURE);
		}
	} else {
		ts_fd = open(filename, O_RDONLY, 0);
		if (ts_fd < 0) {
			perror("File open failed");
			exit(EXIT_FAILURE);
		}
	}

	if (outfilename == NULL || strcmp(outfilename, "-") == 0) {
		out_fd = STDOUT_FILENO;
		if (isatty(out_fd)) {
			logger(1,
			       "Output should be redirected. Not writing any data.\n");
			out_fd = open("/dev/null", O_WRONLY);
		}
	} else {
		out_fd =
		    open(outfilename, O_WRONLY | O_CREAT | O_TRUNC,
			 S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
		if (out_fd < 0) {
			perror("File open failed");
			exit(EXIT_FAILURE);
		}
	}

	dx = (vbi_dvb_demux *) _vbi_dvb_ts_demux_new( /*callback */ NULL,
						     /*user data */ NULL,
						     tspid);
	assert(NULL != dx);

	should_quit = 1;
	alarm(record_limit);
	signal(SIGALRM, &alarmhandler);	//for exiting stuck records

	mainloop();

	logger(1, "Quitting...\n");

	vbi_dvb_demux_delete(dx);
	dx = NULL;

	close(ts_fd);
	close(out_fd);
	exit(EXIT_SUCCESS);
}
