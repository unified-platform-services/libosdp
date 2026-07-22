/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include "osdp_common.h"
#include "osdp_file.h"
#include "osdp_piv.h"
#include "osdp_bio.h"
#include "osdp_diag.h"
#include "osdp_metrics.h"

enum osdp_cp_error_e {
	OSDP_CP_ERR_NONE = 0,
	OSDP_CP_ERR_GENERIC = -1,
	OSDP_CP_ERR_NO_DATA = -2,
	OSDP_CP_ERR_RETRY_CMD = -3,
	OSDP_CP_ERR_DEFER = -4,
	OSDP_CP_ERR_INPROG = -5,
	OSDP_CP_ERR_UNKNOWN = -6,
	OSDP_CP_ERR_SEQ_NUM = -7,
	OSDP_CP_ERR_APP = -8, /* Application layer error */
};

static void cp_dispatch_event(struct osdp_pd *pd,
			      const struct osdp_event *event)
{
	struct osdp *ctx = pd_to_osdp(pd);

	if (ctx->event_callback) {
		ctx->event_callback(ctx->event_callback_arg, pd->idx,
				    (struct osdp_event *)event);
		osdp_metrics_report(pd, OSDP_METRIC_EVENT);
	}
}

static int cp_cmd_queue_init(struct osdp_pd *pd)
{
	queue_init(&pd->cmd_queue);
	return 0;
}

static inline void cp_cmd_free(struct osdp_pd *pd, const struct osdp_cmd *cmd)
{
	ARG_UNUSED(pd);
	ARG_UNUSED(cmd);
}

static int cp_cmd_enqueue(struct osdp_pd *pd, const struct osdp_cmd *cmd)
{
	queue_enqueue(&pd->cmd_queue, (queue_node_t *)&cmd->_node);
	return 0;
}

static int cp_cmd_dequeue(struct osdp_pd *pd, const struct osdp_cmd **cmd)
{
	queue_node_t *node;

	if (queue_dequeue(&pd->cmd_queue, &node))
		return -1;
	*cmd = CONTAINER_OF(node, struct osdp_cmd, _node);
	return 0;
}

static inline void cp_complete_cmd(struct osdp_pd *pd,
				   const struct osdp_cmd *cmd,
				   enum osdp_completion_status status)
{
	struct osdp *ctx = pd_to_osdp(pd);

	if (!cmd || !ctx->command_completion_callback)
		return;
	ctx->command_completion_callback(ctx->command_completion_callback_arg,
					 pd->idx, cmd, status);
}

static const char *cp_get_cap_name(int cap)
{
	if (cap <= OSDP_PD_CAP_UNUSED || cap >= OSDP_PD_CAP_SENTINEL) {
		return NULL;
	}
	const char *cap_name[] = {
		[OSDP_PD_CAP_CONTACT_STATUS_MONITORING] = "ContactStatusMonitoring",
		[OSDP_PD_CAP_OUTPUT_CONTROL] = "OutputControl",
		[OSDP_PD_CAP_CARD_DATA_FORMAT] = "CardDataFormat",
		[OSDP_PD_CAP_READER_LED_CONTROL] = "LEDControl",
		[OSDP_PD_CAP_READER_AUDIBLE_OUTPUT] = "AudibleControl",
		[OSDP_PD_CAP_READER_TEXT_OUTPUT] = "TextOutput",
		[OSDP_PD_CAP_TIME_KEEPING] = "TimeKeeping",
		[OSDP_PD_CAP_CHECK_CHARACTER_SUPPORT] = "CheckCharacter",
		[OSDP_PD_CAP_COMMUNICATION_SECURITY] = "CommunicationSecurity",
		[OSDP_PD_CAP_RECEIVE_BUFFERSIZE] = "ReceiveBufferSize",
		[OSDP_PD_CAP_LARGEST_COMBINED_MESSAGE_SIZE] = "CombinedMessageSize",
		[OSDP_PD_CAP_SMART_CARD_SUPPORT] = "SmartCard",
		[OSDP_PD_CAP_READERS] = "Reader",
		[OSDP_PD_CAP_BIOMETRICS] = "Biometric",
		[OSDP_PD_CAP_SECURE_PIN_ENTRY] = "SecurePinEntry",
		[OSDP_PD_CAP_OSDP_VERSION] = "OsdpVersion",
	};
	return cap_name[cap];
}

static void fill_local_keyset_cmd(struct osdp_pd *pd, struct osdp_cmd *cmd);

/**
 * Commands whose data block is filled in from an osdp_cmd that the app
 * queued. The rest are either self-contained or built from PD state.
 */
static bool cmd_needs_payload(int cmd_id)
{
	switch (cmd_id) {
	case CMD_OUT:
	case CMD_LED:
	case CMD_BUZ:
	case CMD_TEXT:
	case CMD_TDSET:
	case CMD_COMSET:
	case CMD_MFG:
	case CMD_BIOREAD:
	case CMD_BIOMATCH:
	case CMD_KEYSET:
		return true;
	default:
		return false;
	}
}

/**
 * Build the command in pd->cmd_id into buf. The command ID byte is written
 * here, ahead of the switch, so each case deals only with its data block and
 * max_len is the space that remains for it.
 *
 * Returns:
 * +ve: length of the command (ID byte inclusive)
 * -ve: error
 */
static int cp_build_command(struct osdp_pd *pd, const struct osdp_cmd *cmd,
			    uint8_t *buf, int max_len)
{
	int ret, len = 0;
	int data_off = osdp_phy_packet_get_data_offset(pd, buf);
	uint8_t *smb = osdp_phy_packet_get_smb(pd, buf);

	buf += data_off;
	max_len -= data_off;
	if (max_len <= 0) {
		return OSDP_CP_ERR_GENERIC;
	}

	if (!cmd && cmd_needs_payload(pd->cmd_id)) {
		LOG_ERR("No command payload to build CMD: %s(%02x)",
			osdp_cmd_name(pd->cmd_id), pd->cmd_id);
		return OSDP_CP_ERR_GENERIC;
	}

	buf[len++] = pd->cmd_id;
	max_len -= 1;

	switch (pd->cmd_id) {
	case CMD_POLL:
	case CMD_LSTAT:
	case CMD_ISTAT:
	case CMD_OSTAT:
	case CMD_RSTAT:
	case CMD_ABORT:
		/* These commands are just the ID byte; they carry no data */
		break;
	case CMD_ID:
		if (max_len < CMD_ID_DATA_LEN) {
			goto out_of_space;
		}
		buf[len++] = 0x00;
		break;
	case CMD_CAP:
		if (max_len < CMD_CAP_DATA_LEN) {
			goto out_of_space;
		}
		buf[len++] = 0x00;
		break;
	case CMD_OUT:
		if (max_len < CMD_OUT_DATA_LEN) {
			goto out_of_space;
		}
		buf[len++] = cmd->output.output_no;
		buf[len++] = cmd->output.control_code;
		bwrite_u16_le(cmd->output.timer_count, buf, &len);
		break;
	case CMD_LED:
		if (max_len < CMD_LED_DATA_LEN) {
			goto out_of_space;
		}
		buf[len++] = cmd->led.reader;
		buf[len++] = cmd->led.led_number;

		buf[len++] = cmd->led.temporary.control_code;
		buf[len++] = cmd->led.temporary.on_count;
		buf[len++] = cmd->led.temporary.off_count;
		buf[len++] = cmd->led.temporary.on_color;
		buf[len++] = cmd->led.temporary.off_color;
		bwrite_u16_le(cmd->led.temporary.timer_count, buf, &len);

		buf[len++] = cmd->led.permanent.control_code;
		buf[len++] = cmd->led.permanent.on_count;
		buf[len++] = cmd->led.permanent.off_count;
		buf[len++] = cmd->led.permanent.on_color;
		buf[len++] = cmd->led.permanent.off_color;
		break;
	case CMD_BUZ:
		if (max_len < CMD_BUZ_DATA_LEN) {
			goto out_of_space;
		}
		buf[len++] = cmd->buzzer.reader;
		buf[len++] = cmd->buzzer.control_code;
		buf[len++] = cmd->buzzer.on_count;
		buf[len++] = cmd->buzzer.off_count;
		buf[len++] = cmd->buzzer.rep_count;
		break;
	case CMD_TEXT:
		if (max_len < CMD_TEXT_DATA_LEN + cmd->text.length) {
			goto out_of_space;
		}
		buf[len++] = cmd->text.reader;
		buf[len++] = cmd->text.control_code;
		buf[len++] = cmd->text.temp_time;
		buf[len++] = cmd->text.offset_row;
		buf[len++] = cmd->text.offset_col;
		buf[len++] = cmd->text.length;
		memcpy(buf + len, cmd->text.data, cmd->text.length);
		len += cmd->text.length;
		break;
	case CMD_TDSET:
		if (max_len < CMD_TDSET_DATA_LEN) {
			goto out_of_space;
		}
		bwrite_u16_le(cmd->tdset.year, buf, &len);
		buf[len++] = cmd->tdset.month;
		buf[len++] = cmd->tdset.day;
		buf[len++] = cmd->tdset.hour;
		buf[len++] = cmd->tdset.minute;
		buf[len++] = cmd->tdset.second;
		break;
	case CMD_COMSET:
		if (max_len < CMD_COMSET_DATA_LEN) {
			goto out_of_space;
		}
		buf[len++] = cmd->comset.address;
		bwrite_u32_le(cmd->comset.baud_rate, buf, &len);
		break;
	case CMD_MFG:
		if (cmd->mfg.length > OSDP_CMD_MFG_MAX_DATALEN) {
			LOG_ERR("Invalid MFG data length (%d)", cmd->mfg.length);
			return OSDP_CP_ERR_GENERIC;
		}
		if (max_len < CMD_MFG_DATA_LEN + cmd->mfg.length) {
			goto out_of_space;
		}
		bwrite_u24_le(cmd->mfg.vendor_code, buf, &len);
		memcpy(buf + len, cmd->mfg.data, cmd->mfg.length);
		len += cmd->mfg.length;
		break;
	case CMD_BIOREAD:
		if (max_len < CMD_BIOREAD_DATA_LEN) {
			goto out_of_space;
		}
		buf[len++] = cmd->bioread.reader;
		buf[len++] = cmd->bioread.type;
		buf[len++] = cmd->bioread.format;
		buf[len++] = cmd->bioread.quality;
		break;
	case CMD_BIOMATCH:
		if (cmd->biomatch.length > OSDP_CMD_BIOMATCH_MAX_TEMPLATE_LEN) {
			LOG_ERR("Invalid BIOMATCH template length (%d)",
				cmd->biomatch.length);
			return OSDP_CP_ERR_GENERIC;
		}
		if (max_len < CMD_BIOMATCH_DATA_LEN + cmd->biomatch.length) {
			goto out_of_space;
		}
		buf[len++] = cmd->biomatch.reader;
		buf[len++] = cmd->biomatch.type;
		buf[len++] = cmd->biomatch.format;
		buf[len++] = cmd->biomatch.quality;
		bwrite_u16_le(cmd->biomatch.length, buf, &len);
		memcpy(buf + len, cmd->biomatch.data, cmd->biomatch.length);
		len += cmd->biomatch.length;
		break;
	case CMD_ACURXSIZE:
		if (max_len < CMD_ACURXSIZE_DATA_LEN) {
			goto out_of_space;
		}
		bwrite_u16_le(OSDP_PACKET_BUF_SIZE, buf, &len);
		break;
	case CMD_KEEPACTIVE:
		if (max_len < CMD_KEEPACTIVE_DATA_LEN) {
			goto out_of_space;
		}
		bwrite_u16_le(0, buf, &len);
		break;
	case CMD_FILETRANSFER:
		ret = osdp_file_cmd_tx_build(pd, buf + len, max_len);
		if (ret <= 0) {
			/* (Only) Abort file transfer on failures */
			buf[0] = CMD_ABORT;
			break;
		}
		len += ret;
		break;
	case CMD_PIVDATA:
	case CMD_GENAUTH:
	case CMD_CRAUTH:
		ret = osdp_piv_cp_cmd_build(pd, buf + len, max_len);
		if (ret <= 0) {
			osdp_piv_abort(pd);
			buf[0] = CMD_ABORT;
			break;
		}
		len += ret;
		break;
	case CMD_KEYSET:
		if (!sc_is_active(pd)) {
			LOG_ERR("Cannot perform KEYSET without SC!");
			return OSDP_CP_ERR_GENERIC;
		}
		if (cmd->keyset.length != 16) {
			LOG_ERR("Invalid key length");
			return OSDP_CP_ERR_GENERIC;
		}
		if (max_len < CMD_KEYSET_DATA_LEN) {
			goto out_of_space;
		}
		buf[len++] = 1;  /* key type (1: SCBK) */
		buf[len++] = 16; /* key length in bytes */
		if (cmd->keyset.type == 1) { /* SCBK */
			memcpy(buf + len, cmd->keyset.data, 16);
		} else if (cmd->keyset.type == 0) {  /* master_key */
			osdp_compute_scbk(pd, (uint8_t *)cmd->keyset.data, buf + len);
		} else {
			LOG_ERR("Unknown key type (%d)", cmd->keyset.type);
			return OSDP_CP_ERR_GENERIC;
		}
		len += 16;
		break;
	case CMD_CHLNG:
		if (smb == NULL) {
			LOG_ERR("Invalid secure message block!");
			return OSDP_CP_ERR_GENERIC;
		}
		if (max_len < CMD_CHLNG_DATA_LEN) {
			goto out_of_space;
		}
		smb[0] = 3;       /* length */
		smb[1] = SCS_11;  /* type */
		smb[2] = sc_use_scbkd(pd) ? 0 : 1;
		memcpy(buf + len, pd->sc.cp_random, 8);
		len += 8;
		break;
	case CMD_SCRYPT:
		if (smb == NULL) {
			LOG_ERR("Invalid secure message block!");
			return OSDP_CP_ERR_GENERIC;
		}
		if (max_len < CMD_SCRYPT_DATA_LEN) {
			goto out_of_space;
		}
		osdp_compute_cp_cryptogram(pd);
		smb[0] = 3;       /* length */
		smb[1] = SCS_13;  /* type */
		smb[2] = sc_use_scbkd(pd) ? 0 : 1;
		memcpy(buf + len, pd->sc.cp_cryptogram, 16);
		len += 16;
		break;
	default:
		LOG_ERR("Unknown/Unsupported CMD: %s(%02x)",
			osdp_cmd_name(pd->cmd_id), pd->cmd_id);
		return OSDP_CP_ERR_GENERIC;
	}

	if (smb && (smb[1] > SCS_14) && sc_is_active(pd)) {
		/**
		 * When SC active and current cmd is not a handshake (<= SCS_14)
		 * then we must set SCS type to 17 if this message has data
		 * bytes and 15 otherwise.
		 */
		smb[0] = 2;
		smb[1] = (len > 1) ? SCS_17 : SCS_15;
	}

	return len;

out_of_space:
	LOG_ERR("Out of buffer space to build CMD: %s(%02x); have:%d",
		osdp_cmd_name(pd->cmd_id), pd->cmd_id, max_len);
	return OSDP_CP_ERR_GENERIC;
}

