/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Cross-version OSDP file-transfer interop harness.
 *
 * Uses ONLY the public libosdp API (osdp.h), so the same source compiles
 * against any two revisions and can be run cross-wise to prove the file
 * transfer wire format is byte-compatible between them.
 *
 *   ftx cp <serial_dev> <input_file> <mode>   # CP = sender: reads, sends
 *   ftx pd <serial_dev> <output_file> <mode>  # PD = receiver: writes
 *
 * <mode> is one of:
 *   plain    no secure channel; frames go on the wire in the clear
 *   secure   pre-shared SCBK on both ends + OSDP_FLAG_ENFORCE_SECURE
 *   install  OSDP_FLAG_INSTALL_MODE: CP holds the SCBK, PD is keyless and the
 *            channel is provisioned over the default key (SCBK-D)
 *
 * Exit 0 on a completed transfer (the file-op close() callback fired), 1 on
 * timeout/failure. A sha256 mismatch on the received file flags a wire-format
 * divergence; in the secure modes the transfer only completes if the secure
 * channel wire format is also compatible.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#include <osdp.h>

#define FILE_ID 42

/* Real monotonic clock (overrides the library's __weak stub). File transfer
 * needs real time for its delay/timeout handling. */
int64_t osdp_millis_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

enum sc_mode { MODE_PLAIN, MODE_SECURE, MODE_INSTALL };

/* Shared secret for the secure modes; identical on both ends. */
static const uint8_t SCBK[16] = {
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};

static volatile int g_done;
static int g_fd = -1;      /* serial link fd */
static int g_file_fd = -1; /* input/output file fd */
static const char *g_file_path;
static enum sc_mode g_mode; /* plain / secure / install */

/*
 * Per-role secure-channel setup for each mode. The CP always needs the SCBK
 * to enable SC at all (a NULL SCBK leaves SC disabled); in install mode the
 * PD is deliberately keyless so the channel is brought up over SCBK-D.
 */
static const uint8_t *scbk_for(int is_cp)
{
	switch (g_mode) {
	case MODE_SECURE:
		return SCBK;
	case MODE_INSTALL:
		return is_cp ? SCBK : NULL;
	default:
		return NULL;
	}
}

static uint32_t flags_for(void)
{
	switch (g_mode) {
	case MODE_SECURE:
		return OSDP_FLAG_ENFORCE_SECURE;
	case MODE_INSTALL:
		return OSDP_FLAG_INSTALL_MODE;
	default:
		return 0;
	}
}

static int parse_mode(const char *s, enum sc_mode *out)
{
	if (strcmp(s, "plain") == 0) {
		*out = MODE_PLAIN;
	} else if (strcmp(s, "secure") == 0) {
		*out = MODE_SECURE;
	} else if (strcmp(s, "install") == 0) {
		*out = MODE_INSTALL;
	} else {
		return -1;
	}
	return 0;
}

/* --- serial channel --- */

static int serial_open(const char *dev)
{
	struct termios tio;
	int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", dev, strerror(errno));
		return -1;
	}
	if (tcgetattr(fd, &tio) == 0) {
		cfmakeraw(&tio);
		cfsetispeed(&tio, B115200);
		cfsetospeed(&tio, B115200);
		tio.c_cc[VMIN] = 0;
		tio.c_cc[VTIME] = 0;
		tcsetattr(fd, TCSANOW, &tio);
	}
	return fd;
}

static int chan_send(void *data, uint8_t *buf, int len)
{
	(void)data;
	int off = 0;
	while (off < len) {
		int n = write(g_fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				continue;
			}
			return -1;
		}
		off += n;
	}
	return len;
}

static int chan_recv(void *data, uint8_t *buf, int len)
{
	(void)data;
	int n = read(g_fd, buf, len);
	if (n < 0) {
		return 0; /* EAGAIN/EINTR: nothing available right now */
	}
	return n;
}

/* --- file ops --- */

static int f_open(void *arg, int file_id, uint32_t *size)
{
	(void)arg;
	int is_cp = (*size == 0); /* sender reports size; receiver is told it */
	if (file_id != FILE_ID) {
		return -1;
	}
	if (is_cp) {
		struct stat st;
		g_file_fd = open(g_file_path, O_RDONLY);
		if (g_file_fd < 0 || fstat(g_file_fd, &st) < 0) {
			return -1;
		}
		*size = (uint32_t)st.st_size;
	} else {
		g_file_fd = open(g_file_path,
				 O_CREAT | O_WRONLY | O_TRUNC, 0644);
		if (g_file_fd < 0) {
			return -1;
		}
	}
	return 0;
}

static int f_read(void *arg, void *buf, uint32_t size, uint32_t offset)
{
	(void)arg;
	return (int)pread(g_file_fd, buf, size, offset);
}

static int f_write(void *arg, const void *buf, uint32_t size, uint32_t offset)
{
	(void)arg;
	return (int)pwrite(g_file_fd, buf, size, offset);
}

static int f_close(void *arg)
{
	(void)arg;
	if (g_file_fd >= 0) {
		close(g_file_fd);
		g_file_fd = -1;
	}
	g_done = 1;
	return 0;
}

