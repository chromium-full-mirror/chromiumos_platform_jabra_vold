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
#include <dirent.h>
#include <time.h>

#include <alsa/asoundlib.h>
#include <linux/input.h>
#include <linux/uinput.h>

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
#define CARD_PREFIX "card"

#define KEY_CROS_VOLDOWN KEY_VOLUMEDOWN
#define KEY_CROS_VOLUP KEY_VOLUMEUP

/* Maximum number of supported Alsa devices for Jabra */
#define ALSA_DEV_COUNT_MAX 4

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
	int uinput_fd;
	int alsa_dev_count;
	snd_hctl_t *hctl[ALSA_DEV_COUNT_MAX];
	snd_hctl_elem_t *playback_vol[ALSA_DEV_COUNT_MAX];
	int mon_fd;
	int devnum;
	int alsa_card_idx;
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
	int err = 0, idx;

	snd_ctl_elem_value_alloca(&elem);
	for (idx = 0; idx < dev->alsa_dev_count; ++idx) {
		err = snd_hctl_elem_read(dev->playback_vol[idx], elem);
		if (err < 0)
			goto return_err;
		err = snd_hctl_elem_write(dev->playback_vol[idx], elem);
		if (err < 0)
			goto return_err;
		logd(LOG_INFO, "Touch volume: %ld\n",
			snd_ctl_elem_value_get_integer(elem, 0));
	}

return_err:
	return (err < 0) ? err : 0;
}

static int alsa_init(struct jabra_dev *dev)
{
	int ret;
	snd_ctl_elem_id_t *id;
	int idx;
	char *card_name;
	char *hwid[ALSA_DEV_COUNT_MAX] = { NULL, };
	time_t deadline = time(NULL) + 5 /* 5-second timeout */;

	/* allow a brief initial delay so that multiple ALSA devices can be found */
	usleep(500000);

	while (time(NULL) < deadline) {
		/* find the Jabra device id */
		for (idx = -1; (snd_card_next(&idx) == 0) && (idx >= 0); ) {
			if ((-1 != dev->alsa_card_idx) && (idx != dev->alsa_card_idx))
				continue;
			ret = snd_card_get_name(idx, &card_name);
			if (!ret && (strncmp(card_name, ALSA_DEV_NAME,
				sizeof(ALSA_DEV_NAME) - 1) == 0)) {
				ret = asprintf(&hwid[dev->alsa_dev_count], "hw:%d", idx);
				logd(LOG_NOTICE, "ALSA dev %s (%s)\n",
						hwid[dev->alsa_dev_count], card_name);
				free(card_name);
				if (dev->alsa_dev_count++ >= ALSA_DEV_COUNT_MAX) {
					logd(LOG_ERR, "Too many ALSA devices found for Jabra\n");
					break;
				}
			}
			if (!ret)
				free(card_name);
		}
		if (dev->alsa_dev_count)
			break;
		logd(LOG_INFO, "no ALSA device, retry later ...\n");
		/*
		 * card not found, loading the kernel module and initializing
		 * the driver might take some time, retry later.
		 */
		usleep(250000);
	}
	if (!dev->alsa_dev_count) {
		logd(LOG_ERR, "Cannot find ALSA device for Jabra\n");
		return -ENODEV;
	}

	/* initialize the Jabra USB-audio mixer control */
	for (idx = 0; idx < dev->alsa_dev_count; ++idx) {
		ret = snd_hctl_open(&dev->hctl[idx], hwid[idx], 0);
		if (ret < 0)
			return -ENODEV;
		ret = snd_hctl_load(dev->hctl[idx]);
		if (ret < 0)
			goto fail_after_open;

		snd_ctl_elem_id_alloca(&id);
		snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
		snd_ctl_elem_id_set_name(id, ALSA_CONTROL_NAME);
		dev->playback_vol[idx] = snd_hctl_find_elem(dev->hctl[idx], id);
		if (!dev->playback_vol[idx])
			goto fail_after_open;

		free(hwid[idx]);
	}
	return 0;

fail_after_open:
	logd(LOG_ERR, "alsa initialization failed (%d)\n", ret);
	for (idx = 0; idx < dev->alsa_dev_count; ++idx) {
		if (dev->hctl[idx]) {
			snd_hctl_close(dev->hctl[idx]);
			dev->hctl[idx] = NULL;
		}
		free(hwid[idx]);
	}
	return ret;
}