/**
 * Did the PD claim CRC-16 support in its capabilities? A PD that did cannot be
 * telling us it speaks checksum when it NAKs a check character; it is telling us
 * the frame reached it corrupted.
 */
static bool cp_pd_declared_crc(struct osdp_pd *pd)
{
	const struct osdp_pd_cap *cap =
		&pd->cap[OSDP_PD_CAP_CHECK_CHARACTER_SUPPORT];

	return cap->function_code == OSDP_PD_CAP_CHECK_CHARACTER_SUPPORT &&
	       (cap->compliance_level & 0x01);
}

static int cp_decode_response(struct osdp_pd *pd, uint8_t *buf, int len)
{
	int t, ret = OSDP_CP_ERR_GENERIC, pos = 0;
	struct osdp_event event = {};
	struct osdp_event_mfgstat *mfgstat;

	/* Consume the reply ID byte; buf, pos and len now all describe the
	 * data block that follows it. */
	pd->reply_id = buf[0];
	buf += 1;
	len -= 1;

	switch (pd->reply_id) {
	case REPLY_ACK:
		if (len != 0) {
			break;
		}
		osdp_piv_cp_cmd_acked(pd);
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_NAK:
		if (len != REPLY_NAK_DATA_LEN) {
			break;
		}
		osdp_metrics_report(pd, OSDP_METRIC_NAK);
		pd->nak_code = buf[pos];
		if (pd->nak_code == OSDP_PD_NAK_MSG_CHK &&
		    ISSET_FLAG(pd, PD_FLAG_CP_USE_CRC)) {
			if (!cp_pd_declared_crc(pd)) {
				LOG_INF("PD NAK'd CRC-16, falling back to checksum");
				CLEAR_FLAG(pd, PD_FLAG_CP_USE_CRC);
				ret = OSDP_CP_ERR_RETRY_CMD;
				break;
			}
			/**
			 * The PD does CRC-16 and said so; downgrading the link
			 * would only make the next frame unreadable too. Take it
			 * at its word -- the frame was corrupted -- and resend.
			 */
			/* The retry funnel in cp_phy_state_update() counts
			 * this against the shared retry budget. */
			if (pd->phy_retry_count < OSDP_CMD_MAX_RETRIES) {
				LOG_WRN("PD NAK'd our check character for CMD: "
					"%s(%02x); resending (%d)",
					osdp_cmd_name(pd->cmd_id), pd->cmd_id,
					pd->phy_retry_count + 1);
				ret = OSDP_CP_ERR_RETRY_CMD;
				break;
			}
		}
		LOG_WRN("PD replied with NAK(%d) for CMD: %s(%02x)",
			pd->nak_code, osdp_cmd_name(pd->cmd_id), pd->cmd_id);
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_PDID:
		if (len != REPLY_PDID_DATA_LEN) {
			break;
		}
		pd->id.vendor_code  = bread_u24_le(buf, &pos);
		pd->id.model = buf[pos++];
		pd->id.version = buf[pos++];
		pd->id.serial_number = bread_u32_le(buf, &pos);
		pd->id.firmware_version = bread_u24_be(buf, &pos);
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_PDCAP:
		if ((len % REPLY_PDCAP_ENTITY_LEN) != 0) {
			LOG_ERR("PDCAP response length is not a multiple of 3");
			return OSDP_CP_ERR_GENERIC;
		}
		while (pos < len) {
			t = buf[pos++]; /* func_code */
			if (t >= OSDP_PD_CAP_SENTINEL) {
				break;
			}
			pd->cap[t].function_code = t;
			pd->cap[t].compliance_level = buf[pos++];
			pd->cap[t].num_items = buf[pos++];
			LOG_DBG("Reports capability '%s' (%d/%d)",
				cp_get_cap_name(pd->cap[t].function_code),
				pd->cap[t].compliance_level,
				pd->cap[t].num_items);
		}

		/* Get peer RX buffer size */
		t = OSDP_PD_CAP_RECEIVE_BUFFERSIZE;
		if ((int)pd->cap[t].function_code == t) {
			pd->peer_rx_size = pd->cap[t].compliance_level;
			pd->peer_rx_size |= (uint32_t)pd->cap[t].num_items << 8;
		}

		/* post-capabilities hooks */
		t = OSDP_PD_CAP_COMMUNICATION_SECURITY;
		if (pd->cap[t].compliance_level & 0x01) {
			SET_FLAG(pd, PD_FLAG_SC_CAPABLE);
		} else {
			CLEAR_FLAG(pd, PD_FLAG_SC_CAPABLE);
		}

		/* Check checksum/CRC support capability */
		t = OSDP_PD_CAP_CHECK_CHARACTER_SUPPORT;
		if ((int)pd->cap[t].function_code == t) {
			if (pd->cap[t].compliance_level & 0x01) {
				SET_FLAG(pd, PD_FLAG_CP_USE_CRC);
			} else {
				CLEAR_FLAG(pd, PD_FLAG_CP_USE_CRC);
			}
		}
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_OSTATR:
		t = OSDP_PD_CAP_OUTPUT_CONTROL;

		if (len != pd->cap[t].num_items ||
		    len > OSDP_STATUS_REPORT_MAX_LEN) {
			LOG_ERR("Invalid output status report length %d", len);
			return OSDP_CP_ERR_GENERIC;
		}
		event.type = OSDP_EVENT_STATUS;
		event.status.type = OSDP_STATUS_REPORT_OUTPUT;
		event.status.nr_entries = len;
		memcpy(event.status.report, buf + pos, len);
		cp_dispatch_event(pd, &event);
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_ISTATR:
		t = OSDP_PD_CAP_CONTACT_STATUS_MONITORING;

		if (len != pd->cap[t].num_items ||
		    len > OSDP_STATUS_REPORT_MAX_LEN) {
			LOG_ERR("Invalid input status report length %d", len);
			return OSDP_CP_ERR_GENERIC;
		}
		event.type = OSDP_EVENT_STATUS;
		event.status.type = OSDP_STATUS_REPORT_INPUT;
		event.status.nr_entries = len;
		memcpy(event.status.report, buf + pos, len);
		cp_dispatch_event(pd, &event);
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_LSTATR:
		if (len != REPLY_LSTATR_DATA_LEN) {
			break;
		}
		event.type = OSDP_EVENT_STATUS;
		event.status.type = OSDP_STATUS_REPORT_LOCAL;
		event.status.nr_entries = 2;
		event.status.report[0] = buf[pos++];
		event.status.report[1] = buf[pos++];
		cp_dispatch_event(pd, &event);
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_RSTATR:
		t = OSDP_PD_CAP_READERS;
		if (len != pd->cap[t].num_items ||
		    len > OSDP_STATUS_REPORT_MAX_LEN) {
			LOG_ERR("Invalid reader status report length %d", len);
			return OSDP_CP_ERR_GENERIC;
		}
		event.type = OSDP_EVENT_STATUS;
		event.status.type = OSDP_STATUS_REPORT_READER;
		event.status.nr_entries = len;
		memcpy(event.status.report, buf + pos, len);
		cp_dispatch_event(pd, &event);
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_COM:
		if (len != REPLY_COM_DATA_LEN) {
			break;
		}
		pd->address = buf[pos++];
		pd->baud_rate = bread_u32_le(buf, &pos);
		LOG_INF("COMSET responded with ID:%d Baud:%" PRIu32,
			pd->address, pd->baud_rate);
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_KEYPAD:
		if (len < REPLY_KEYPAD_DATA_LEN) {
			break;
		}
		event.type = OSDP_EVENT_KEYPRESS;
		event.keypress.reader_no = buf[pos++];
		event.keypress.length = buf[pos++];
		if (event.keypress.length != (len - REPLY_KEYPAD_DATA_LEN) ||
		    event.keypress.length > OSDP_EVENT_KEYPRESS_MAX_DATALEN) {
			break;
		}
		memcpy(event.keypress.data, buf + pos, event.keypress.length);
		cp_dispatch_event(pd, &event);
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_RAW:
		if (len < REPLY_RAW_DATA_LEN) {
			break;
		}
		event.type = OSDP_EVENT_CARDREAD;
		event.cardread.reader_no = buf[pos++];
		event.cardread.format = buf[pos++];
		event.cardread.length = bread_u16_le(buf, &pos);
		event.cardread.direction = 0; /* un-specified */
		t = BITS_TO_BYTES(event.cardread.length);
		if (t != (len - REPLY_RAW_DATA_LEN) ||
		    t > OSDP_EVENT_CARDREAD_MAX_DATALEN) {
			break;
		}
		memcpy(event.cardread.data, buf + pos, t);
		cp_dispatch_event(pd, &event);
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_FMT:
		/**
		 * osdp_FMT was underspecified by SIA from get-go. It was marked
		 * for deprecation in v2.2.2. To avoid confusions, we will just
		 * ignore it here.
		 *
		 * See: https://github.com/osdp-dev/libosdp/issues/206
		 */
		LOG_WRN("Ignoring deprecated response osdp_FMT");
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_BUSY:
		/* PD busy; signal upper layer to retry command */
		if (len != 0) {
			break;
		}
		ret = OSDP_CP_ERR_RETRY_CMD;
		break;
	case REPLY_PIVDATAR:
	case REPLY_GENAUTHR:
	case REPLY_CRAUTHR:
		t = osdp_piv_cp_reply_consume(pd, buf, len, &event);
		if (t < 0) {
			break;
		}
		if (t > 0) {
			cp_dispatch_event(pd, &event);
		}
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_MFGREP:
		if (len < REPLY_MFGREP_DATA_LEN) {
			break;
		}
		event.type = OSDP_EVENT_MFGREP;
		event.mfgrep.vendor_code = bread_u24_le(buf, &pos);
		event.mfgrep.length = len - REPLY_MFGREP_DATA_LEN;
		if (event.mfgrep.length > OSDP_EVENT_MFGREP_MAX_DATALEN) {
			break;
		}
		memcpy(event.mfgrep.data, buf + pos, event.mfgrep.length);
		cp_dispatch_event(pd, &event);
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_MFGSTATR:
	case REPLY_MFGERRR:
		/* These replies carry no vendor code; the payload is entirely
		 * vendor defined and may be empty.
		 */
		if (len > OSDP_EVENT_MFGSTAT_MAX_DATALEN) {
			break;
		}
		if (pd->reply_id == REPLY_MFGSTATR) {
			event.type = OSDP_EVENT_MFGSTATR;
			mfgstat = &event.mfgstatr;
		} else {
			event.type = OSDP_EVENT_MFGERRR;
			mfgstat = &event.mfgerrr;
		}
		mfgstat->length = len;
		memcpy(mfgstat->data, buf + pos, mfgstat->length);
		cp_dispatch_event(pd, &event);
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_BIOREADR:
		if (is_bioreadr_multipart(pd)) {
			t = osdp_bio_cp_reply_consume(pd, buf, len, &event);
			if (t < 0) {
				break;
			}
			if (t > 0) {
				cp_dispatch_event(pd, &event);
			}
			ret = OSDP_CP_ERR_NONE;
			break;
		}
		if (len < REPLY_BIOREADR_DATA_LEN) {
			break;
		}
		event.type = OSDP_EVENT_BIOREADR;
		event.bioreadr.reader = buf[pos++];
		event.bioreadr.status = buf[pos++];
		event.bioreadr.type = buf[pos++];
		event.bioreadr.quality = buf[pos++];
		event.bioreadr.length = bread_u16_le(buf, &pos);
		if (event.bioreadr.length != len - REPLY_BIOREADR_DATA_LEN ||
		    event.bioreadr.length > OSDP_EVENT_BIOREADR_MAX_TEMPLATE_LEN) {
			LOG_ERR("BIOREADR template length error (%d)",
				event.bioreadr.length);
			break;
		}
		memcpy(event.bioreadr.data, buf + pos, event.bioreadr.length);
		cp_dispatch_event(pd, &event);
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_BIOMATCHR:
		if (len != REPLY_BIOMATCHR_DATA_LEN) {
			break;
		}
		event.type = OSDP_EVENT_BIOMATCHR;
		event.biomatchr.reader = buf[pos++];
		event.biomatchr.status = buf[pos++];
		event.biomatchr.score = buf[pos++];
		cp_dispatch_event(pd, &event);
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_FTSTAT:
		ret = osdp_file_cmd_stat_decode(pd, buf + pos, len);
		break;
	case REPLY_CCRYPT:
		if (sc_is_active(pd) || pd->cmd_id != CMD_CHLNG) {
			LOG_EM("Out of order REPLY_CCRYPT; has PD gone rogue?");
			break;
		}
		if (len != REPLY_CCRYPT_DATA_LEN) {
			break;
		}
		memcpy(pd->sc.pd_client_uid, buf + pos, 8);
		memcpy(pd->sc.pd_random, buf + pos + 8, 8);
		memcpy(pd->sc.pd_cryptogram, buf + pos + 16, 16);
		pos += 32;
		osdp_compute_session_keys(pd);
		if (osdp_verify_pd_cryptogram(pd) != 0) {
			LOG_ERR("Failed to verify PD cryptogram");
			osdp_metrics_report(pd, OSDP_METRIC_SC_FAILURE);
			return OSDP_CP_ERR_APP;
		}
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_RMAC_I:
		if (sc_is_active(pd) || pd->cmd_id != CMD_SCRYPT) {
			LOG_EM("Out of order REPLY_RMAC_I; has PD gone rogue?");
			break;
		}
		if (len != REPLY_RMAC_I_DATA_LEN) {
			break;
		}
		memcpy(pd->sc.r_mac, buf + pos, 16);
		ret = OSDP_CP_ERR_NONE;
		break;
	default:
		LOG_WRN("Unknown reply %s(%02x)",
			osdp_reply_name(pd->reply_id), pd->reply_id);
		return OSDP_CP_ERR_UNKNOWN;
	}

	if (ret != OSDP_CP_ERR_NONE) {
		LOG_ERR("Failed to decode REPLY: %s(%02x) for CMD: %s(%02x)",
			osdp_reply_name(pd->reply_id), pd->reply_id,
			osdp_cmd_name(pd->cmd_id), pd->cmd_id);
	}

	if (pd->cmd_id != CMD_POLL ||
	    (pd->cmd_id == CMD_POLL && pd->reply_id != REPLY_ACK)) {
		LOG_DBG("CMD: %s(%02x) REPLY: %s(%02x)",
			osdp_cmd_name(pd->cmd_id), pd->cmd_id,
			osdp_reply_name(pd->reply_id), pd->reply_id);
	}

	return ret;
}

