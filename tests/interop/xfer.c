/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Cross-version OSDP command/event transport interop harness.
 *
 * Uses ONLY the public libosdp API (osdp.h), so the same source compiles
 * against any two revisions and can be run cross-wise to prove the command
 * (CP->PD) and event (PD->CP) wire formats are byte-compatible between them.
 *
 *   xfer cp <serial_dev> <mode>   # CP: sends commands, verifies events
 *   xfer pd <serial_dev> <mode>   # PD: verifies commands, sends events
 *
 * <mode> is one of:
 *   plain    no secure channel
 *   secure   pre-shared SCBK on both ends + OSDP_FLAG_ENFORCE_SECURE
 *   install  OSDP_FLAG_INSTALL_MODE: CP holds the SCBK, PD is keyless and the
 *            channel is provisioned over the default key (SCBK-D)
 *
 * Both ends share a hardcoded expected sequence of commands and events (built
 * by build_commands()/build_events() below). Each side self-verifies every
 * item it receives against that sequence; any field divergence fails the run.
 *
 * In the two secure modes nothing flows until the secure-channel handshake
 * completes, so successful transport also proves the SC wire format is
 * compatible; the CP additionally asserts the SC status mask came up.
 *
 * Exit 0 iff every expected item was transported AND verified (and, in a
 * secure mode, SC came up); 1 on any mismatch, shortfall, or timeout.
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

#include <osdp.h>

#define NUM_CMDS   4
#define NUM_EVENTS 3
#define RUN_TIMEOUT_MS 60000
#define GRACE_MS       1500

#define BITS_TO_BYTES(n) (((n) + 7) / 8)

enum sc_mode { MODE_PLAIN, MODE_SECURE, MODE_INSTALL };

/* Shared secret for the secure modes; identical on both ends. */
static const uint8_t SCBK[16] = {
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};

/* Real monotonic clock (overrides the library's __weak stub). */
int64_t osdp_millis_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int g_fd = -1;             /* serial link fd */
static enum sc_mode g_mode;       /* plain / secure / install */

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

/* --- shared expected sequences (single source of truth for both ends) --- */

static void build_commands(struct osdp_cmd cmds[NUM_CMDS])
{
	memset(cmds, 0, sizeof(struct osdp_cmd) * NUM_CMDS);

	cmds[0].id = OSDP_CMD_OUTPUT;
	cmds[0].output.output_no = 2;
	cmds[0].output.control_code = OSDP_CMD_OUTPUT_CC_PERMANENT_ON;
	cmds[0].output.timer_count = 0;

	cmds[1].id = OSDP_CMD_LED;
	cmds[1].led.reader = 0;
	cmds[1].led.led_number = 1;
	cmds[1].led.temporary.control_code = OSDP_CMD_LED_TEMPORARY_CC_SET;
	cmds[1].led.temporary.on_count = 10;
	cmds[1].led.temporary.off_count = 20;
	cmds[1].led.temporary.on_color = OSDP_LED_COLOR_RED;
	cmds[1].led.temporary.off_color = OSDP_LED_COLOR_GREEN;
	cmds[1].led.temporary.timer_count = 300;
	cmds[1].led.permanent.control_code = OSDP_CMD_LED_PERMANENT_CC_SET;
	cmds[1].led.permanent.on_count = 5;
	cmds[1].led.permanent.off_count = 6;
	cmds[1].led.permanent.on_color = OSDP_LED_COLOR_BLUE;
	cmds[1].led.permanent.off_color = OSDP_LED_COLOR_AMBER;
	/* permanent block carries no timer on the wire; must stay 0 */
	cmds[1].led.permanent.timer_count = 0;

	cmds[2].id = OSDP_CMD_BUZZER;
	cmds[2].buzzer.reader = 0;
	cmds[2].buzzer.control_code = OSDP_CMD_BUZZER_CC_DEFAULT_TONE;
	cmds[2].buzzer.on_count = 5;
	cmds[2].buzzer.off_count = 4;
	cmds[2].buzzer.rep_count = 3;

	cmds[3].id = OSDP_CMD_MFG;
	cmds[3].mfg.vendor_code = 0x00a1b2c3;
	cmds[3].mfg.length = 6;
	memcpy(cmds[3].mfg.data, "\xde\xad\xbe\xef\x01\x02", 6);
}

