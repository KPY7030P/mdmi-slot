#include <stdio.h>
#include <string.h>
#include "instrument.h"

#define TFI_SIZE 42
#define VGI_SIZE INSTRUMENT_VGI_SIZE
#define TYI_SIZE 32
#define TYI_GROUP 4

enum { F_TL, F_MUL, F_DT, F_RS, F_AR, F_DR, F_SR, F_SL, F_RR, F_AM, F_SSG };
enum { V_ALGORITHM, V_FEEDBACK, V_AMS, V_FMS, V_OPS };

// TFI and VGI keep detune centred on 3; the chip has 0 for none, 1-3 up and 5-7 down.
static const uint8_t CENTRED_DT[8] = {7, 6, 5, 0, 1, 2, 3, 4};

// And back: register detune 4 is none as well.
static const uint8_t DT_CENTRED[8] = {3, 4, 5, 6, 3, 2, 1, 0};

// The model's operator for an operator in the file.
static const int FILE_OP[PATCH_OPS] = {0, 2, 1, 3};

static int16_t *op_of(patch_instrument *out, int file_op)
{
	return out->voice + V_OPS + FILE_OP[file_op] * PATCH_OP_PARAMS;
}

// The highest value of each field, as TFI and VGI write it.
typedef struct {
	uint8_t field;
	uint8_t max;
} file_field;

// TFI and VGI write the operator's fields in this order; VGI keeps AM in the top bit of DR.
static const file_field OP_FIELDS[10] = {
	{F_MUL, 15}, {F_DT, 7}, {F_TL, 127}, {F_RS, 3}, {F_AR, 31}, {F_DR, 31}, {F_SR, 31}, {F_RR, 15}, {F_SL, 15}, {F_SSG, 15},
};

static bool read_centred(const uint8_t *bytes, int first, bool has_am, patch_instrument *out)
{
	for (int op = 0; op < PATCH_OPS; op++) {
		const uint8_t *in = bytes + first + op * 10;
		int16_t *values = op_of(out, op);
		for (int i = 0; i < 10; i++) {
			int value = in[i];
			if (has_am && OP_FIELDS[i].field == F_DR) {
				values[F_AM] = (int16_t)(value >> 7);
				value &= 0x7F;
			}
			if (value > OP_FIELDS[i].max) {
				return false;
			}
			values[OP_FIELDS[i].field] = (int16_t)(OP_FIELDS[i].field == F_DT ? CENTRED_DT[value] : value);
		}
	}
	return true;
}

static bool read_tfi(const uint8_t *bytes, patch_instrument *out)
{
	if (bytes[0] > 7 || bytes[1] > 7) {
		return false;
	}
	out->voice[V_ALGORITHM] = bytes[0];
	out->voice[V_FEEDBACK] = bytes[1];
	return read_centred(bytes, 2, false, out);
}

static bool read_vgi(const uint8_t *bytes, patch_instrument *out)
{
	if (bytes[0] > 7 || bytes[1] > 7 || bytes[2] & 0xC8) {
		return false;
	}
	out->voice[V_ALGORITHM] = bytes[0];
	out->voice[V_FEEDBACK] = bytes[1];
	out->voice[V_FMS] = bytes[2] & 7;
	out->voice[V_AMS] = bytes[2] >> 4;
	return read_centred(bytes, 3, true, out);
}

// Register values grouped by register, an operator each. The first TYI ends in "TYI" and ignores AM;
// Genny's has AMS and FMS in place of the "T".
static bool read_tyi(const uint8_t *bytes, patch_instrument *out)
{
	bool old = memcmp(bytes + 29, "TYI", 3) == 0;
	if (!old && memcmp(bytes + 30, "YI", 2) != 0) {
		return false;
	}
	for (int op = 0; op < PATCH_OPS; op++) {
		int16_t *values = op_of(out, op);
		const uint8_t *in = bytes + op;
		values[F_DT] = (in[0] >> 4) & 7;
		values[F_MUL] = in[0] & 15;
		values[F_TL] = in[TYI_GROUP] & 0x7F;
		values[F_RS] = in[2 * TYI_GROUP] >> 6;
		values[F_AR] = in[2 * TYI_GROUP] & 31;
		values[F_AM] = old ? 0 : in[3 * TYI_GROUP] >> 7;
		values[F_DR] = in[3 * TYI_GROUP] & 31;
		values[F_SR] = in[4 * TYI_GROUP] & 31;
		values[F_SL] = in[5 * TYI_GROUP] >> 4;
		values[F_RR] = in[5 * TYI_GROUP] & 15;
		values[F_SSG] = in[6 * TYI_GROUP] & 15;
	}
	out->voice[V_FEEDBACK] = (bytes[28] >> 3) & 7;
	out->voice[V_ALGORITHM] = bytes[28] & 7;
	out->voice[V_AMS] = old ? 0 : (bytes[29] >> 4) & 3;
	out->voice[V_FMS] = old ? 0 : bytes[29] & 7;
	return true;
}

// The file's name without its folder and extension.
static void name_of(const char *file_name, char *name, size_t size, const char **extension)
{
	const char *base = file_name;
	for (const char *c = file_name; *c; c++) {
		if (*c == '/' || *c == '\\') {
			base = c + 1;
		}
	}
	const char *dot = strrchr(base, '.');
	*extension = dot ? dot + 1 : "";
	int length = dot ? (int)(dot - base) : (int)strlen(base);
	snprintf(name, size, "%.*s", length, base);
}

static bool same_text(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		if ((*a | 0x20) != (*b | 0x20)) {
			return false;
		}
	}
	return *a == *b;
}

bool instrument_parse(const char *file_name, const uint8_t *bytes, size_t size, patch_instrument *out, char *error, size_t error_size)
{
	const char *extension;
	memset(out, 0, sizeof(*out));
	name_of(file_name, out->name, sizeof(out->name), &extension);
	size_t expected;
	bool (*read)(const uint8_t *, patch_instrument *);
	if (same_text(extension, "tfi")) {
		expected = TFI_SIZE;
		read = read_tfi;
	} else if (same_text(extension, "vgi")) {
		expected = VGI_SIZE;
		read = read_vgi;
	} else if (same_text(extension, "tyi")) {
		expected = TYI_SIZE;
		read = read_tyi;
	} else {
		snprintf(error, error_size, "Not an instrument: .%s, only TFI, VGI, TYI", extension);
		return false;
	}
	if (size != expected) {
		snprintf(error, error_size, "Bad %s file: %u bytes, not %u", extension, (unsigned)size, (unsigned)expected);
		return false;
	}
	if (!read(bytes, out)) {
		snprintf(error, error_size, "Bad %s file: values out of range", extension);
		return false;
	}
	return true;
}

void instrument_write_vgi(const patch_instrument *instrument, uint8_t *out)
{
	const int16_t *voice = instrument->voice;
	out[0] = (uint8_t)voice[V_ALGORITHM];
	out[1] = (uint8_t)voice[V_FEEDBACK];
	out[2] = (uint8_t)(voice[V_AMS] << 4 | voice[V_FMS]);
	for (int op = 0; op < PATCH_OPS; op++) {
		const int16_t *values = voice + V_OPS + FILE_OP[op] * PATCH_OP_PARAMS;
		uint8_t *at = out + 3 + op * 10;
		for (int i = 0; i < 10; i++) {
			int field = OP_FIELDS[i].field, value = values[field];
			if (field == F_DT) {
				value = DT_CENTRED[value & 7];
			} else if (field == F_DR) {
				value |= values[F_AM] << 7;
			}
			at[i] = (uint8_t)value;
		}
	}
}