/* Build + finalize the outgoing command into the staging buffer. Leaves
 * the wire bytes parked in pd->packet_buf[0..packet_buf_len) ready for
 * osdp_phy_send_packet() to queue on the channel. Must run exactly once
 * per command — osdp_phy_finalize_packet() advances sc.c_mac and the
 * sequence number. */
static int cp_build_packet(struct osdp_pd *pd)
{
	int ret, packet_buf_size = get_tx_buf_size(pd);
	struct osdp_cmd local_keyset_cmd;
	const struct osdp_cmd *cmd = pd->active_cmd;
	uint8_t *buf = osdp_tx_staging_buf(pd);

	pd->packet_buf = buf;

	ret = osdp_phy_packet_init(pd, buf, packet_buf_size);
	if (ret < 0) {
		return OSDP_CP_ERR_GENERIC;
	}
	pd->packet_buf_len = ret;

	if (pd->state == OSDP_CP_STATE_SET_SCBK && pd->cmd_id == CMD_KEYSET) {
		memset(&local_keyset_cmd, 0, sizeof(local_keyset_cmd));
		fill_local_keyset_cmd(pd, &local_keyset_cmd);
		cmd = &local_keyset_cmd;
	}

	ret = cp_build_command(pd, cmd, buf, packet_buf_size);
	if (ret < 0) {
		return OSDP_CP_ERR_GENERIC;
	}
	pd->packet_buf_len += ret;

	ret = osdp_phy_finalize_packet(pd, buf, pd->packet_buf_len,
				       packet_buf_size);
	if (ret < 0) {
		return OSDP_CP_ERR_GENERIC;
	}
	pd->packet_buf_len = ret;
	return OSDP_CP_ERR_NONE;
}

static int cp_process_reply(struct osdp_pd *pd)
{
	uint8_t *buf;
	int err, len;

	err = osdp_phy_check_packet(pd);

	/* Translate phy error codes to CP errors */
	switch (err) {
	case OSDP_ERR_PKT_NONE:
		break;
	case OSDP_ERR_PKT_WAIT:
	case OSDP_ERR_PKT_NO_DATA:
		return OSDP_CP_ERR_NO_DATA;
	case OSDP_ERR_PKT_BUSY:
		/* Recorded so the retry-exhaustion path can tell a busy PD
		 * from a dead one; a BUSY never reaches the decoder. */
		pd->reply_id = REPLY_BUSY;
		return OSDP_CP_ERR_RETRY_CMD;
	case OSDP_ERR_PKT_NACK:
		if (pd->nak_code == OSDP_PD_NAK_SEQ_NUM) {
			LOG_WRN("NAK(SEQ_NUM); restarting communication");
			osdp_phy_state_reset(pd, true);
			sc_deactivate(pd);
			pd->state = OSDP_CP_STATE_INIT;
			return OSDP_CP_ERR_SEQ_NUM;
		}
		/* Other NACKs: CP cannot do anything about an invalid reply from a PD.
		 * Default to going offline and retrying after a while. The reason for
		 * this failure was probably better logged by lower layers.
		 */
		__fallthrough;
	default:
		return OSDP_CP_ERR_GENERIC;
	}

	/* Valid OSDP packet in buffer */
	len = osdp_phy_decode_packet(pd, &buf);
	if (len <= 0) {
		return OSDP_CP_ERR_GENERIC;
	}

	return cp_decode_response(pd, buf, len);
}