static void build_events(struct osdp_event events[NUM_EVENTS])
{
	memset(events, 0, sizeof(struct osdp_event) * NUM_EVENTS);

	events[0].type = OSDP_EVENT_CARDREAD;
	events[0].cardread.reader_no = 0;
	events[0].cardread.format = OSDP_CARD_FMT_RAW_WIEGAND;
	events[0].cardread.direction = 0;
	events[0].cardread.length = 32; /* bits -> 4 bytes on the wire */
	memcpy(events[0].cardread.data, "\x11\x22\x33\x44", 4);

	events[1].type = OSDP_EVENT_KEYPRESS;
	events[1].keypress.reader_no = 0;
	events[1].keypress.length = 4;
	memcpy(events[1].keypress.data, "\x0a\x0b\x0c\x0d", 4);

	events[2].type = OSDP_EVENT_MFGREP;
	events[2].mfgrep.vendor_code = 0x00c0ffee;
	events[2].mfgrep.length = 5;
	memcpy(events[2].mfgrep.data, "\x05\x04\x03\x02\x01", 5);
}

/* --- comparators (compare only fields that cross the wire) --- */

static int cmd_equal(const struct osdp_cmd *a, const struct osdp_cmd *b)
{
	if (a->id != b->id) {
		return 0;
	}
	switch (a->id) {
	case OSDP_CMD_OUTPUT:
		return a->output.output_no == b->output.output_no &&
		       a->output.control_code == b->output.control_code &&
		       a->output.timer_count == b->output.timer_count;
	case OSDP_CMD_LED:
		return a->led.reader == b->led.reader &&
		       a->led.led_number == b->led.led_number &&
		       memcmp(&a->led.temporary, &b->led.temporary,
			      sizeof(a->led.temporary)) == 0 &&
		       memcmp(&a->led.permanent, &b->led.permanent,
			      sizeof(a->led.permanent)) == 0;
	case OSDP_CMD_BUZZER:
		return a->buzzer.reader == b->buzzer.reader &&
		       a->buzzer.control_code == b->buzzer.control_code &&
		       a->buzzer.on_count == b->buzzer.on_count &&
		       a->buzzer.off_count == b->buzzer.off_count &&
		       a->buzzer.rep_count == b->buzzer.rep_count;
	case OSDP_CMD_MFG:
		return a->mfg.vendor_code == b->mfg.vendor_code &&
		       a->mfg.length == b->mfg.length &&
		       memcmp(a->mfg.data, b->mfg.data, a->mfg.length) == 0;
	default:
		return 0;
	}
}

static int event_equal(const struct osdp_event *a, const struct osdp_event *b)
{
	if (a->type != b->type) {
		return 0;
	}
	switch (a->type) {
	case OSDP_EVENT_CARDREAD:
		return a->cardread.reader_no == b->cardread.reader_no &&
		       a->cardread.format == b->cardread.format &&
		       a->cardread.direction == b->cardread.direction &&
		       a->cardread.length == b->cardread.length &&
		       memcmp(a->cardread.data, b->cardread.data,
			      BITS_TO_BYTES(a->cardread.length)) == 0;
	case OSDP_EVENT_KEYPRESS:
		return a->keypress.reader_no == b->keypress.reader_no &&
		       a->keypress.length == b->keypress.length &&
		       memcmp(a->keypress.data, b->keypress.data,
			      a->keypress.length) == 0;
	case OSDP_EVENT_MFGREP:
		return a->mfgrep.vendor_code == b->mfgrep.vendor_code &&
		       a->mfgrep.length == b->mfgrep.length &&
		       memcmp(a->mfgrep.data, b->mfgrep.data,
			      a->mfgrep.length) == 0;
	default:
		return 0;
	}
}

