#include <string.h>
#include "names.h"

// Russian and Ukrainian letters from U+0400, two characters at most, lower case; the capital takes
// the same with its first letter raised. An empty entry drops the letter (the hard and soft signs).
static const char *const CYRILLIC[0x60] = {
	// U+0400-040F: Ѐ Ё Ђ Ѓ Є Ѕ І Ї Ј Љ Њ Ћ Ќ Ѝ Ў Џ
	"e", "yo", "d", "g", "ye", "z", "i", "yi", "j", "l", "n", "c", "k", "i", "u", "dz",
	// U+0410-042F: А-Я
	"a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
	"r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "sh", "", "y", "", "e", "yu", "ya",
	// U+0430-044F: а-я
	"a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
	"r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "sh", "", "y", "", "e", "yu", "ya",
	// U+0450-045F: ѐ ё ђ ѓ є ѕ і ї ј љ њ ћ ќ ѝ ў џ
	"e", "yo", "d", "g", "ye", "z", "i", "yi", "j", "l", "n", "c", "k", "i", "u", "dz",
};

// The length of the UTF-8 sequence a lead byte starts, 0 for a byte that can't start one.
static int sequence_length(unsigned char lead)
{
	if (lead < 0x80) {
		return 1;
	}
	if (lead >= 0xC2 && lead <= 0xDF) {
		return 2;
	}
	if (lead >= 0xE0 && lead <= 0xEF) {
		return 3;
	}
	if (lead >= 0xF0 && lead <= 0xF4) {
		return 4;
	}
	return 0;
}

// Whether the n bytes at s are a whole sequence; *code gets its code point.
static int whole(const unsigned char *s, int n, unsigned *code)
{
	unsigned value = n == 1 ? s[0] : s[0] & (0x7Fu >> n);
	for (int i = 1; i < n; i++) {
		if ((s[i] & 0xC0) != 0x80) {
			return 0;
		}
		value = value << 6 | (s[i] & 0x3Fu);
	}
	*code = value;
	return 1;
}

// The transliteration, whole letters only while chars fit (stage 55); true when a letter didn't.
static int fit(const char *utf8, char *out, size_t size, size_t chars)
{
	size_t at = 0;
	const unsigned char *s = (const unsigned char *)utf8;
	if (chars > size - 1) {
		chars = size - 1;
	}
	while (*s) {
		const int n = sequence_length(*s);
		unsigned code;
		if (!n || !whole(s, n, &code)) {
			s++;
			continue;
		}
		s += n;
		char shown[3] = {0};
		if (code >= 0x20 && code < 0x7F) {
			shown[0] = (char)code;
		} else if (code < 0x80) {
			continue;
		} else if (code >= 0x400 && code < 0x460) {
			const char *letters = CYRILLIC[code - 0x400];
			memcpy(shown, letters, strlen(letters));
			const int capital = code < 0x430;
			if (capital && shown[0]) {
				shown[0] = (char)(shown[0] - 'a' + 'A');
			}
		} else {
			shown[0] = '?';
		}
		const size_t length = strlen(shown);
		if (at + length > chars) {
			out[at] = 0;
			return 1;
		}
		memcpy(out + at, shown, length);
		at += length;
	}
	out[at] = 0;
	return 0;
}

void names_ascii(const char *utf8, char *out, size_t size)
{
	if (size) {
		fit(utf8, out, size, size - 1);
	}
}

void names_fit(const char *utf8, char *out, size_t size, int chars)
{
	if (!size) {
		return;
	}
	if (fit(utf8, out, size, chars > 0 ? (size_t)chars : 0)) {
		// A cut name doesn't end on a space.
		for (size_t at = strlen(out); at > 0 && out[at - 1] == ' '; at--) {
			out[at - 1] = 0;
		}
	}
}

void names_cut_utf8(char *utf8)
{
	const size_t length = strlen(utf8);
	size_t lead = length;
	// Back over at most 3 continuation bytes to the last sequence's lead byte.
	while (lead > 0 && length - lead < 3 && ((unsigned char)utf8[lead - 1] & 0xC0) == 0x80) {
		lead--;
	}
	if (lead == 0) {
		return;
	}
	lead--;
	const int n = sequence_length((unsigned char)utf8[lead]);
	if (n > 1 && (size_t)n > length - lead) {
		utf8[lead] = 0;
	}
}