static inline bool cp_sc_should_retry(struct osdp_pd *pd)
{
	return (sc_is_capable(pd) && !sc_is_active(pd) &&
		osdp_millis_since(pd->sc_tstamp) > OSDP_PD_SC_RETRY_MS);
}

static int cp_translate_cmd(struct osdp_pd *pd, const struct osdp_cmd *cmd)
{
	switch (cmd->id) {
	case OSDP_CMD_OUTPUT: return CMD_OUT;
	case OSDP_CMD_LED:    return CMD_LED;
	case OSDP_CMD_BUZZER: return CMD_BUZ;
	case OSDP_CMD_TEXT:   return CMD_TEXT;
	case OSDP_CMD_TDSET:  return CMD_TDSET;
	case OSDP_CMD_COMSET: return CMD_COMSET;
	case OSDP_CMD_MFG:    return CMD_MFG;
	case OSDP_CMD_BIOREAD:  return CMD_BIOREAD;
	case OSDP_CMD_BIOMATCH: return CMD_BIOMATCH;
	case OSDP_CMD_STATUS:
		switch (cmd->status.type) {
		case OSDP_STATUS_REPORT_INPUT:  return CMD_ISTAT;
		case OSDP_STATUS_REPORT_OUTPUT: return CMD_OSTAT;
		case OSDP_STATUS_REPORT_LOCAL:  return CMD_LSTAT;
		case OSDP_STATUS_REPORT_READER: return CMD_RSTAT;
		default: return -1;
		}
	case OSDP_CMD_KEYSET:
		if (cmd->keyset.type != 1 || !sc_is_active(pd)) {
			return -1;
		} else {
			return CMD_KEYSET;
		}
	case OSDP_CMD_FILE_TX:
		/**
		 * This external command is handled as multiple command from
		 * osdp_file.c and it maintains it's own state. This means we
		 * should never reach here unless something is wrong.
		 */
		__fallthrough;
	default: BUG();
	}

	return -1;
}

static void fill_local_keyset_cmd(struct osdp_pd *pd, struct osdp_cmd *cmd)
{
	cmd->id = OSDP_CMD_KEYSET;
	cmd->keyset.type = 1;
	cmd->keyset.length = sizeof(pd->sc.scbk);
	memcpy(cmd->keyset.data, pd->sc.scbk, sizeof(pd->sc.scbk));
}

static inline bool cp_phy_bus_is_busy(struct osdp_pd *pd)
{
	return (pd->phy_state == OSDP_CP_PHY_STATE_SEND_CMD ||
		pd->phy_state == OSDP_CP_PHY_STATE_REPLY_WAIT);
}

static inline bool cp_phy_running(struct osdp_pd *pd)
{
	return cp_phy_bus_is_busy(pd) ||
	       pd->phy_state == OSDP_CP_PHY_STATE_WAIT ||
	       pd->phy_state == OSDP_CP_PHY_STATE_RETRY_CMD;
}

static inline bool cp_phy_kick(struct osdp_pd *pd)
{
	if (pd->phy_state == OSDP_CP_PHY_STATE_IDLE) {
		pd->phy_state = OSDP_CP_PHY_STATE_SEND_CMD;
		return true;
	}
	return false;
}

static void cp_phy_state_done(struct osdp_pd *pd)
{
	/* called when we have a valid response from the PD */
	if (sc_is_active(pd)) {
		pd->sc_tstamp = osdp_millis_now();
	}
	pd->phy_retry_count = 0;
	pd->phy_state = OSDP_CP_PHY_STATE_DONE;
}

static void cp_phy_state_wait(struct osdp_pd *pd, uint32_t wait_ms)
{
	pd->wait_ms = wait_ms;
	pd->phy_tstamp = osdp_millis_now();
	pd->phy_state = OSDP_CP_PHY_STATE_WAIT;
}

static uint32_t cp_calculate_transmit_time(struct osdp_pd* pd)
{
	return (pd->packet_buf_len * 10000U + pd->baud_rate - 1) / pd->baud_rate;
}

static int cp_phy_state_update(struct osdp_pd *pd)
{
	int rc, ret = OSDP_CP_ERR_DEFER;

	switch (pd->phy_state) {
	case OSDP_CP_PHY_STATE_DONE:
	case OSDP_CP_PHY_STATE_IDLE:
		ret = OSDP_CP_ERR_NONE;
		break;
	case OSDP_CP_PHY_STATE_ERR:
		ret = OSDP_CP_ERR_GENERIC;
		break;
	case OSDP_CP_PHY_STATE_WAIT:
		if (osdp_millis_since(pd->phy_tstamp) < pd->wait_ms) {
			return OSDP_CP_ERR_DEFER;
		}
		pd->phy_state = OSDP_CP_PHY_STATE_RETRY_CMD;
		return OSDP_CP_ERR_DEFER;
	case OSDP_CP_PHY_STATE_RETRY_CMD: {
		struct osdp_channel *channel = &pd_to_osdp(pd)->channel;

		/*
		 * A retry must repeat the command in its original form (v2.2
		 * sections 5.4 and 7.19): rewind the sequence number so the
		 * rebuild below goes out with the one the original carried.
		 * Rebuilding under SC yields the same bytes since the MAC
		 * chain only advances when a MAC'd reply is accepted.
		 */
		if (channel->flush)
			channel->flush(channel->data);
		pd->seq_number = pd->phy_tx_seq - 1;
		pd->phy_state = OSDP_CP_PHY_STATE_SEND_CMD;
	}
		__fallthrough;
	case OSDP_CP_PHY_STATE_SEND_CMD:
		if (cp_build_packet(pd)) {
			LOG_ERR("Failed to build packet for CMD: %s(%02x)",
				osdp_cmd_name(pd->cmd_id), pd->cmd_id);
			goto error;
		}
		pd->phy_state = OSDP_CP_PHY_STATE_SEND_CMD_WAIT;
		__fallthrough;
	case OSDP_CP_PHY_STATE_SEND_CMD_WAIT:
		rc = osdp_phy_send_packet(pd, pd->packet_buf,
					  pd->packet_buf_len);
		if (rc == OSDP_ERR_PKT_WAIT_TX) {
			/* Channel not ready; retry the same finalized bytes
			 * on the next refresh. */
			return OSDP_CP_ERR_DEFER;
		}
		if (rc < 0) {
			LOG_ERR("Failed to send packet for CMD: %s(%02x)",
				osdp_cmd_name(pd->cmd_id), pd->cmd_id);
			goto error;
		}
		osdp_metrics_report(pd, OSDP_METRIC_COMMAND);
		ret = OSDP_CP_ERR_INPROG;
		osdp_phy_state_reset(pd, false);
		pd->reply_id = REPLY_INVALID;
		pd->phy_state = OSDP_CP_PHY_STATE_REPLY_WAIT;
		pd->phy_tstamp = osdp_millis_now();
		pd->resp_timeout_ms = OSDP_RESP_TOUT_MS + cp_calculate_transmit_time(pd);
		break;
	case OSDP_CP_PHY_STATE_REPLY_WAIT:
		rc = cp_process_reply(pd);
		if (IS_ENABLED(OPT_OSDP_RX_ZERO_COPY)) {
			osdp_phy_release_packet(pd);
		}
		if (rc == OSDP_CP_ERR_NONE || rc == OSDP_CP_ERR_APP) {
			pd->tstamp = osdp_millis_now();
			osdp_phy_progress_sequence(pd);
		}
		if (rc == OSDP_CP_ERR_NONE ||
		    rc == OSDP_CP_ERR_SEQ_NUM ||
		    (rc == OSDP_CP_ERR_UNKNOWN && pd->cmd_id == CMD_POLL &&
		     is_ignore_unsolicited_messages(pd))) {
			cp_phy_state_done(pd);
			return OSDP_CP_ERR_NONE;
		}
		if (rc == OSDP_CP_ERR_GENERIC ||
		    rc == OSDP_CP_ERR_UNKNOWN ||
		    rc == OSDP_CP_ERR_APP) {
			goto error;
		}
		if (rc == OSDP_CP_ERR_RETRY_CMD) {
			if (pd->phy_retry_count >= OSDP_CMD_MAX_RETRIES) {
				LOG_ERR("Retry budget exhausted for CMD: "
					"%s(%02x); giving up",
					osdp_cmd_name(pd->cmd_id), pd->cmd_id);
				cp_phy_state_done(pd);
				return OSDP_CP_ERR_NONE;
			}
			pd->phy_retry_count += 1;
			cp_phy_state_wait(pd, OSDP_CMD_RETRY_WAIT_MS);
			return OSDP_CP_ERR_DEFER;
		}
		if (osdp_millis_since(pd->phy_tstamp) > pd->resp_timeout_ms) {
			if (pd->phy_retry_count < OSDP_CMD_MAX_RETRIES) {
				pd->phy_retry_count += 1;
				LOG_DBG("No response in %dms post-transmit; probing (%d)",
					OSDP_RESP_TOUT_MS,
					pd->phy_retry_count);
				cp_phy_state_wait(pd, OSDP_CMD_RETRY_WAIT_MS);
				return OSDP_CP_ERR_DEFER;
			}
			LOG_ERR("Response timeout for CMD: %s(%02x)",
				osdp_cmd_name(pd->cmd_id), pd->cmd_id);
			goto error;
		}
		ret = OSDP_CP_ERR_INPROG;
		break;
	}

	return ret;
error:
	pd->phy_state = OSDP_CP_PHY_STATE_ERR;
	return OSDP_CP_ERR_GENERIC;
}

static const char *state_get_name(enum osdp_cp_state_e state)
{
	switch (state) {
	case OSDP_CP_STATE_INIT:      return "ID-Request";
	case OSDP_CP_STATE_CAPDET:    return "Cap-Detect";
	case OSDP_CP_STATE_SC_CHLNG:  return "SC-Chlng";
	case OSDP_CP_STATE_SC_SCRYPT: return "SC-Scrypt";
	case OSDP_CP_STATE_SET_SCBK:  return "SC-SetSCBK";
	case OSDP_CP_STATE_ONLINE:    return "Online";
	case OSDP_CP_STATE_OFFLINE:   return "Offline";
	case OSDP_CP_STATE_DISABLED:  return "Disabled";
	default:
		BUG();
	}
}

