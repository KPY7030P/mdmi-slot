#ifndef MDMI_INSTRUMENT_H
#define MDMI_INSTRUMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "patch.h"

// Instrument files of other programs: TFI, VGI and TYI hold one FM voice each and no name, so the
// name is the file's (D13). They keep operators in register order, 1, 3, 2, 4; the model and the
// ROM count them 1, 2, 3, 4 (docs/research/hardware-rom.md, section 7).
#define INSTRUMENT_VGI_SIZE 43

// Reads a file by its name's extension. False with a reason short enough for the window's footer.
bool instrument_parse(const char *file_name, const uint8_t *bytes, size_t size, patch_instrument *out, char *error, size_t error_size);

// A voice as VGI, which holds every field of it, with detune centred on 3 like VGM Music Maker's.
void instrument_write_vgi(const patch_instrument *instrument, uint8_t *out);

// EEF is a PSG envelope of bare bytes, one per frame, as the MDMI ROM reads it (D28). Echo's EEF
// shifts pitch by other steps; such a file loads as it is. The written one ends in EEF_END.
bool instrument_parse_eef(const char *file_name, const uint8_t *bytes, size_t size, patch_envelope *out, char *error, size_t error_size);
size_t instrument_write_eef(const patch_envelope *envelope, uint8_t *out);

#endif
