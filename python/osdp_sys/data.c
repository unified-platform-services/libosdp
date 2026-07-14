/*
 * Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "module.h"

/*
 * Commands and events are described by a table of fields rather than by a pair
 * of hand written translators each. A field says where it lives in the C struct
 * and what it is called in the dict, and one engine walks the table in either
 * direction. Adding a command means adding a table; the conversion, the bounds
 * checks and the error paths come with it.
 */

enum pyosdp_field_kind {
	FIELD_U8,
	FIELD_U16,
	FIELD_U32,
	FIELD_INT,
	FIELD_BYTES, /* a buffer, with its length held in another field */
	FIELD_STR, /* UTF-8 into a buffer, with its length held likewise */
	FIELD_SUBDICT, /* a nested struct, marshalled as a nested dict */
};

struct pyosdp_field {
	const char *key;
	enum pyosdp_field_kind kind;
	size_t offset;

	/* FIELD_BYTES and FIELD_STR */
	size_t len_offset;
	enum pyosdp_field_kind len_kind;
	size_t max_len;
	bool allow_empty;
	bool optional;

	/* FIELD_SUBDICT */
	const struct pyosdp_field *fields;
	size_t present_offset; /* nested block is absent when this byte is 0 */
};

#define INT_FIELD(st, member, int_kind)                                        \
	{                                                                      \
		.key = #member,                                                \
		.kind = int_kind,                                              \
		.offset = offsetof(st, member),                                \
	}

#define U8_FIELD(st, member)   INT_FIELD(st, member, FIELD_U8)
#define U16_FIELD(st, member)  INT_FIELD(st, member, FIELD_U16)
#define U32_FIELD(st, member)  INT_FIELD(st, member, FIELD_U32)
#define INT_FIELD_(st, member) INT_FIELD(st, member, FIELD_INT)

#define END_OF_FIELDS { .key = NULL }

/* --- Field access --- */

static int field_get_int(const void *base, size_t offset,
			 enum pyosdp_field_kind kind)
{
	const void *p = (const uint8_t *)base + offset;

	switch (kind) {
	case FIELD_U8:
		return *(const uint8_t *)p;
	case FIELD_U16:
		return *(const uint16_t *)p;
	case FIELD_U32:
		return (int)*(const uint32_t *)p;
	case FIELD_INT:
		return *(const int *)p;
	default:
		return -1;
	}
}

static void field_set_int(void *base, size_t offset,
			  enum pyosdp_field_kind kind, int val)
{
	void *p = (uint8_t *)base + offset;

	switch (kind) {
	case FIELD_U8:
		*(uint8_t *)p = (uint8_t)val;
		break;
	case FIELD_U16:
		*(uint16_t *)p = (uint16_t)val;
		break;
	case FIELD_U32:
		*(uint32_t *)p = (uint32_t)val;
		break;
	case FIELD_INT:
		*(int *)p = val;
		break;
	default:
		break;
	}
}

/* --- The engine --- */

static int pyosdp_fields_to_dict(PyObject *obj, const void *base,
				 const struct pyosdp_field *fields);

static int field_to_dict(PyObject *obj, const void *base,
			 const struct pyosdp_field *f)
{
	const uint8_t *data = (const uint8_t *)base + f->offset;
	PyObject *child;
	char str[64];
	int len;

	switch (f->kind) {
	case FIELD_U8:
	case FIELD_U16:
	case FIELD_U32:
	case FIELD_INT:
		return pyosdp_dict_add_int(
			obj, f->key, field_get_int(base, f->offset, f->kind));
	case FIELD_BYTES:
		len = field_get_int(base, f->len_offset, f->len_kind);
		if (len < 0 || (size_t)len > f->max_len) {
			PyErr_Format(PyExc_ValueError, "'%s' has a bad length",
				     f->key);
			return -1;
		}
		return pyosdp_dict_add_bytes(obj, f->key, data, len);
	case FIELD_STR:
		len = field_get_int(base, f->len_offset, f->len_kind);
		if (len < 0 || (size_t)len > f->max_len ||
		    (size_t)len >= sizeof(str)) {
			PyErr_Format(PyExc_ValueError, "'%s' has a bad length",
				     f->key);
			return -1;
		}
		memcpy(str, data, len);
		str[len] = '\0';
		return pyosdp_dict_add_str(obj, f->key, str);
	case FIELD_SUBDICT:
		if (field_get_int(data, f->present_offset, FIELD_U8) == 0)
			return 0; /* this block does nothing; leave it out */
		child = PyDict_New();
		if (child == NULL)
			return -1;
		if (pyosdp_fields_to_dict(child, data, f->fields) ||
		    PyDict_SetItemString(obj, f->key, child)) {
			Py_DECREF(child);
			return -1;
		}
		Py_DECREF(child);
		return 0;
	}
	return -1;
}

