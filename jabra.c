/*
 * Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <glob.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#include <alsa/asoundlib.h>
#include <linux/input.h>

#define DAEMON_NAME "jabra_vold"

/* endpoint used for the HID interrupt */
#define JABRA_ENDPOINT 0x81

/*
 * Report numbers used by the Jabra device.
 *
 * We could parse them from the HID descriptors,
 * but that's long and complicated.
 */
enum jabra_report {
	JABRA_HID_REPORT = 1,
	JABRA_TELEPHONY_REPORT = 3,
};

/* bit for mute key press in the telephony report */
#define MUTE_FLAG 0x08

/* Names used by the kernel Alsa audio driver */
#define ALSA_DEV_NAME "Jabra SPEAK"
#define ALSA_CONTROL_NAME "PCM Playback Volume"

/* shell pattern to find the keyboard input device node */
#define INPUT_DEV_PATTERN "/dev/input/by-path/*-event-kbd"

/* ChromeOS keyboard is using F9 rather than KEY_VOLUMEDOWN */
#define KEY_CROS_VOLDOWN KEY_F9
/* ChromeOS keyboard is using F10 rather than KEY_VOLUMEUP */
#define KEY_CROS_VOLUP KEY_F10
/* ChromeOS keyboard is using F8 rather than KEY_MICMUTE */
#define KEY_CROS_MUTE KEY_F8

/*
 * Linux kernel usbmon binary interface format
 * from Documentation/usb/usbmon.txt
 *
 * Only the 48 first bytes as we are using read() rather the IOCTL
 * which returns the full header.
 */
struct usbmon_packet {
	uint64_t id;		/*  0: URB ID - from submission to callback */
	unsigned char type;	/*  8: Same as text; extensible. */
	unsigned char xfer_type; /*    ISO (0), Intr, Control, Bulk (3) */
	unsigned char epnum;	/*     Endpoint number and transfer direction */
	unsigned char devnum;	/*     Device address */
	uint16_t busnum;	/* 12: Bus number */
	char flag_setup;	/* 14: Same as text */
	char flag_data;		/* 15: Same as text; Binary zero is OK. */
	int64_t ts_sec;		/* 16: gettimeofday */
	int32_t ts_usec;	/* 24: gettimeofday */
	int status;		/* 28: */
	unsigned int length;	/* 32: Length of data (submitted or actual) */
	unsigned int len_cap;	/* 36: Delivered length */
	union {			/* 40: */
		unsigned char setup[8];	/* Only for Control S-type */
		struct iso_rec {		/* Only for ISO */
			int error_count;
			int numdesc;
		} iso;
	} s;
};

/* Device node for the usbmon binary interface */
#define USBMON_DEV_FMT "/dev/usbmon%d"

/* device context */
struct jabra_dev {
	FILE *evdev;
	snd_hctl_t *hctl;
	snd_hctl_elem_t *playback_vol;
	int mon_fd;
	int devnum;
};

static void logd(int level, const char *fmt, ...)
		__attribute__((format (printf, 2, 3)));
static void logd(int level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsyslog(level, fmt, ap);
	va_end(ap);
}

static int alsa_touch_volume(struct jabra_dev *dev)
{
	snd_ctl_elem_value_t *elem;
	int err;

	snd_ctl_elem_value_alloca(&elem);
	err = snd_hctl_elem_read(dev->playback_vol, elem);
	if (err < 0)
		return err;
	err = snd_hctl_elem_write(dev->playback_vol, elem);
	if (err < 0)
		return err;
	logd(LOG_INFO, "Touch volume: %ld\n",
		snd_ctl_elem_value_get_integer(elem, 0));

	return 0;
}