/* --- PD role: verifies inbound commands, submits outbound events --- */

static struct osdp_cmd g_exp_cmds[NUM_CMDS];
static int g_rx_cmds;      /* commands received (verified in order) */
static int g_cmd_mismatch; /* set on first bad command */

static int pd_command_cb(void *arg, struct osdp_cmd *cmd)
{
	(void)arg;
	/*
	 * Install mode provisions the real key via a KEYSET once SC is up on
	 * SCBK-D; accept it but don't count it against the transport sequence.
	 */
	if (cmd && cmd->id == OSDP_CMD_KEYSET) {
		return 0;
	}
	if (g_rx_cmds < NUM_CMDS && cmd_equal(cmd, &g_exp_cmds[g_rx_cmds])) {
		g_rx_cmds++;
	} else {
		g_cmd_mismatch = 1;
		fprintf(stderr, "PD: command %d mismatch (id=%d)\n",
			g_rx_cmds, cmd ? (int)cmd->id : -1);
	}
	return 0; /* ACK regardless, so the exchange keeps flowing */
}

/*
 * Capabilities the CP's commands require to be accepted (not NAK'd):
 * OUTPUT for output_no 2, LED for led_number 1, AUDIBLE for the buzzer,
 * READERS so reader_no 0 validates, and COMMUNICATION_SECURITY for SC.
 */
static const struct osdp_pd_cap pd_caps[] = {
	{ OSDP_PD_CAP_OUTPUT_CONTROL, 1, 4 },
	{ OSDP_PD_CAP_READER_LED_CONTROL, 1, 2 },
	{ OSDP_PD_CAP_READER_AUDIBLE_OUTPUT, 1, 1 },
	{ OSDP_PD_CAP_READERS, 1, 1 },
	{ OSDP_PD_CAP_COMMUNICATION_SECURITY, 1, 1 },
	{ (uint8_t)-1, 0, 0 }, /* sentinel */
};

static int run_pd(void)
{
	struct osdp_channel chan = { .send = chan_send, .recv = chan_recv };
	struct osdp_event events[NUM_EVENTS];
	osdp_pd_info_t info = {
		.address = 101,
		.baud_rate = 115200,
		.flags = flags_for(),
		.id = {
			.version = 1,
			.model = 1,
			.vendor_code = 0xC0FFEE,
			.serial_number = 0x01020304,
			.firmware_version = 0x0A0B0C0D,
		},
		.cap = pd_caps,
		.scbk = scbk_for(0),
	};

	build_commands(g_exp_cmds);
	build_events(events);

	osdp_t *ctx = osdp_pd_setup(&chan, &info);
	if (!ctx) {
		fprintf(stderr, "pd setup failed\n");
		return 1;
	}
	osdp_pd_set_command_callback(ctx, pd_command_cb, NULL);

	int tx_ev = 0;
	int64_t start = osdp_millis_now();
	int64_t grace_start = 0;
	while (osdp_millis_now() - start < RUN_TIMEOUT_MS) {
		osdp_pd_refresh(ctx);
		if (tx_ev < NUM_EVENTS &&
		    osdp_pd_submit_event(ctx, &events[tx_ev]) == 0) {
			tx_ev++;
		}
		if (g_rx_cmds >= NUM_CMDS && tx_ev >= NUM_EVENTS) {
			if (grace_start == 0) {
				grace_start = osdp_millis_now();
			} else if (osdp_millis_now() - grace_start > GRACE_MS) {
				break;
			}
		}
		usleep(500);
	}
	osdp_pd_teardown(ctx);

	if (g_cmd_mismatch || g_rx_cmds != NUM_CMDS || tx_ev != NUM_EVENTS) {
		fprintf(stderr,
			"PD: FAIL (rx_cmds=%d/%d tx_ev=%d/%d mismatch=%d)\n",
			g_rx_cmds, NUM_CMDS, tx_ev, NUM_EVENTS, g_cmd_mismatch);
		return 1;
	}
	return 0;
}