/*
 * Feature-engine contract: each engine below (file/piv/bio) is a polled
 * command source with private state. To participate here an engine exports:
 *
 *   *_is_active(pd)      cheap liveness predicate; false when idle
 *   *_abort(pd)          idempotent teardown; fires MP START-before-DONE;
 *                        must be wired into osdp_engines_abort()
 *   *_cp_get_command(pd) >0 wire cmd to send now, 0 not mine (ask next),
 *                        -1 mine but hold the bus this tick
 *   *_pd_reply_pending(pd)  (optional, PD role) continuation to drain on POLL
 *
 * Priority is positional: app queue first, then engines in order, then the
 * paced keep-alive POLL. Engines are consulted *before* the POLL timer so an
 * active transfer streams at full refresh rate.
 */
static int cp_get_online_command(struct osdp_pd *pd)
{
	const struct osdp_cmd *cmd;
	int ret;

	if (cp_cmd_dequeue(pd, &cmd) == 0) {
		ret = cp_translate_cmd(pd, cmd);
		if (cmd->flags & OSDP_CMD_FLAG_BROADCAST) {
			SET_FLAG(pd, PD_FLAG_PKT_BROADCAST);
		}
		if (ret < 0) {
			cp_complete_cmd(pd, cmd, OSDP_COMPLETION_FAILED);
			pd->active_cmd = NULL;
		} else {
			pd->active_cmd = cmd;
		}
		cp_cmd_free(pd, cmd);
		return ret;
	}

	ret = osdp_file_tx_get_command(pd);
	if (ret != 0) {
		return ret;
	}

	ret = osdp_piv_cp_get_command(pd);
	if (ret != 0) {
		return ret;
	}

	ret = osdp_bio_cp_get_command(pd);
	if (ret != 0) {
		return ret;
	}

	if (osdp_millis_since(pd->tstamp) > OSDP_PD_POLL_TIMEOUT_MS) {
		pd->tstamp = osdp_millis_now();
		return CMD_POLL;
	}

	return -1;
}

static void notify_pd_status(struct osdp_pd *pd, bool is_online)
{
	struct osdp *ctx = pd_to_osdp(pd);
	struct osdp_event evt;

	if (!ctx->event_callback || !is_notifications_enabled(pd)) {
		return;
	}

	evt.type = OSDP_EVENT_NOTIFICATION;
	evt.notif.type = OSDP_NOTIFICATION_PD_STATUS;
	evt.notif.pd_status.online = is_online;
	ctx->event_callback(ctx->event_callback_arg, pd->idx, &evt);
	osdp_metrics_report(pd, OSDP_METRIC_EVENT);
}

static void notify_sc_status(struct osdp_pd *pd)
{
	struct osdp *ctx = pd_to_osdp(pd);
	struct osdp_event evt;

	if (!ctx->event_callback || !is_notifications_enabled(pd)) {
		return;
	}

	evt.type = OSDP_EVENT_NOTIFICATION;
	evt.notif.type = OSDP_NOTIFICATION_SC_STATUS;
	evt.notif.sc_status.active = sc_is_active(pd);
	evt.notif.sc_status.scbk_d = sc_use_scbkd(pd);
	ctx->event_callback(ctx->event_callback_arg, pd->idx, &evt);
	osdp_metrics_report(pd, OSDP_METRIC_EVENT);
}

static void cp_keyset_complete(struct osdp_pd *pd)
{
	const struct osdp_cmd *cmd = pd->active_cmd;

	if (!sc_use_scbkd(pd)) {
		if (!cmd || cmd->id != OSDP_CMD_KEYSET) {
			LOG_ERR("Missing active keyset command");
		} else {
			memcpy(pd->sc.scbk, cmd->keyset.data, 16);
		}
	} else {
		CLEAR_FLAG(pd, PD_FLAG_SC_USE_SCBKD);
	}
	sc_deactivate(pd);
	notify_sc_status(pd);
	if (pd->state == OSDP_CP_STATE_ONLINE) {
		make_request(pd, CP_REQ_RESTART_SC);
		LOG_INF("SCBK set; restarting SC to verify new SCBK");
	}
}

static bool cp_check_online_response(struct osdp_pd *pd)
{
	/* Always allow an ACK from the PD; Also, the most common case */
	if (pd->reply_id == REPLY_ACK) {
		if (pd->cmd_id == CMD_KEYSET) {
			/**
			 * When we received an ACK for keyset (either in current
			 * SC session or in plaintext after PD discarded the SC
			 * in favour of the new SCBK), we need to call to commit
			 * the new key pd->sc.scbk and restart the SC.
			 */
			cp_keyset_complete(pd);
		}
		return true;
	}

	/* A NAK or no response is always an error */
	if (pd->reply_id == REPLY_NAK || pd->reply_id == REPLY_INVALID) {
		return false;
	}

	/* Check for known poll responses */
	if (pd->cmd_id == CMD_POLL) {
		if (pd->reply_id == REPLY_LSTATR ||
		    pd->reply_id == REPLY_ISTATR ||
		    pd->reply_id == REPLY_OSTATR ||
		    pd->reply_id == REPLY_RSTATR ||
		    pd->reply_id == REPLY_MFGREP ||
		    pd->reply_id == REPLY_MFGSTATR ||
		    pd->reply_id == REPLY_MFGERRR ||
		    pd->reply_id == REPLY_BIOREADR ||
		    pd->reply_id == REPLY_BIOMATCHR ||
		    pd->reply_id == REPLY_PIVDATAR ||
		    pd->reply_id == REPLY_GENAUTHR ||
		    pd->reply_id == REPLY_CRAUTHR ||
		    pd->reply_id == REPLY_RAW ||
		    pd->reply_id == REPLY_KEYPAD) {
			return true;
		}
		return is_ignore_unsolicited_messages(pd);
	}

	/* Otherwise, we permit only expected responses */
	switch (pd->cmd_id) {
	case CMD_FILETRANSFER: return pd->reply_id == REPLY_FTSTAT;
	case CMD_COMSET:       return pd->reply_id == REPLY_COM;
	case CMD_MFG:
		/* REPLY_MFGERRR is a valid reply, but it reports that the
		 * vendor command failed; see the soft-failure handling in
		 * state_update(). */
		return pd->reply_id == REPLY_ACK ||
		       pd->reply_id == REPLY_MFGREP ||
		       pd->reply_id == REPLY_MFGSTATR;
	case CMD_BIOREAD:
		/* ACK when the app defers the scan; the reply then rides out on
		 * a later poll. */
		return pd->reply_id == REPLY_ACK ||
		       pd->reply_id == REPLY_BIOREADR;
	case CMD_BIOMATCH:
		return pd->reply_id == REPLY_ACK ||
		       pd->reply_id == REPLY_BIOMATCHR;
	case CMD_PIVDATA:
		/* ACK when the app defers; reply fragments then ride out on
		 * later polls. */
		return pd->reply_id == REPLY_ACK ||
		       pd->reply_id == REPLY_PIVDATAR;
	case CMD_GENAUTH:
	case CMD_CRAUTH:
		/* ACK per command fragment (and when the app defers); the
		 * first reply fragment may answer the final one inline. */
		return pd->reply_id == REPLY_ACK ||
		       (pd->cmd_id == CMD_GENAUTH ?
				pd->reply_id == REPLY_GENAUTHR :
				pd->reply_id == REPLY_CRAUTHR);
	case CMD_LSTAT:        return pd->reply_id == REPLY_LSTATR;
	case CMD_ISTAT:        return pd->reply_id == REPLY_ISTATR;
	case CMD_OSTAT:        return pd->reply_id == REPLY_OSTATR;
	case CMD_OUT:          return pd->reply_id == REPLY_OSTATR;
	case CMD_RSTAT:        return pd->reply_id == REPLY_RSTATR;
	default:
		LOG_ERR("Unexpected respose: CMD: %s(%02x) REPLY: %s(%02x)",
			osdp_cmd_name(pd->cmd_id), pd->cmd_id,
			osdp_reply_name(pd->reply_id), pd->reply_id);
		return false;
	}
}

static inline int state_get_cmd(struct osdp_pd *pd)
{
	enum osdp_cp_state_e state = pd->state;

	switch (state) {
	case OSDP_CP_STATE_INIT:      return CMD_ID;
	case OSDP_CP_STATE_CAPDET:    return CMD_CAP;
	case OSDP_CP_STATE_SC_CHLNG:  return CMD_CHLNG;
	case OSDP_CP_STATE_SC_SCRYPT: return CMD_SCRYPT;
	case OSDP_CP_STATE_SET_SCBK:  return CMD_KEYSET;
	case OSDP_CP_STATE_ONLINE:    return cp_get_online_command(pd);
	default: return -1;
	}
}

static inline bool state_check_reply(struct osdp_pd *pd)
{
	enum osdp_cp_state_e state = pd->state;

	switch (state) {
	case OSDP_CP_STATE_INIT:      return pd->reply_id == REPLY_PDID;
	case OSDP_CP_STATE_CAPDET:    return pd->reply_id == REPLY_PDCAP;
	case OSDP_CP_STATE_SC_CHLNG:  return pd->reply_id == REPLY_CCRYPT;
	case OSDP_CP_STATE_SC_SCRYPT: return pd->reply_id == REPLY_RMAC_I;
	case OSDP_CP_STATE_SET_SCBK:  return pd->reply_id == REPLY_ACK;
	case OSDP_CP_STATE_ONLINE:    return cp_check_online_response(pd);
	default: return false;
	}
}

static enum osdp_cp_state_e get_next_ok_state(struct osdp_pd *pd)
{
	enum osdp_cp_state_e state = pd->state;