static int pyosdp_fields_to_dict(PyObject *obj, const void *base,
				 const struct pyosdp_field *fields)
{
	const struct pyosdp_field *f;

	for (f = fields; f->key != NULL; f++) {
		if (field_to_dict(obj, base, f))
			return -1;
	}
	return 0;
}

static int pyosdp_fields_to_struct(void *base, PyObject *dict,
				   const struct pyosdp_field *fields);

static int field_to_struct(void *base, PyObject *dict,
			   const struct pyosdp_field *f)
{
	uint8_t *data = (uint8_t *)base + f->offset;
	PyObject *item;
	uint8_t *buf;
	char *str;
	size_t slen;
	int val, len;

	switch (f->kind) {
	case FIELD_U8:
	case FIELD_U16:
	case FIELD_U32:
	case FIELD_INT:
		if (pyosdp_dict_get_int(dict, f->key, &val))
			return -1;
		field_set_int(base, f->offset, f->kind, val);
		return 0;
	case FIELD_BYTES:
		item = PyDict_GetItemString(dict, f->key);
		if (item == NULL) {
			if (!f->optional) {
				PyErr_Format(
					PyExc_KeyError,
					"Key: '%s' of type: bytes expected",
					f->key);
				return -1;
			}
			field_set_int(base, f->len_offset, f->len_kind, 0);
			return 0;
		}
		if (pyosdp_parse_bytes(item, &buf, &len, f->allow_empty))
			return -1;
		if ((size_t)len > f->max_len) {
			PyErr_Format(PyExc_ValueError,
				     "'%s' is too long: %d bytes", f->key, len);
			return -1;
		}
		memcpy(data, buf, len);
		field_set_int(base, f->len_offset, f->len_kind, len);
		return 0;
	case FIELD_STR:
		if (pyosdp_dict_get_str(dict, f->key, &str))
			return -1;
		slen = strlen(str);
		if (slen > f->max_len) {
			PyErr_Format(PyExc_ValueError,
				     "'%s' is too long: %zu bytes", f->key,
				     slen);
			free(str);
			return -1;
		}
		memcpy(data, str, slen);
		free(str);
		field_set_int(base, f->len_offset, f->len_kind, (int)slen);
		return 0;
	case FIELD_SUBDICT:
		item = PyDict_GetItemString(dict, f->key);
		if (item == NULL)
			return 0; /* an absent block stays zeroed: no action */
		if (!PyDict_Check(item)) {
			PyErr_Format(PyExc_TypeError, "'%s' must be a dict",
				     f->key);
			return -1;
		}
		return pyosdp_fields_to_struct(data, item, f->fields);
	}
	return -1;
}

static int pyosdp_fields_to_struct(void *base, PyObject *dict,
				   const struct pyosdp_field *fields)
{
	const struct pyosdp_field *f;

	for (f = fields; f->key != NULL; f++) {
		if (field_to_struct(base, dict, f))
			return -1;
	}
	return 0;
}

/* ------------------------------- */
/*            COMMANDS             */
/* ------------------------------- */

static const struct pyosdp_field cmd_output_fields[] = {
	U8_FIELD(struct osdp_cmd_output, output_no),
	INT_FIELD_(struct osdp_cmd_output, control_code),
	U16_FIELD(struct osdp_cmd_output, timer_count),
	END_OF_FIELDS,
};

/*
 * A LED command carries two independent parameter blocks. A control code of
 * zero means "do nothing" for that block, which is what an absent nested dict
 * marshals to. Only the temporary block has a timer.
 */