/* --- CP role: submits outbound commands, verifies inbound events --- */

static struct osdp_event g_exp_events[NUM_EVENTS];
static int g_rx_events;    /* events received (verified in order) */
static int g_event_mismatch;

static int cp_event_cb(void *arg, int pd, struct osdp_event *ev)
{
	(void)arg;
	(void)pd;
	if (g_rx_events < NUM_EVENTS &&
	    event_equal(ev, &g_exp_events[g_rx_events])) {
		g_rx_events++;
	} else {
		g_event_mismatch = 1;
		fprintf(stderr, "CP: event %d mismatch (type=%d)\n",
			g_rx_events, ev ? (int)ev->type : -1);
	}
	return 0;
}

static int run_cp(void)
{
	struct osdp_channel chan = { .send = chan_send, .recv = chan_recv };
	struct osdp_cmd cmds[NUM_CMDS];
	osdp_pd_info_t info = {
		.address = 101,
		.baud_rate = 115200,
		.flags = flags_for(),
		.id = {
			.version = 1,
			.model = 1,
			.vendor_code = 0xC0FFEE,
			.serial_number = 0x01020304,
			.firmware_version = 0x0A0B0C0D,
		},
		.cap = pd_caps,
		.scbk = scbk_for(1),
	};

	build_commands(cmds);
	build_events(g_exp_events);

	osdp_t *ctx = osdp_cp_setup(&chan, 1, &info);
	if (!ctx) {
		fprintf(stderr, "cp setup failed\n");
		return 1;
	}
	osdp_cp_set_event_callback(ctx, cp_event_cb, NULL);

	int want_sc = (g_mode != MODE_PLAIN);
	int tx_cmd = 0;
	int sc_seen = 0;
	int64_t start = osdp_millis_now();
	int64_t grace_start = 0;
	while (osdp_millis_now() - start < RUN_TIMEOUT_MS) {
		osdp_cp_refresh(ctx);
		if (tx_cmd < NUM_CMDS &&
		    osdp_cp_submit_command(ctx, 0, &cmds[tx_cmd]) == 0) {
			tx_cmd++;
		}
		if (want_sc && !sc_seen) {
			uint8_t mask = 0;
			osdp_get_sc_status_mask(ctx, &mask);
			if (mask & 1) {
				sc_seen = 1;
			}
		}
		if (tx_cmd >= NUM_CMDS && g_rx_events >= NUM_EVENTS &&
		    (!want_sc || sc_seen)) {
			if (grace_start == 0) {
				grace_start = osdp_millis_now();
			} else if (osdp_millis_now() - grace_start > GRACE_MS) {
				break;
			}
		}
		usleep(500);
	}
	osdp_cp_teardown(ctx);

	if (g_event_mismatch || g_rx_events != NUM_EVENTS ||
	    tx_cmd != NUM_CMDS || (want_sc && !sc_seen)) {
		fprintf(stderr,
			"CP: FAIL (tx_cmd=%d/%d rx_ev=%d/%d mismatch=%d sc=%d/%d)\n",
			tx_cmd, NUM_CMDS, g_rx_events, NUM_EVENTS,
			g_event_mismatch, sc_seen, want_sc);
		return 1;
	}
	return 0;
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

int main(int argc, char **argv)
{
	if (argc != 4) {
		fprintf(stderr,
			"usage: %s cp|pd <serial_dev> plain|secure|install\n",
			argv[0]);
		return 2;
	}
	if (parse_mode(argv[3], &g_mode)) {
		fprintf(stderr, "mode must be plain, secure, or install\n");
		return 2;
	}
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