static int alsa_init(struct jabra_dev *dev)
{
	int ret;
	snd_ctl_elem_id_t *id;
	int idx;
	char *card_name;
	char *hwid = NULL;
	time_t deadline = time(NULL) + 5 /* 5-second timeout */;

	while (time(NULL) < deadline) {
		/* find the Jabra device id */
		for (idx = -1; (snd_card_next(&idx) == 0) && (idx >= 0); ) {
			ret = snd_card_get_name(idx, &card_name);
			if (!ret && (strncmp(card_name, ALSA_DEV_NAME,
			     sizeof(ALSA_DEV_NAME) - 1) == 0)) {
				ret = asprintf(&hwid, "hw:%d", idx);
				logd(LOG_NOTICE, "ALSA dev %s (%s)\n",
							hwid, card_name);
				free(card_name);
				break;
			}
			if (!ret)
				free(card_name);
		}
		if (hwid)
			break;
		logd(LOG_INFO, "no ALSA device, retry later ...\n");
		/*
		 * card not found, loading the kernel module and initializing
		 * the driver might take some time, retry later.
		 */
		usleep(250000);
	}
	if (!hwid) {
		logd(LOG_ERR, "Cannot find ALSA device for Jabra\n");
		return -ENODEV;
	}

	/* initialize the Jabra USB-audio mixer control */
	ret = snd_hctl_open(&dev->hctl, hwid, 0);
	if (ret < 0)
		return -ENODEV;
	ret = snd_hctl_load(dev->hctl);
	if (ret < 0)
		goto fail_after_open;

	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, ALSA_CONTROL_NAME);
	dev->playback_vol = snd_hctl_find_elem(dev->hctl, id);
	if (!dev->playback_vol)
		goto fail_after_open;

	free(hwid);
	return 0;

fail_after_open:
	logd(LOG_ERR, "alsa initialization failed (%d)\n", ret);
	snd_hctl_close(dev->hctl);
	free(hwid);
	return ret;
}

static int hid_init(struct jabra_dev *dev)
{
	int ret;
	glob_t globbuf;

	globbuf.gl_offs = 0;
	ret = glob(INPUT_DEV_PATTERN, GLOB_NOESCAPE, NULL, &globbuf);
	if (ret) {
		logd(LOG_ERR, "Cannot find keyboard input device\n");
		return -ENODEV;
	}
	logd(LOG_NOTICE, "Using %s for HID event injection\n",
		globbuf.gl_pathv[0]);

	dev->evdev = fopen(globbuf.gl_pathv[0],"w");
	globfree(&globbuf);
	if (!dev->evdev) {
		logd(LOG_ERR, "Cannot open evdev for HID event injection\n");
		return errno;
	}
	return 0;
}

static void inject_hid_event(struct jabra_dev *dev, uint8_t key)
{
	struct input_event evt = { .type = EV_KEY, .code = key, .value = 1 };
	struct input_event syn = { .type = EV_SYN, .code = SYN_REPORT };
	int ret;

	ret = gettimeofday(&evt.time, NULL);
	if (ret)
		logd(LOG_WARNING, "Cannot get time\n");
	/* key press event */
	ret = fwrite(&evt, 1, sizeof(evt), dev->evdev);
	if (ret < 0)
		logd(LOG_WARNING, "Cannot inject HID event\n");
	/* key release event */
	evt.value = 0;
	ret = fwrite(&evt, 1, sizeof(evt), dev->evdev);
	if (ret < 0)
		logd(LOG_WARNING, "Cannot inject HID event\n");
	syn.time = evt.time;
	ret = fwrite(&syn, 1, sizeof(syn), dev->evdev);
	if (ret < 0)
		logd(LOG_WARNING, "Cannot inject HID SYN report\n");
	fflush(dev->evdev);
}

static void handle_hid_event(struct jabra_dev *dev,
			     uint8_t report, uint8_t code)
{
	if (report == JABRA_HID_REPORT) { /* volume keys */
		switch(code)
		{
		case 0: /* volume key release */
			/* touch the volume on the USB Audio interface
			 * to ensure we receive the next one
			 */
			alsa_touch_volume(dev);
			break;
		case 1: /* volume- press */
			inject_hid_event(dev, KEY_CROS_VOLDOWN);
			break;
		case 2: /* volume+ press */
			inject_hid_event(dev, KEY_CROS_VOLUP);
			break;
		}
	} else if (report == JABRA_TELEPHONY_REPORT) { /* other keys */
		/*
		 * if 'code' has the MUTE_FLAG bit set, the user has pressed
		 * the mic mute button, but the UI is not handling mic mute key
		 * so far (only speaker mute), so we don't inject any HID event.
		 */
	} else {
		logd(LOG_NOTICE, "HID message %02x %02x\n", report, code);
	}
}

