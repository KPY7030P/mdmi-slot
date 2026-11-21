#ifndef MDMI_NAMES_H
#define MDMI_NAMES_H

#include <stddef.h>

// Names as the window and the ROM's screen show them (stage 24): both fonts are ASCII only. Names
// themselves stay as they came, UTF-8, in the project, the bank and files.

// ASCII for a UTF-8 string: Cyrillic letters by a short transliteration (Ж Zh, Щ Sh), any other
// character outside ASCII as '?', control bytes and a broken or cut sequence dropped. Never longer
// than the input in bytes, so text cut to a number of bytes still fits as many columns. out holds
// at most size - 1 characters and always ends with 0.
void names_ascii(const char *utf8, char *out, size_t size);
// The same cut to chars characters by whole letters of the transliteration (stage 55): Sh for Ш goes
// whole or not at all; a cut name loses the spaces at its end. Every place the window cuts a name.
void names_fit(const char *utf8, char *out, size_t size, int chars);
// Ends a UTF-8 string cut at a byte limit on a whole character: a sequence the cut broke goes.
void names_cut_utf8(char *utf8);

#endif