static struct osdp_file_ops fops = {
	.arg = NULL,
	.open = f_open,
	.read = f_read,
	.write = f_write,
	.close = f_close,
};

/*
 * Accept any command. This harness only cares about the file transfer, but a
 * callback must be present for the PD to accept the install-mode KEYSET that
 * provisions the real SCBK once SC is up on SCBK-D.
 */
static int f_command(void *arg, struct osdp_cmd *cmd)
{
	(void)arg;
	(void)cmd;
	return 0;
}

/* --- roles --- */

static osdp_pd_info_t pd_info = {
	.address = 101,
	.baud_rate = 115200,
	.flags = 0,
	.id = {
		.version = 1,
		.model = 1,
		.vendor_code = 0xC0FFEE,
		.serial_number = 0x01020304,
		.firmware_version = 0x0A0B0C0D,
	},
	.cap = (struct osdp_pd_cap[]){
		/* Needed so the PD accepts CHLNG and brings up SC */
		{ OSDP_PD_CAP_COMMUNICATION_SECURITY, 1, 1 },
		{ (uint8_t)-1, 0, 0 }, /* sentinel */
	},
	.scbk = NULL, /* set per-role from the mode before setup */
};

static int run_cp(void)
{
	struct osdp_channel chan = { .send = chan_send, .recv = chan_recv };
	struct osdp_cmd cmd = { .id = OSDP_CMD_FILE_TX };
	cmd.file_tx.id = FILE_ID;
	cmd.file_tx.flags = 0;

	pd_info.flags = flags_for();
	pd_info.scbk = scbk_for(1);

	osdp_t *ctx = osdp_cp_setup(&chan, 1, &pd_info);
	if (!ctx) {
		fprintf(stderr, "cp setup failed\n");
		return 1;
	}
	if (osdp_file_register_ops(ctx, 0, &fops)) {
		fprintf(stderr, "cp file ops reg failed\n");
		return 1;
	}

	int want_sc = (g_mode != MODE_PLAIN);
	int sc_seen = 0;
	int submitted = 0;
	int64_t start = osdp_millis_now();
	while (!g_done && osdp_millis_now() - start < 60000) {
		osdp_cp_refresh(ctx);
		if (want_sc && !sc_seen) {
			uint8_t mask = 0;
			osdp_get_sc_status_mask(ctx, &mask);
			if (mask & 1) {
				sc_seen = 1;
			}
		}
		/* Hold the transfer until SC is up so it rides the secure
		 * channel (in install mode nothing enforces this for us). */
		if (!submitted && (!want_sc || sc_seen) &&
		    osdp_cp_submit_command(ctx, 0, &cmd) == 0) {
			submitted = 1;
		}
		usleep(500);
	}
	/* grace: flush the final FTSTAT exchange */
	int64_t g = osdp_millis_now();
	while (osdp_millis_now() - g < 200) {
		osdp_cp_refresh(ctx);
		usleep(500);
	}
	osdp_cp_teardown(ctx);
	if (want_sc && !sc_seen) {
		fprintf(stderr, "cp: secure channel never came up\n");
		return 1;
	}
	return g_done ? 0 : 1;
}

static int run_pd(void)
{
	struct osdp_channel chan = { .send = chan_send, .recv = chan_recv };

	pd_info.flags = flags_for();
	pd_info.scbk = scbk_for(0);

	osdp_t *ctx = osdp_pd_setup(&chan, &pd_info);
	if (!ctx) {
		fprintf(stderr, "pd setup failed\n");
		return 1;
	}
	osdp_pd_set_command_callback(ctx, f_command, NULL);
	if (osdp_file_register_ops(ctx, 0, &fops)) {
		fprintf(stderr, "pd file ops reg failed\n");
		return 1;
	}
	int64_t start = osdp_millis_now();
	while (!g_done && osdp_millis_now() - start < 60000) {
		osdp_pd_refresh(ctx);
		usleep(500);
	}
	int64_t g = osdp_millis_now();
	while (osdp_millis_now() - g < 200) {
		osdp_pd_refresh(ctx);
		usleep(500);
	}
	osdp_pd_teardown(ctx);
	return g_done ? 0 : 1;
}

int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr,
			"usage: %s cp|pd <serial_dev> <file> plain|secure|install\n",
			argv[0]);
		return 2;
	}
	if (parse_mode(argv[4], &g_mode)) {
		fprintf(stderr, "mode must be plain, secure, or install\n");
		return 2;
	}
	g_file_path = argv[3];
	g_fd = serial_open(argv[2]);
	if (g_fd < 0) {
		return 2;
	}
	osdp_logger_init("osdp", OSDP_LOG_INFO, NULL);

	int rc;
	if (strcmp(argv[1], "cp") == 0) {
		rc = run_cp();
	} else if (strcmp(argv[1], "pd") == 0) {
		rc = run_pd();
	} else {
		fprintf(stderr, "role must be cp or pd\n");
		rc = 2;
	}
	close(g_fd);
	return rc;
}