static int usbmon_poll(struct jabra_dev *dev)
{
	struct pkt {
		struct usbmon_packet hdr;
		uint8_t payload[64];
	} pkt;
	ssize_t rlen;

	while (1) {
		rlen = read(dev->mon_fd, &pkt, sizeof(pkt));
		if (rlen <= 0)
			return rlen;
		if ((pkt.hdr.devnum == dev->devnum) &&
			(pkt.hdr.epnum == JABRA_ENDPOINT) &&
			(pkt.hdr.type == 'C')) {

			if ((pkt.hdr.status == 0) && (pkt.hdr.len_cap == 3))
				handle_hid_event(dev, pkt.payload[0],
						      pkt.payload[1]);
			if (pkt.hdr.status == -EPROTO) {
				logd(LOG_NOTICE,
					"Comm failed : disconnected ?\n");
				return 0;
			}
		}
	}
}

static int usbmon_open(struct jabra_dev *dev, int busnum)
{
	char *node;
	int ret;

	ret = asprintf(&node, USBMON_DEV_FMT, busnum);
	if (ret < 0)
		return ret;

	dev->mon_fd = open(node, O_RDONLY);
	if (dev->mon_fd < 0) {
		dev->mon_fd = 0;
		ret = errno;
		logd(LOG_ERR, "Cannot open usbmon node %s (%d)\n",
				node, ret);
		goto fail_free;
	}
	logd(LOG_INFO,"Using %s bus %d device %d\n", node, busnum, dev->devnum);
	ret = 0;

fail_free:
	free(node);
	return ret;
}

static int use_jabra_device(int busnum, int devnum, int loglvl, int errlog)
{
	struct jabra_dev *dev;
	int ret;

	/* setup the syslog facility and honor UDEV log level if set */
	openlog(DAEMON_NAME, errlog ? LOG_PERROR : 0, LOG_DAEMON);
	setlogmask(LOG_UPTO(loglvl));

	dev = calloc(1, sizeof(struct jabra_dev));
	if (!dev)
		return -ENOMEM;
	dev->devnum = devnum;

	ret = hid_init(dev);
	if (ret)
		goto err;

	ret = alsa_init(dev);
	if (ret)
		goto err;

	ret = usbmon_open(dev, busnum);
	if (ret)
		goto err;

	usbmon_poll(dev);
err:
	if (dev->mon_fd)
		close(dev->mon_fd);
	if (dev->hctl)
		snd_hctl_close(dev->hctl);
	if (dev->evdev)
		fclose(dev->evdev);
	free(dev);
	closelog();
	return ret;
}

int main(int argc, char *argv[])
{
	int opt;
	char *busnum_str = getenv("BUSNUM");
	char *devnum_str = getenv("DEVNUM");
	char *loglevel_str = getenv("LOGLEVEL");
	int use_stderr = 0;
	int busnum, devnum;
	int loglevel = LOG_NOTICE;

	while ((opt = getopt(argc, argv, "b:d:n:")) != -1) {
		switch (opt) {
		case 'b':
			busnum_str = optarg;
			break;
		case 'd':
			loglevel_str = optarg;
			use_stderr = 1;
			break;
		case 'n':
			devnum_str = optarg;
			break;
		}
	}

	if (!busnum_str || !devnum_str) {
		fprintf(stderr, "USB bus/device not defined\n");
		return -ENODEV;
	}
	busnum = atoi(busnum_str);
	devnum = atoi(devnum_str);
	if (loglevel_str)
		loglevel = atoi(loglevel_str);

	return use_jabra_device(busnum, devnum, loglevel, use_stderr);
}