static int hid_init(struct jabra_dev *dev)
{
	int ret;
	size_t count;
	char* uidev_ptr;
	struct uinput_user_dev *uidev = calloc(1, sizeof(struct uinput_user_dev));

	time_t deadline = time(NULL) + 5 /* 5-second timeout */;

	if (!uidev) {
		logd(LOG_ERR, "Failed to allocate uinput_user_dev buffer");
		return -ENOMEM;
	}

	while (time(NULL) < deadline) {
		dev->uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
		if (dev->uinput_fd >= 0)
			break;
		logd(LOG_INFO, "uinput not started, retry later ...\n");
		/*
		 * uinput not started, retry later.
		 */
		usleep(250000);
	}


	if (dev->uinput_fd < 0) {
		logd(LOG_ERR, "Cannot open uinput for HID event injection\n");
		return -ENOMEM;
	}

	ioctl(dev->uinput_fd, UI_SET_EVBIT, EV_KEY);
	ioctl(dev->uinput_fd, UI_SET_EVBIT, EV_SYN);
	ioctl(dev->uinput_fd, UI_SET_KEYBIT, KEY_CROS_VOLDOWN);
	ioctl(dev->uinput_fd, UI_SET_KEYBIT, KEY_CROS_VOLUP);

	snprintf(uidev->name, UINPUT_MAX_NAME_SIZE, "jabra_vold input device");
	uidev->id.bustype = BUS_USB;
	uidev->id.version = 1;

	for (uidev_ptr = (char*)uidev, count = sizeof(*uidev); count; ) {
		int written = write(dev->uinput_fd, uidev_ptr, count);
		if (written < 0) {
			if (errno == EINTR || errno == EAGAIN) continue;
			logd(LOG_ERR, "Failed to configure input device\n");
			ret = errno;
			goto fail_exit;
		} else {
			uidev_ptr += written;
			count -= written;
		}
	}
	ret = ioctl(dev->uinput_fd, UI_DEV_CREATE);
	if (ret < 0) {
		logd(LOG_ERR, "Failed to create input device\n");
		goto fail_exit;
	}

	free(uidev);
	return 0;

fail_exit:
	close(dev->uinput_fd);
	dev->uinput_fd = 0;
	free(uidev);
	return ret;
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
	ret = write(dev->uinput_fd, &evt, sizeof(evt));
	if (ret < 0)
		logd(LOG_WARNING, "Cannot inject HID event\n");
	/* key release event */
	evt.value = 0;
	ret = write(dev->uinput_fd, &evt, sizeof(evt));
	if (ret < 0)
		logd(LOG_WARNING, "Cannot inject HID event\n");
	syn.time = evt.time;
	ret = write(dev->uinput_fd, &syn, sizeof(syn));
	if (ret < 0)
		logd(LOG_WARNING, "Cannot inject HID SYN report\n");
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

/* Search for the sound card index under /sys/devices/, the directory
 * path starts with the DEVPATH environment variable provided by udev,
 * and follow by the subdirectory to sound subsustem. Example:
 * /sys/devices/pci0000:00/0000:00:14.0/usb1/1-6/1-6:1.0/sound/
 */
static int find_card(char *devpath, int busnum, int devnum)
{
	char path[256];
	char *tokens, *tmp, *last;
	DIR * dir;
	struct dirent * ptr;
	int alsa_card_idx = -1;
	int opendir_retry = 5;

	/* Extract the last layer of directory from devpath. i.e
	 * '1-6' from /sys/devices/pci0000:00/0000:00:14.0/usb1/1-6
	 */
	last = NULL;
	tokens = strdup(devpath);
	tmp = strtok(tokens, "/");
	while (tmp) {
		last = tmp;
		tmp = strtok(NULL, "/");
	}
	snprintf(path, sizeof(path), "%s/%s:1.0/sound/", devpath, last);
	free(tokens);

usb_open_dir:
	dir = opendir(path);
	if (dir == NULL) {
		if (errno == ENOENT) {
			if (opendir_retry-- == 0) {
				logd(LOG_ERR, "Open %s timeout\n", path);
				return -EINVAL;
			}
			usleep(100000);
			goto usb_open_dir;
		}
		logd(LOG_ERR, "Open %s fail, %s\n", path, strerror(errno));
		return -errno;
	}

	/* Search for the file named 'cardX' where X is the sound card index
	 * given by ALSA framework. */
	while((ptr = readdir(dir)) != NULL) {
		if (strncmp(CARD_PREFIX, ptr->d_name, sizeof(CARD_PREFIX) - 1))
			continue;

		alsa_card_idx = atoi(ptr->d_name + sizeof(CARD_PREFIX) - 1);
		logd(LOG_INFO, "Successfully found card %d by parsing %s\n",
		     alsa_card_idx, path);
		break;
	}

	closedir(dir);
	return alsa_card_idx;
}

static int use_jabra_device(char *devpath, int busnum, int devnum, int loglvl, int errlog)
{
	struct jabra_dev *dev;
	int idx, ret;

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

	/* Look up the sound card index before alsa_init(). Set to -1 if
	 * card index cannot be found, in which case alsa_init() will
	 * keep all volume controls from all sound cards to use. */
	if (devpath) {
		ret = find_card(devpath, busnum, devnum);
		dev->alsa_card_idx = (ret < 0) ? -1 : ret;
	}

	ret = alsa_init(dev);
	if (ret)
		goto err;

	ret = usbmon_open(dev, busnum);
	if (ret)
		goto err;

	/* Set volume is required to receive HID packets afterwards. */
	alsa_touch_volume(dev);

	usbmon_poll(dev);
err:
	if (dev->mon_fd)
		close(dev->mon_fd);
	for (idx = 0; idx < dev->alsa_dev_count; ++idx) {
		if (dev->hctl[idx])
			snd_hctl_close(dev->hctl[idx]);
	}
	if (dev->uinput_fd) {
		ioctl(dev->uinput_fd, UI_DEV_DESTROY);
		close(dev->uinput_fd);
	}
	free(dev);
	closelog();
	return ret;
}

int main(int argc, char *argv[])
{
	int opt;
	char *action_str = "start";
	char *busnum_str = getenv("BUSNUM");
	char *devnum_str = getenv("DEVNUM");
	char *loglevel_str = getenv("LOGLEVEL");
	char *devpath = getenv("DEVPATH");
	char pid_filename[128];
	int use_stderr = 0;
	int busnum, devnum;
	int loglevel = LOG_NOTICE;
	int pid;
	int firmware_version = -1;
	int model = -1;

	while ((opt = getopt(argc, argv, "a:b:d:m:n:p:v:")) != -1) {
		switch (opt) {
		case 'a':
			action_str = optarg;
			break;
		case 'b':
			busnum_str = optarg;
			break;
		case 'd':
			loglevel_str = optarg;
			use_stderr = 1;
			break;
		case 'm':
			model = atoi(optarg);
			break;
		case 'n':
			devnum_str = optarg;
			break;
		case 'p':
			if (optarg)
				devpath = optarg;
			break;
		case 'v':
			firmware_version = atoi(optarg);
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
	snprintf(pid_filename, sizeof(pid_filename), "/run/jabra_vold/jabra_vold.%d.%d.pid",
			busnum, devnum);

	if (model < 0) {
		logd(LOG_INFO,
		     "No model info. Can't deduce if kernel HID is active\n");
	} else if (firmware_version < 0) {
		logd(LOG_INFO,
		     "No firmware version. Can't deduce if kernel HID is active\n");
	} else if (((model == 412) && firmware_version >= 111) ||
		   (model == 510 && firmware_version >= 214)) {
		logd(LOG_INFO,
		     "Jabra %d with firmware version %d has kernel HID, exiting...\n",
		     model, firmware_version);
		return 0;
	} else {
		logd(LOG_INFO,
		     "Jabra %d with firmware version %d does not have kernel HID",
		     model, firmware_version);
	}

	if (strcmp(action_str, "start") == 0) {
		if (daemon(0, 0)) {
			perror("Failed to daemonize");
			return errno;
		}

		FILE* pid_file = fopen(pid_filename, "r");
		if (pid_file) {
			int pid_to_kill;
			if (fscanf(pid_file, "%d", &pid_to_kill) > 0) {
				logd(LOG_INFO, "Stopping stale jabra_vold process %d\n", pid_to_kill);
				kill(pid_to_kill, SIGKILL);
			}
			fclose(pid_file);
		}
		pid_file = fopen(pid_filename, "we");
		if (!pid_file) {
			fprintf(stderr, "Failed to open PID file to write for jabra_vold %d:%d: %m\n",
				busnum, devnum);
			return errno;
		}
		pid = getpid();
		logd(LOG_INFO, "Started jabra_vold process %d.\n", pid);
		if (fprintf(pid_file, "%d\n", pid) <= 0) {
			int ret = errno;
			perror("Failed to write PID to the PID file.");
			fclose(pid_file);
			return ret;
		}
		fclose(pid_file);
	} else if (strcmp(action_str, "stop") == 0) {
		FILE* pid_file = fopen(pid_filename, "r");
		if (!pid_file) {
			logd(LOG_ERR, "Failed to open PID file to read for jabra_vold %d:%d\n",
					busnum, devnum);
			return -ENOMEM;
		}
		if (fscanf(pid_file, "%d", &pid) <= 0) {
			logd(LOG_ERR, "Failed to obtain PID for jabra_vold %d:%d\n",
					busnum, devnum);
			return -ENOMEM;
		}
		fclose(pid_file);
		remove(pid_filename);
		logd(LOG_INFO, "Stopping jabra_vold process %d\n", pid);
		kill(pid, SIGKILL);
		return 0;
	} else {
		fprintf(stderr, "Unrecognized action string '%s'\n", action_str);
		return -EINVAL;
	}

	return use_jabra_device(devpath, busnum, devnum, loglevel, use_stderr);
}