	switch (state) {
	case OSDP_CP_STATE_INIT:
		return OSDP_CP_STATE_CAPDET;
	case OSDP_CP_STATE_CAPDET:
		if (sc_is_capable(pd)) {
			CLEAR_FLAG(pd, PD_FLAG_SC_USE_SCBKD);
			return OSDP_CP_STATE_SC_CHLNG;
		}
		if (is_enforce_secure(pd)) {
			LOG_INF("SC disabled/incapable; Set PD offline "
				"due to ENFORCE_SECURE");
			return OSDP_CP_STATE_OFFLINE;
		}
		return OSDP_CP_STATE_ONLINE;
	case OSDP_CP_STATE_SC_CHLNG:
		osdp_metrics_report(pd, OSDP_METRIC_SC_HANDSHAKE);
		return OSDP_CP_STATE_SC_SCRYPT;
	case OSDP_CP_STATE_SC_SCRYPT:
		sc_activate(pd);
		notify_sc_status(pd);
		if (sc_use_scbkd(pd)) {
			LOG_WRN("SC active with SCBK-D. Set SCBK");
			return OSDP_CP_STATE_SET_SCBK;
		}
		return OSDP_CP_STATE_ONLINE;
	case OSDP_CP_STATE_SET_SCBK:
		cp_keyset_complete(pd);
		return OSDP_CP_STATE_SC_CHLNG;
	case OSDP_CP_STATE_ONLINE:
		if (cp_sc_should_retry(pd)) {
			LOG_INF("Attempting to restart SC after %d seconds",
				OSDP_PD_SC_RETRY_MS/1000);
			return OSDP_CP_STATE_SC_CHLNG;
		}
		return OSDP_CP_STATE_ONLINE;
	case OSDP_CP_STATE_OFFLINE:
		if (osdp_millis_since(pd->tstamp) > pd->wait_ms) {
			return OSDP_CP_STATE_INIT;
		}
		return OSDP_CP_STATE_OFFLINE;
	case OSDP_CP_STATE_DISABLED:
		return OSDP_CP_STATE_DISABLED;
	default: BUG();
	}
}

static enum osdp_cp_state_e get_next_err_state(struct osdp_pd *pd)
{
	enum osdp_cp_state_e state = pd->state;

	switch (state) {
	case OSDP_CP_STATE_INIT:
		return OSDP_CP_STATE_OFFLINE;
	case OSDP_CP_STATE_CAPDET:
		return OSDP_CP_STATE_OFFLINE;
	case OSDP_CP_STATE_SC_CHLNG:
		if (is_enforce_secure(pd)) {
			LOG_ERR("CHLNG failed. Set PD offline due to "
				"ENFORCE_SECURE");
			return OSDP_CP_STATE_OFFLINE;
		}
		if (!sc_use_scbkd(pd)) {
			SET_FLAG(pd, PD_FLAG_SC_USE_SCBKD);
			LOG_WRN("SC Failed. Retry with SCBK-D");
			return OSDP_CP_STATE_SC_CHLNG;
		}
		CLEAR_FLAG(pd, PD_FLAG_SC_USE_SCBKD);
		/**
		 * SC setup failed; Update sc_tstamp so the next retry happens
		 * after OSDP_PD_SC_RETRY_MS.
		 */
		pd->sc_tstamp = osdp_millis_now();
		return OSDP_CP_STATE_ONLINE;
	case OSDP_CP_STATE_SC_SCRYPT:
		if (is_enforce_secure(pd)) {
			LOG_ERR("SCRYPT failed. Set PD offline due to "
				"ENFORCE_SECURE");
			return OSDP_CP_STATE_OFFLINE;
		}
		return OSDP_CP_STATE_ONLINE;
	case OSDP_CP_STATE_SET_SCBK:
		sc_deactivate(pd);
		notify_sc_status(pd);
		if (is_enforce_secure(pd) || sc_use_scbkd(pd)) {
			LOG_ERR("Failed to set SCBK; "
				"Set PD offline due to ENFORCE_SECURE");
			return OSDP_CP_STATE_OFFLINE;
		}
		return OSDP_CP_STATE_ONLINE;
	case OSDP_CP_STATE_ONLINE:
		return OSDP_CP_STATE_OFFLINE;
	case OSDP_CP_STATE_OFFLINE:
		return OSDP_CP_STATE_OFFLINE;
	case OSDP_CP_STATE_DISABLED:
		return OSDP_CP_STATE_DISABLED;
	default: BUG();
	}
}

static inline enum osdp_cp_state_e get_next_state(struct osdp_pd *pd, int err)
{
	return (err == 0) ? get_next_ok_state(pd) : get_next_err_state(pd);
}

static void cp_state_change(struct osdp_pd *pd, enum osdp_cp_state_e next)
{
	enum osdp_cp_state_e cur = pd->state;

	switch (next) {
	case OSDP_CP_STATE_INIT:
		osdp_phy_state_reset(pd, true);
		break;
	case OSDP_CP_STATE_ONLINE:
		LOG_INF("Online; %s SC", sc_is_active(pd) ? "With" : "Without");
		notify_pd_status(pd, true);
		break;
	case OSDP_CP_STATE_OFFLINE:
		pd->tstamp = osdp_millis_now();
		pd->wait_ms = OSDP_ONLINE_RETRY_WAIT_MAX_MS;
		sc_deactivate(pd);
		notify_sc_status(pd);
		LOG_ERR("Going offline for %" PRIu32
			" seconds; Was in '%s' state",
			pd->wait_ms / 1000, state_get_name(cur));
		osdp_engines_abort(pd);
		notify_pd_status(pd, false);
		break;
	case OSDP_CP_STATE_SC_CHLNG:
		osdp_phy_state_reset(pd, true);
		osdp_sc_setup(pd);
		break;
	case OSDP_CP_STATE_DISABLED:
		sc_deactivate(pd);
		notify_sc_status(pd);
		osdp_engines_abort(pd);
		notify_pd_status(pd, false);
		osdp_phy_state_reset(pd, true);
		LOG_INF("PD disabled; going offline until re-enabled");
		break;
	default: break;
	}

	LOG_DBG("StateChange: [%s] -> [%s] (SC-%s%s)",
		state_get_name(cur),
		state_get_name(next),
		sc_is_active(pd) ? "Active" : "Inactive",
		(sc_is_active(pd) && sc_use_scbkd(pd)) ? " with SCBK-D" : ""
	);

	pd->state = next;
}

static bool cp_cmd_is_app_owned(int cmd_id)
{
	switch (cmd_id) {
	case CMD_OUT:
	case CMD_LED:
	case CMD_BUZ:
	case CMD_TEXT:
	case CMD_TDSET:
	case CMD_ISTAT:
	case CMD_OSTAT:
	case CMD_LSTAT:
	case CMD_RSTAT:
	case CMD_COMSET:
	case CMD_KEYSET:
	case CMD_MFG:
	case CMD_BIOREAD:
	case CMD_BIOMATCH:
	case CMD_PIVDATA:
	case CMD_GENAUTH:
	case CMD_CRAUTH:
	case CMD_FILETRANSFER:
		return true;
	default:
		/* CMD_POLL, CMD_ID, CMD_CAP, CMD_CHLNG and CMD_SCRYPT are ours;
		 * if the PD refuses them, the link is not usable. */
		return false;
	}
}

static bool cp_nak_code_is_app_level(uint8_t nak_code)
{
	switch (nak_code) {
	case OSDP_PD_NAK_CMD_LEN:
	case OSDP_PD_NAK_CMD_UNKNOWN:
	case OSDP_PD_NAK_BIO_TYPE:
	case OSDP_PD_NAK_BIO_FMT:
	case OSDP_PD_NAK_RECORD:
		return true;
	default:
		/* MSG_CHK, SEQ_NUM, SC_UNSUP and SC_COND describe the state of
		 * the link rather than the command; they need the state machine
		 * to reset the connection. */
		return false;
	}
}

/**
 * A command can fail without the link being at fault: the PD received the frame
 * and declined to act on it. Such failures are reported to the app and the PD
 * stays online. This can only happen for a command the app asked for, and only
 * when the PD answered us; a timeout or a malformed reply is always a hard
 * failure, as is an unexpected (if well formed) reply, which means the PD has
 * lost track of the exchange.
 */
static bool cp_cmd_failure_is_soft(struct osdp_pd *pd)
{
	if (pd->state != OSDP_CP_STATE_ONLINE ||
	    pd->phy_state != OSDP_CP_PHY_STATE_DONE ||
	    !cp_cmd_is_app_owned(pd->cmd_id)) {
		return false;
	}

	/* The vendor command failed, but the PD told us so in a reply of its
	 * own instead of a NAK. */
	if (pd->cmd_id == CMD_MFG && pd->reply_id == REPLY_MFGERRR) {
		return true;
	}

	/* The PD answered every retry with BUSY until the budget ran out:
	 * it is alive and coherent, just overloaded. Drop the command, not
	 * the link. */
	if (pd->reply_id == REPLY_BUSY) {
		return true;
	}

	return pd->reply_id == REPLY_NAK &&
	       cp_nak_code_is_app_level(pd->nak_code);
}

static void notify_command_status(struct osdp_pd *pd, int status)
{
	int app_cmd;
	struct osdp_event evt;
	struct osdp *ctx = pd_to_osdp(pd);

	if (!ctx->event_callback || !is_notifications_enabled(pd)) {
		return;
	}

	switch (pd->cmd_id) {
	case CMD_OUT:    app_cmd = OSDP_CMD_OUTPUT; break;
	case CMD_LED:    app_cmd = OSDP_CMD_LED;    break;
	case CMD_BUZ:    app_cmd = OSDP_CMD_BUZZER; break;
	case CMD_TEXT:   app_cmd = OSDP_CMD_TEXT;   break;
	case CMD_TDSET:  app_cmd = OSDP_CMD_TDSET;  break;
	case CMD_COMSET: app_cmd = OSDP_CMD_COMSET; break;
	case CMD_ISTAT:  app_cmd = OSDP_CMD_STATUS; break;
	case CMD_OSTAT:  app_cmd = OSDP_CMD_STATUS; break;
	case CMD_LSTAT:  app_cmd = OSDP_CMD_STATUS; break;
	case CMD_RSTAT:  app_cmd = OSDP_CMD_STATUS; break;
	case CMD_KEYSET: app_cmd = OSDP_CMD_KEYSET; break;
	case CMD_MFG:
		if (pd->reply_id == REPLY_MFGREP ||
		    pd->reply_id == REPLY_MFGSTATR) {
			/**
			* if we received a manufacturer-specific reply, there is
			* a dedicated event (OSDP_EVENT_MFGREP/OSDP_EVENT_MFGSTATR)
			* for it. So we can skip sending a notification event.
			*
			* REPLY_MFGERRR also has a dedicated event, but it reports
			* a failed command, so the notification is still sent.
			*/
			return;
		}
		app_cmd = OSDP_CMD_MFG;
		break;
	case CMD_BIOREAD:
	case CMD_BIOMATCH:
		if (pd->reply_id == REPLY_BIOREADR ||
		    pd->reply_id == REPLY_BIOMATCHR) {
			/* The dedicated event carries the scan result; skip the
			 * redundant notification. */
			return;
		}
		app_cmd = (pd->cmd_id == CMD_BIOREAD) ? OSDP_CMD_BIOREAD
						      : OSDP_CMD_BIOMATCH;
		break;
	default:
		return;
	}

	evt.type = OSDP_EVENT_NOTIFICATION;
	evt.notif.type = OSDP_NOTIFICATION_COMMAND;
	evt.notif.command.command = app_cmd;
	evt.notif.command.success = (status != 0);

	ctx->event_callback(ctx->event_callback_arg, pd->idx, &evt);
	osdp_metrics_report(pd, OSDP_METRIC_EVENT);
}