static const struct pyosdp_field led_temporary_fields[] = {
	U8_FIELD(struct osdp_cmd_led_params, control_code),
	U8_FIELD(struct osdp_cmd_led_params, on_count),
	U8_FIELD(struct osdp_cmd_led_params, off_count),
	U8_FIELD(struct osdp_cmd_led_params, on_color),
	U8_FIELD(struct osdp_cmd_led_params, off_color),
	U16_FIELD(struct osdp_cmd_led_params, timer_count),
	END_OF_FIELDS,
};

static const struct pyosdp_field led_permanent_fields[] = {
	U8_FIELD(struct osdp_cmd_led_params, control_code),
	U8_FIELD(struct osdp_cmd_led_params, on_count),
	U8_FIELD(struct osdp_cmd_led_params, off_count),
	U8_FIELD(struct osdp_cmd_led_params, on_color),
	U8_FIELD(struct osdp_cmd_led_params, off_color),
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_led_fields[] = {
	U8_FIELD(struct osdp_cmd_led, reader),
	U8_FIELD(struct osdp_cmd_led, led_number),
	{
		.key = "temporary",
		.kind = FIELD_SUBDICT,
		.offset = offsetof(struct osdp_cmd_led, temporary),
		.fields = led_temporary_fields,
		.present_offset =
			offsetof(struct osdp_cmd_led_params, control_code),
	},
	{
		.key = "permanent",
		.kind = FIELD_SUBDICT,
		.offset = offsetof(struct osdp_cmd_led, permanent),
		.fields = led_permanent_fields,
		.present_offset =
			offsetof(struct osdp_cmd_led_params, control_code),
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_buzzer_fields[] = {
	U8_FIELD(struct osdp_cmd_buzzer, reader),
	INT_FIELD_(struct osdp_cmd_buzzer, control_code),
	U8_FIELD(struct osdp_cmd_buzzer, on_count),
	U8_FIELD(struct osdp_cmd_buzzer, off_count),
	U8_FIELD(struct osdp_cmd_buzzer, rep_count),
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_text_fields[] = {
	U8_FIELD(struct osdp_cmd_text, reader),
	INT_FIELD_(struct osdp_cmd_text, control_code),
	U8_FIELD(struct osdp_cmd_text, temp_time),
	U8_FIELD(struct osdp_cmd_text, offset_row),
	U8_FIELD(struct osdp_cmd_text, offset_col),
	{
		.key = "data",
		.kind = FIELD_STR,
		.offset = offsetof(struct osdp_cmd_text, data),
		.len_offset = offsetof(struct osdp_cmd_text, length),
		.len_kind = FIELD_U8,
		.max_len = OSDP_CMD_TEXT_MAX_LEN,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_keyset_fields[] = {
	U8_FIELD(struct osdp_cmd_keyset, type),
	{
		.key = "data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_cmd_keyset, data),
		.len_offset = offsetof(struct osdp_cmd_keyset, length),
		.len_kind = FIELD_U8,
		.max_len = OSDP_CMD_KEYSET_KEY_MAX_LEN,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_comset_fields[] = {
	U8_FIELD(struct osdp_cmd_comset, address),
	U32_FIELD(struct osdp_cmd_comset, baud_rate),
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_mfg_fields[] = {
	U32_FIELD(struct osdp_cmd_mfg, vendor_code),
	{
		.key = "data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_cmd_mfg, data),
		.len_offset = offsetof(struct osdp_cmd_mfg, length),
		.len_kind = FIELD_U8,
		.max_len = OSDP_CMD_MFG_MAX_DATALEN,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_bioread_fields[] = {
	U8_FIELD(struct osdp_cmd_bioread, reader),
	U8_FIELD(struct osdp_cmd_bioread, type),
	U8_FIELD(struct osdp_cmd_bioread, format),
	U8_FIELD(struct osdp_cmd_bioread, quality),
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_biomatch_fields[] = {
	U8_FIELD(struct osdp_cmd_biomatch, reader),
	U8_FIELD(struct osdp_cmd_biomatch, type),
	U8_FIELD(struct osdp_cmd_biomatch, format),
	U8_FIELD(struct osdp_cmd_biomatch, quality),
	{
		.key = "data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_cmd_biomatch, data),
		.len_offset = offsetof(struct osdp_cmd_biomatch, length),
		.len_kind = FIELD_U16,
		.max_len = OSDP_CMD_BIOMATCH_MAX_TEMPLATE_LEN,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_file_tx_fields[] = {
	INT_FIELD_(struct osdp_cmd_file_tx, id),
	U32_FIELD(struct osdp_cmd_file_tx, flags),
	END_OF_FIELDS,
};

/*
 * A CP sends a status command as a query and carries no report; a PD answers by
 * returning one with an entry per input. Hence the report is optional and may
 * legitimately be empty.
 */
static const struct pyosdp_field cmd_status_fields[] = {
	INT_FIELD_(struct osdp_status_report, type),
	{
		.key = "report",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_status_report, report),
		.len_offset = offsetof(struct osdp_status_report, nr_entries),
		.len_kind = FIELD_INT,
		.max_len = OSDP_STATUS_REPORT_MAX_LEN,
		.allow_empty = true,
		.optional = true,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field notification_fields[] = {
	INT_FIELD_(struct osdp_notification, type),
	INT_FIELD_(struct osdp_notification, arg0),
	INT_FIELD_(struct osdp_notification, arg1),
	END_OF_FIELDS,
};

/* ------------------------------- */
/*             EVENTS              */
/* ------------------------------- */

/*
 * Card data is a bit string for the raw formats, so its length is a bit count
 * and the bytes that carry it are a whole number of bytes wide. Every other
 * event says what it means in its field table; this one cannot.
 */
static int pyosdp_make_dict_event_cardread(PyObject *obj, const void *payload)
{
	const struct osdp_event_cardread *ev = payload;
	int nr_bytes = ev->length;

	if (pyosdp_dict_add_int(obj, "reader_no", ev->reader_no) ||
	    pyosdp_dict_add_int(obj, "format", ev->format) ||
	    pyosdp_dict_add_int(obj, "direction", ev->direction))
		return -1;

	if (ev->format == OSDP_CARD_FMT_RAW_UNSPECIFIED ||
	    ev->format == OSDP_CARD_FMT_RAW_WIEGAND) {
		if (pyosdp_dict_add_int(obj, "length", ev->length))
			return -1;
		nr_bytes = (ev->length + 7) / 8;
	}

	if (nr_bytes < 0 || nr_bytes > OSDP_EVENT_CARDREAD_MAX_DATALEN) {
		PyErr_SetString(PyExc_ValueError, "Card data has a bad length");
		return -1;
	}

	return pyosdp_dict_add_bytes(obj, "data", ev->data, nr_bytes);
}

static int pyosdp_make_struct_event_cardread(void *payload, PyObject *dict)
{
	struct osdp_event_cardread *ev = payload;
	int reader_no, format, direction, length, nr_bytes;
	uint8_t *data;

	if (pyosdp_dict_get_int(dict, "reader_no", &reader_no) ||
	    pyosdp_dict_get_int(dict, "format", &format) ||
	    pyosdp_dict_get_int(dict, "direction", &direction) ||
	    pyosdp_dict_get_bytes(dict, "data", &data, &nr_bytes))
		return -1;

	length = nr_bytes;
	if (format == OSDP_CARD_FMT_RAW_UNSPECIFIED ||
	    format == OSDP_CARD_FMT_RAW_WIEGAND) {
		if (pyosdp_dict_get_int(dict, "length", &length))
			return -1;
		if (length < 0 || (length + 7) / 8 > nr_bytes) {
			PyErr_Format(PyExc_ValueError,
				     "Card of %d bits needs more than %d bytes",
				     length, nr_bytes);
			return -1;
		}
		nr_bytes = (length + 7) / 8;
	}

	if (nr_bytes > OSDP_EVENT_CARDREAD_MAX_DATALEN) {
		PyErr_SetString(PyExc_ValueError, "Card data is too long");
		return -1;
	}

	ev->reader_no = reader_no;
	ev->format = format;
	ev->direction = direction;
	ev->length = (uint16_t)length;
	memcpy(ev->data, data, nr_bytes);
	return 0;
}

static const struct pyosdp_field event_keypress_fields[] = {
	INT_FIELD_(struct osdp_event_keypress, reader_no),
	{
		.key = "data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_event_keypress, data),
		.len_offset = offsetof(struct osdp_event_keypress, length),
		.len_kind = FIELD_INT,
		.max_len = OSDP_EVENT_KEYPRESS_MAX_DATALEN,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field event_mfgrep_fields[] = {
	U32_FIELD(struct osdp_event_mfgrep, vendor_code),
	{
		.key = "data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_event_mfgrep, data),
		.len_offset = offsetof(struct osdp_event_mfgrep, length),
		.len_kind = FIELD_U8,
		.max_len = OSDP_EVENT_MFGREP_MAX_DATALEN,
	},
	END_OF_FIELDS,
};

/* Backs both MFGSTATR and MFGERRR; either may carry an empty payload */
static const struct pyosdp_field event_mfgstat_fields[] = {
	{
		.key = "data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_event_mfgstat, data),
		.len_offset = offsetof(struct osdp_event_mfgstat, length),
		.len_kind = FIELD_U8,
		.max_len = OSDP_EVENT_MFGSTAT_MAX_DATALEN,
		.allow_empty = true,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field event_bioreadr_fields[] = {
	U8_FIELD(struct osdp_event_bioreadr, reader),
	U8_FIELD(struct osdp_event_bioreadr, status),
	U8_FIELD(struct osdp_event_bioreadr, type),
	U8_FIELD(struct osdp_event_bioreadr, quality),
	{
		/* A failed scan carries no template */
		.key = "data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_event_bioreadr, data),
		.len_offset = offsetof(struct osdp_event_bioreadr, length),
		.len_kind = FIELD_U16,
		.max_len = OSDP_EVENT_BIOREADR_MAX_TEMPLATE_LEN,
		.allow_empty = true,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field event_biomatchr_fields[] = {
	U8_FIELD(struct osdp_event_biomatchr, reader),
	U8_FIELD(struct osdp_event_biomatchr, status),
	U8_FIELD(struct osdp_event_biomatchr, score),
	END_OF_FIELDS,
};

static const struct pyosdp_field event_status_fields[] = {
	INT_FIELD_(struct osdp_status_report, type),
	{
		.key = "report",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_status_report, report),
		.len_offset = offsetof(struct osdp_status_report, nr_entries),
		.len_kind = FIELD_INT,
		.max_len = OSDP_STATUS_REPORT_MAX_LEN,
	},
	END_OF_FIELDS,
};

/* --- Translators --- */

struct pyosdp_translator {
	const struct pyosdp_field *fields;
	/* Set only when a payload's meaning cannot be stated as a field table */
	int (*to_dict)(PyObject *obj, const void *payload);
	int (*to_struct)(void *payload, PyObject *dict);
};

static const struct pyosdp_translator command_translator[OSDP_CMD_SENTINEL] = {
	[OSDP_CMD_OUTPUT] = { .fields = cmd_output_fields },
	[OSDP_CMD_LED] = { .fields = cmd_led_fields },
	[OSDP_CMD_BUZZER] = { .fields = cmd_buzzer_fields },
	[OSDP_CMD_TEXT] = { .fields = cmd_text_fields },
	[OSDP_CMD_KEYSET] = { .fields = cmd_keyset_fields },
	[OSDP_CMD_COMSET] = { .fields = cmd_comset_fields },
	[OSDP_CMD_COMSET_DONE] = { .fields = cmd_comset_fields },
	[OSDP_CMD_MFG] = { .fields = cmd_mfg_fields },
	[OSDP_CMD_BIOREAD] = { .fields = cmd_bioread_fields },
	[OSDP_CMD_BIOMATCH] = { .fields = cmd_biomatch_fields },
	[OSDP_CMD_FILE_TX] = { .fields = cmd_file_tx_fields },
	[OSDP_CMD_STATUS] = { .fields = cmd_status_fields },
	[OSDP_CMD_NOTIFICATION] = { .fields = notification_fields },
};

static const struct pyosdp_translator event_translator[OSDP_EVENT_SENTINEL] = {
	[OSDP_EVENT_CARDREAD] = {
		.to_dict = pyosdp_make_dict_event_cardread,
		.to_struct = pyosdp_make_struct_event_cardread,
	},
	[OSDP_EVENT_KEYPRESS] = { .fields = event_keypress_fields },
	[OSDP_EVENT_MFGREP] = { .fields = event_mfgrep_fields },
	[OSDP_EVENT_MFGSTATR] = { .fields = event_mfgstat_fields },
	[OSDP_EVENT_MFGERRR] = { .fields = event_mfgstat_fields },
	[OSDP_EVENT_BIOREADR] = { .fields = event_bioreadr_fields },
	[OSDP_EVENT_BIOMATCHR] = { .fields = event_biomatchr_fields },
	[OSDP_EVENT_STATUS] = { .fields = event_status_fields },
	[OSDP_EVENT_NOTIFICATION] = { .fields = notification_fields },
};

static int translate_to_dict(const struct pyosdp_translator *t, PyObject *obj,
			     const void *payload)
{
	if (t->fields)
		return pyosdp_fields_to_dict(obj, payload, t->fields);
	return t->to_dict(obj, payload);
}

static int translate_to_struct(const struct pyosdp_translator *t, void *payload,
			       PyObject *dict)
{
	if (t->fields)
		return pyosdp_fields_to_struct(payload, dict, t->fields);
	return t->to_struct(payload, dict);
}

/*
 * Every payload lives in the same anonymous union, so they all begin at the
 * same address and one pointer serves whichever the id selects.
 */
static void *cmd_payload(struct osdp_cmd *cmd)
{
	return &cmd->led;
}

static void *event_payload(struct osdp_event *event)
{
	return &event->cardread;
}

/* --- Exposed Methods --- */

int pyosdp_make_struct_cmd(struct osdp_cmd *cmd, PyObject *dict)
{
	int cmd_id;

	if (pyosdp_dict_get_int(dict, "command", &cmd_id))
		return -1;
	if (cmd_id <= 0 || cmd_id >= OSDP_CMD_SENTINEL) {
		PyErr_Format(PyExc_ValueError, "Unknown command: %d", cmd_id);
		return -1;
	}
	if (translate_to_struct(&command_translator[cmd_id], cmd_payload(cmd),
				dict))
		return -1;

	cmd->id = cmd_id;
	return 0;
}

int pyosdp_make_dict_cmd(PyObject **dict, struct osdp_cmd *cmd)
{
	PyObject *obj;

	if (cmd->id <= 0 || cmd->id >= OSDP_CMD_SENTINEL) {
		PyErr_Format(PyExc_ValueError, "Unknown command: %d", cmd->id);
		return -1;
	}

	obj = PyDict_New();
	if (obj == NULL)
		return -1;

	if (pyosdp_dict_add_int(obj, "command", cmd->id) ||
	    translate_to_dict(&command_translator[cmd->id], obj,
			      cmd_payload(cmd))) {
		Py_DECREF(obj);
		return -1;
	}

	*dict = obj;
	return 0;
}

int pyosdp_make_struct_event(struct osdp_event *event, PyObject *dict)
{
	int event_type;

	if (pyosdp_dict_get_int(dict, "event", &event_type))
		return -1;
	if (event_type <= 0 || event_type >= OSDP_EVENT_SENTINEL) {
		PyErr_Format(PyExc_ValueError, "Unknown event: %d", event_type);
		return -1;
	}
	if (translate_to_struct(&event_translator[event_type],
				event_payload(event), dict))
		return -1;

	event->type = event_type;
	return 0;
}

int pyosdp_make_dict_event(PyObject **dict, struct osdp_event *event)
{
	PyObject *obj;

	if (event->type <= 0 || event->type >= OSDP_EVENT_SENTINEL) {
		PyErr_Format(PyExc_ValueError, "Unknown event: %d",
			     event->type);
		return -1;
	}

	obj = PyDict_New();
	if (obj == NULL)
		return -1;

	if (pyosdp_dict_add_int(obj, "event", event->type) ||
	    translate_to_dict(&event_translator[event->type], obj,
			      event_payload(event))) {
		Py_DECREF(obj);
		return -1;
	}

	*dict = obj;
	return 0;
}

PyObject *pyosdp_make_dict_pd_id(struct osdp_pd_id *pd_id)
{
	PyObject *obj = PyDict_New();

	if (obj == NULL)
		return NULL;

	if (pyosdp_dict_add_int(obj, "version", pd_id->version) ||
	    pyosdp_dict_add_int(obj, "model", pd_id->model) ||
	    pyosdp_dict_add_int(obj, "vendor_code", pd_id->vendor_code) ||
	    pyosdp_dict_add_int(obj, "serial_number", pd_id->serial_number) ||
	    pyosdp_dict_add_int(obj, "firmware_version",
				pd_id->firmware_version)) {
		Py_DECREF(obj);
		return NULL;
	}

	return obj;
}