static int state_update(struct osdp_pd *pd)
{
	int err;
	bool status;
	enum osdp_cp_state_e next, cur = pd->state;

	if (cp_phy_running(pd)) {
		err = cp_phy_state_update(pd);
		if (err == OSDP_CP_ERR_INPROG || err == OSDP_CP_ERR_DEFER) {
			return err;
		}
	}

	err = OSDP_CP_ERR_NONE;
	switch (pd->phy_state) {
	case OSDP_CP_PHY_STATE_IDLE:
		pd->cmd_id = state_get_cmd(pd);
		if (pd->cmd_id > 0 && cp_phy_kick(pd)) {
			return OSDP_CP_ERR_DEFER;
		}
		break;
	case OSDP_CP_PHY_STATE_ERR:
		err = OSDP_CP_ERR_GENERIC;
		__fallthrough;
	case OSDP_CP_PHY_STATE_DONE:
		status = state_check_reply(pd);
		notify_command_status(pd, status);
		cp_complete_cmd(pd, pd->active_cmd,
				status ? OSDP_COMPLETION_OK : OSDP_COMPLETION_FAILED);
		pd->active_cmd = NULL;
		if (!status) {
			err = cp_cmd_failure_is_soft(pd) ? OSDP_CP_ERR_NONE
							 : OSDP_CP_ERR_GENERIC;
			if (err == OSDP_CP_ERR_NONE &&
			    pd->cmd_id == CMD_FILETRANSFER) {
				/* Going offline used to do this for us. */
				osdp_file_tx_abort(pd);
			}
			if (err == OSDP_CP_ERR_NONE &&
			    osdp_piv_owns_cmd(pd, pd->cmd_id)) {
				/* PD refused the smartcard command (NAK). */
				osdp_piv_abort(pd);
			}
		}
		osdp_phy_state_reset(pd, false);
		break;
	default:
		BUG();
	}

	next = get_next_state(pd, err);

	if (pd->state == OSDP_CP_STATE_ONLINE || next == OSDP_CP_STATE_ONLINE) {
		if (check_request(pd, CP_REQ_RESTART_SC)) {
			osdp_phy_state_reset(pd, true);
			next = OSDP_CP_STATE_SC_CHLNG;
		}
		if (check_request(pd, CP_REQ_OFFLINE)) {
			LOG_INF("Going offline due to request");
			next = OSDP_CP_STATE_OFFLINE;
		}
	}

	if (check_request(pd, CP_REQ_DISABLE)) {
		next = OSDP_CP_STATE_DISABLED;
	}

	if (check_request(pd, CP_REQ_ENABLE)) {
		next = OSDP_CP_STATE_INIT;
	}

	if (cur != next) {
		cp_state_change(pd, next);
	}
	return OSDP_CP_ERR_DEFER;
}

static int cp_submit_command(struct osdp_pd *pd, const struct osdp_cmd *cmd)
{
	const uint32_t all_flags = (
		OSDP_CMD_FLAG_BROADCAST
	);

	if (pd->state == OSDP_CP_STATE_DISABLED) {
		LOG_ERR("PD is disabled");
		return -1;
	}

	if (pd->state != OSDP_CP_STATE_ONLINE) {
		LOG_ERR("PD is not online");
		return -1;
	}

	if (cmd->flags & ~all_flags) {
		LOG_ERR("Invalid command flag");
		return -1;
	}

	if (cmd->flags & OSDP_CMD_FLAG_BROADCAST) {
		if (NUM_PD(pd->osdp_ctx) != 1) {
			LOG_ERR("Command broadcast is allowed only in single"
				" PD environments");
			return -1;
		}
		if (is_enforce_secure(pd)) {
			LOG_ERR("Cannot send command in broadcast mode"
				" due to ENFORCE_SECURE");
			return -1;
		}
	}

	int reader_no = osdp_cmd_reader_no(cmd);
	if (reader_no > pd->cap[OSDP_PD_CAP_READERS].num_items) {
		LOG_ERR("Command targets reader %d but PD advertises only %d",
			reader_no, pd->cap[OSDP_PD_CAP_READERS].num_items);
		return -1;
	}

	if (cmd->id == OSDP_CMD_FILE_TX) {
		return osdp_file_tx_command(pd, cmd->file_tx.id,
					    cmd->file_tx.flags);
	} else if (cmd->id == OSDP_CMD_PIVDATA ||
		   cmd->id == OSDP_CMD_GENAUTH ||
		   cmd->id == OSDP_CMD_CRAUTH) {
		/* Multi-exchange op driven from the refresh loop (like
		 * FILE_TX); it does not ride the command queue. */
		return osdp_piv_cp_submit(pd, cmd);
	} else if (cmd->id == OSDP_CMD_KEYSET &&
		   (cmd->keyset.type != 1 || !sc_is_active(pd))) {
		LOG_ERR("Invalid keyset request");
		return -1;
	}

	return cp_cmd_enqueue(pd, cmd);
}

static void cp_collect_init_flags(struct osdp_pd *pd, uint32_t flags)
{
	if (flags & OSDP_FLAG_ENFORCE_SECURE) {
		SET_FLAG(pd, PD_FLAG_ENFORCE_SECURE);
	}
	if (flags & OSDP_FLAG_INSTALL_MODE) {
		SET_FLAG(pd, PD_FLAG_INSTALL_MODE);
	}
	if (flags & OSDP_FLAG_IGN_UNSOLICITED) {
		SET_FLAG(pd, PD_FLAG_IGNORE_USR);
	}
	if (flags & OSDP_FLAG_ENABLE_NOTIFICATION) {
		SET_FLAG(pd, PD_FLAG_ENABLE_NOTIF);
	}
	if (flags & OSDP_FLAG_CAPTURE_PACKETS) {
		SET_FLAG(pd, PD_FLAG_CAPTURE_PKT);
	}
	if (flags & OSDP_FLAG_ALLOW_EMPTY_ENCRYPTED_DATA_BLOCK) {
		SET_FLAG(pd, PD_FLAG_ALLOW_EMPTY_EDB);
	}
	if (flags & OSDP_FLAG_BIOREADR_MULTIPART) {
		SET_FLAG(pd, PD_FLAG_BIOREADR_MP);
	}
}

static int cp_expand_pd_array(struct osdp *ctx, int num_pd,
			      struct osdp_pd **old_pd_array,
			      struct osdp_pd **new_pd_array,
			      int *old_num_pd)
{
	*old_num_pd = ctx->_num_pd;
	*old_pd_array = ctx->pd;
	*new_pd_array = cp_pd_array_alloc(*old_num_pd, num_pd);
	if (*new_pd_array == NULL) {
		LOG_PRINT("Failed to allocate new osdp_pd[] context");
		return -1;
	}

	ctx->pd = *new_pd_array;
	ctx->_num_pd = *old_num_pd + num_pd;
#ifndef OPT_OSDP_STATIC
	memcpy(*new_pd_array, *old_pd_array, sizeof(struct osdp_pd) * *old_num_pd);
#endif /* OPT_OSDP_STATIC */
	return 0;
}

static int cp_setup_pd_rx_storage(struct osdp *ctx, struct osdp_pd *pd, int pd_idx)
{
#ifdef OPT_OSDP_RX_ZERO_COPY
	if (!ctx->channel.recv_pkt || !ctx->channel.release_pkt) {
		LOG_ERR("recv_pkt/release_pkt cannot be NULL in OPT_OSDP_RX_ZERO_COPY");
		return -1;
	}

	pd->rx_pkt = cp_rx_pkt_alloc(pd_idx);
	if (!pd->rx_pkt) {
		LOG_ERR("Failed to allocate rx_pkt");
		return -1;
	}

#else /* OPT_OSDP_RX_ZERO_COPY */
	ARG_UNUSED(ctx);

	pd->rx_rb = cp_rx_rb_alloc(pd_idx);
	if (!pd->rx_rb) {
		LOG_ERR("Failed to allocate rx_rb");
		return -1;
	}

#endif /* OPT_OSDP_RX_ZERO_COPY */
	return 0;
}

static int cp_add_pd(struct osdp *ctx, int num_pd, const osdp_pd_info_t *info_list)
{
	int i, old_num_pd;
	struct osdp_pd *old_pd_array, *new_pd_array, *pd;
	const osdp_pd_info_t *info;

	assert(num_pd);
	assert(info_list);

	if (cp_expand_pd_array(ctx, num_pd, &old_pd_array,
			       &new_pd_array, &old_num_pd)) {
		return -1;
	}

	for (i = 0; i < num_pd; i++) {
		info = info_list + i;
		pd = osdp_to_pd(ctx, i + old_num_pd);
		pd->idx = i + old_num_pd;
		pd->osdp_ctx = ctx;
		pd->packet_buf = osdp_tx_staging_buf(pd);
		if (info->name) {
			strncpy(pd->name, info->name, OSDP_PD_NAME_MAXLEN - 1);
		} else {
			snprintf(pd->name, OSDP_PD_NAME_MAXLEN, "PD-%d", info->address);
		}
		pd->baud_rate = info->baud_rate;
		pd->address = info->address;
		pd->flags = 0;
		pd->seq_number = -1;
		cp_collect_init_flags(pd, info->flags);
		SET_FLAG(pd, PD_FLAG_SC_DISABLED);
		/* Default to CRC-16 until we know PD capabilities */
		SET_FLAG(pd, PD_FLAG_CP_USE_CRC);
		if (cp_setup_pd_rx_storage(ctx, pd, i + old_num_pd)) {
			goto error;
		}
		if (info->scbk != NULL) {
			memcpy(pd->sc.scbk, info->scbk, 16);
			CLEAR_FLAG(pd, PD_FLAG_SC_DISABLED);
		} else if (is_enforce_secure(pd)) {
			LOG_PRINT("SCBK must be passed for each PD when"
				  " ENFORCE_SECURE is requested.");
			goto error;
		}
		if (cp_cmd_queue_init(pd)) {
			goto error;
		}
		if (IS_ENABLED(OPT_OSDP_SKIP_MARK_BYTE)) {
			SET_FLAG(pd, PD_FLAG_PKT_SKIP_MARK);
		}

		if (is_capture_enabled(pd)) {
			osdp_packet_capture_init(pd);
		}
	}
	SET_CURRENT_PD(ctx, 0);

#ifndef OPT_OSDP_STATIC
	if (old_num_pd) {
		free(old_pd_array);
	}
#endif
	return 0;

error:
	ctx->pd = old_pd_array;
	ctx->_num_pd = old_num_pd;

#ifndef OPT_OSDP_STATIC
	free(new_pd_array);
#else
	memset(new_pd_array + old_num_pd, 0, sizeof(struct osdp_pd) * num_pd);
#endif
	return -1;
}

/* --- Exported Methods --- */

osdp_t *osdp_cp_setup(const struct osdp_channel *channel, int num_pd,
		      const osdp_pd_info_t *info)
{
	assert(channel);

	struct osdp *ctx = cp_ctx_alloc();
	if (ctx == NULL) {
		LOG_PRINT("Failed to allocate osdp context");
		return NULL;
	}

	input_check_init(ctx);

#ifndef OPT_OSDP_LOG_MINIMAL
	logger_get_default(&ctx->logger);
#endif
	memcpy(&ctx->channel, channel, sizeof(ctx->channel));

	if (num_pd && cp_add_pd(ctx, num_pd, info)) {
		LOG_PRINT("Failed to add PDs");
		goto error;
	}

	LOG_PRINT("CP Setup complete; LibOSDP-%s %s NumPDs:%d",
		  osdp_get_version(), osdp_get_source_info(),
		  NUM_PD(ctx));
	return ctx;
error:
	osdp_cp_teardown((osdp_t *)ctx);
	return NULL;
}

int osdp_cp_add_pd(osdp_t *ctx, int num_pd, const osdp_pd_info_t *info)
{
	input_check(ctx);
	struct osdp *cp_ctx = TO_OSDP(ctx);

	if (!num_pd || !info) {
		LOG_PRINT("num_pd must be > 0 and info cannot be NULL");
		return -1;
	}

	if (cp_add_pd(cp_ctx, num_pd, info)) {
		LOG_PRINT("Failed to add PDs");
		return -1;
	}

	LOG_PRINT("Added %d PDs; TotalPDs:%d", num_pd, cp_ctx->_num_pd);
	return 0;
}

void osdp_cp_teardown(osdp_t *ctx)
{
	input_check(ctx);
	int i;
	struct osdp_pd *pd;
	const struct osdp_cmd *cmd;
	struct osdp *cp_ctx = TO_OSDP(ctx);

	for (i = 0; i < cp_ctx->_num_pd; i++) {
		pd = osdp_to_pd(cp_ctx, i);
		while (cp_cmd_dequeue(pd, &cmd) == 0) {
			cp_complete_cmd(pd, cmd, OSDP_COMPLETION_ABORTED);
			cp_cmd_free(pd, cmd);
		}
		cp_complete_cmd(pd, pd->active_cmd, OSDP_COMPLETION_ABORTED);
		pd->active_cmd = NULL;
		if (is_capture_enabled(pd)) {
			osdp_packet_capture_finish(pd);
		}
		osdp_fill_zeros(&pd->sc, sizeof(struct osdp_secure_channel));

#ifndef OPT_OSDP_STATIC
		safe_free(pd->file);
		safe_free(pd->piv);
		safe_free(pd->bio);
#ifdef OPT_OSDP_RX_ZERO_COPY
		safe_free(pd->rx_pkt);
#else
		safe_free(pd->rx_rb);
#endif
#endif /* OPT_OSDP_STATIC */

	}

	if (cp_ctx->channel.close) {
		cp_ctx->channel.close(cp_ctx->channel.data);
	}

#ifndef OPT_OSDP_STATIC
	safe_free(cp_ctx->pd);
	safe_free(cp_ctx);
#endif
}

void osdp_cp_refresh(osdp_t *ctx)
{
	input_check(ctx);
	int next_pd_idx, refresh_count = 0;
	struct osdp_pd *pd;
	struct osdp *cp_ctx = TO_OSDP(ctx);

	if (cp_ctx->_num_pd == 0) {
		return;
	}
	if (cp_ctx->_current_pd == NULL) {
		SET_CURRENT_PD(cp_ctx, 0);
	}
	while (refresh_count < cp_ctx->_num_pd) {
		pd = cp_ctx->_current_pd;

		state_update(pd);

		/*
		 * On a shared multi-drop bus the CP must complete one PD's
		 * exchange before starting the next. Break while the channel
		 * is occupied with a send/reply/retry cycle.
		 */
		if (cp_phy_bus_is_busy(pd)) {
			break;
		}

		next_pd_idx = pd->idx + 1;
		if (next_pd_idx >= cp_ctx->_num_pd) {
			next_pd_idx = 0;
		}
		SET_CURRENT_PD(cp_ctx, next_pd_idx);
		refresh_count++;
	}
}

void osdp_cp_set_event_callback(osdp_t *ctx, cp_event_callback_t cb, void *arg)
{
	input_check(ctx);
	TO_OSDP(ctx)->event_callback = cb;
	TO_OSDP(ctx)->event_callback_arg = arg;
}

void osdp_cp_set_command_completion_callback(osdp_t *ctx,
					     cp_command_completion_callback_t cb,
					     void *arg)
{
	input_check(ctx);
	TO_OSDP(ctx)->command_completion_callback = cb;
	TO_OSDP(ctx)->command_completion_callback_arg = arg;
}

int osdp_cp_send_command(osdp_t *ctx, int pd_idx, const struct osdp_cmd *cmd)
{
	input_check(ctx, pd_idx);
	struct osdp_pd *pd = osdp_to_pd(ctx, pd_idx);

	return cp_submit_command(pd, cmd);
}

int osdp_cp_submit_command(osdp_t *ctx, int pd_idx, const struct osdp_cmd *cmd)
{
	input_check(ctx, pd_idx);
	struct osdp_pd *pd = osdp_to_pd(ctx, pd_idx);

	return cp_submit_command(pd, cmd);
}

int osdp_cp_flush_commands(osdp_t *ctx, int pd_idx)
{
	input_check(ctx, pd_idx);
	struct osdp_pd *pd = osdp_to_pd(ctx, pd_idx);
	const struct osdp_cmd *cmd;
	int count = 0;

	while (cp_cmd_dequeue(pd, &cmd) == 0) {
		cp_complete_cmd(pd, cmd, OSDP_COMPLETION_FLUSHED);
		cp_cmd_free(pd, cmd);
		count++;
	}
	return count;
}

int osdp_cp_get_pd_id(const osdp_t *ctx, int pd_idx, struct osdp_pd_id *id)
{
	input_check(ctx, pd_idx);
	struct osdp_pd *pd = osdp_to_pd(ctx, pd_idx);

	memcpy(id, &pd->id, sizeof(struct osdp_pd_id));
	return 0;
}

int osdp_cp_get_capability(const osdp_t *ctx, int pd_idx, struct osdp_pd_cap *cap)
{
	input_check(ctx, pd_idx);
	int fc;
	struct osdp_pd *pd = osdp_to_pd(ctx, pd_idx);

	fc = cap->function_code;
	if (fc <= OSDP_PD_CAP_UNUSED || fc >= OSDP_PD_CAP_SENTINEL) {
		return -1;
	}

	cap->compliance_level = pd->cap[fc].compliance_level;
	cap->num_items = pd->cap[fc].num_items;
	return 0;
}

int osdp_cp_modify_flag(osdp_t *ctx, int pd_idx, uint32_t flags, bool do_set)
{
	input_check(ctx, pd_idx);
	const uint32_t all_flags = (
		OSDP_FLAG_ENFORCE_SECURE |
		OSDP_FLAG_INSTALL_MODE |
		OSDP_FLAG_IGN_UNSOLICITED |
		OSDP_FLAG_ENABLE_NOTIFICATION |
		OSDP_FLAG_ALLOW_EMPTY_ENCRYPTED_DATA_BLOCK
	);
	struct osdp_pd *pd = osdp_to_pd(ctx, pd_idx);
	uint32_t pd_flags = 0;

	if (flags & ~all_flags) {
		return -1;
	}

	if (flags & OSDP_FLAG_ENFORCE_SECURE) {
		pd_flags |= PD_FLAG_ENFORCE_SECURE;
	}
	if (flags & OSDP_FLAG_INSTALL_MODE) {
		pd_flags |= PD_FLAG_INSTALL_MODE;
	}
	if (flags & OSDP_FLAG_IGN_UNSOLICITED) {
		pd_flags |= PD_FLAG_IGNORE_USR;
	}
	if (flags & OSDP_FLAG_ENABLE_NOTIFICATION) {
		pd_flags |= PD_FLAG_ENABLE_NOTIF;
	}
	if (flags & OSDP_FLAG_CAPTURE_PACKETS) {
		pd_flags |= PD_FLAG_CAPTURE_PKT;
	}
	if (flags & OSDP_FLAG_ALLOW_EMPTY_ENCRYPTED_DATA_BLOCK) {
		pd_flags |= PD_FLAG_ALLOW_EMPTY_EDB;
	}
	do_set ? SET_FLAG(pd, pd_flags) : CLEAR_FLAG(pd, pd_flags);
	return 0;
}

int osdp_cp_disable_pd(osdp_t *ctx, int pd_idx)
{
	input_check(ctx, pd_idx);
	struct osdp_pd *pd = osdp_to_pd(ctx, pd_idx);

	if (pd->state == OSDP_CP_STATE_DISABLED) {
		LOG_DBG("PD is already disabled");
		return -1;
	}

	if (test_request(pd, CP_REQ_DISABLE)) {
		LOG_DBG("PD disable request already pending");
		return -1;
	}

	make_request(pd, CP_REQ_DISABLE);
	return 0;
}

int osdp_cp_enable_pd(osdp_t *ctx, int pd_idx)
{
	input_check(ctx, pd_idx);
	struct osdp_pd *pd = osdp_to_pd(ctx, pd_idx);

	if (pd->state != OSDP_CP_STATE_DISABLED) {
		LOG_DBG("PD is already enabled");
		return -1;
	}

	if (test_request(pd, CP_REQ_ENABLE)) {
		LOG_DBG("PD enable request already pending");
		return -1;
	}

	make_request(pd, CP_REQ_ENABLE);
	return 0;
}

bool osdp_cp_is_pd_enabled(const osdp_t *ctx, int pd_idx)
{
	input_check(ctx, pd_idx);
	struct osdp_pd *pd = osdp_to_pd(ctx, pd_idx);

	return pd->state != OSDP_CP_STATE_DISABLED;
}

/* Export the CP command queue and state machine to the unit tests. */
OSDP_TEST_ALIAS(cp_cmd_enqueue);
OSDP_TEST_ALIAS(cp_phy_state_update);
OSDP_TEST_ALIAS(state_update);
OSDP_TEST_ALIAS(cp_cmd_failure_is_soft);

/* Export the CP frame codecs to out-of-module drivers (tests, embedders). */
OSDP_TEST_ALIAS(cp_build_command);
OSDP_TEST_ALIAS(cp_decode_response);

#ifdef UNIT_TESTING
const int CP_ERR_DEFER = OSDP_CP_ERR_DEFER;
const int CP_ERR_CAN_YIELD = OSDP_CP_ERR_DEFER;
const int CP_ERR_INPROG = OSDP_CP_ERR_INPROG;
#endif /* UNIT_TESTING */
