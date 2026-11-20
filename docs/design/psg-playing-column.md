# PSG envelope: the playing column (stage 41)

The green 1 px line (`C_PLAYING`) leaves both PSG envelopes. The column of the frame that plays lights up instead,
in the chip's own colours, as W17 does on the ROM screen. No new constants.

| Part of the playing column | Now | Playing |
| --- | --- | --- |
| Background, full lane height (level and pitch lanes) | `C_BG` / loop `C_CELL` / hover `C_LINE` | `C_PSG_DARK` 0x004466 |
| Level body | `C_PSG_DARK` | `C_PSG` 0x22CCEE |
| Level top (3 px editor, 2 px ALL) | `C_PSG` | `C_TEXT` 0xEEEEEE |
| Pitch bar | `C_PSG` | `C_TEXT` |
| Pitch bar, noise without pitch | `C_LINE` | stays `C_LINE`; the background alone marks it |
| Level grid lines | `C_CELL` / `C_LINE` | `C_LINE`, as in a loop |

Playing wins over loop and hover in that one column; the loop strip above the lane still shows the loop. The 1 px gap
between columns stays, so neighbours keep their edge.

Passed and future columns do not change. W17 has a fixed 12 columns; in the window the lane is an editor, a loop
makes "passed" meaningless after its first turn, and the dim release already uses the dim tones.

Narrow columns. Editor columns are at least 3 px (406 / 124). On ALL they go down to 1 px or below: the playing column
is then a 1 px stripe, white at the level top, cyan below, dark blue above, in the place of the old green line. A
silent step (level 15) shows only the dark blue background: faint on black, still told apart from the grey loop by hue.

The release not played after a stopped loop is never lit: the editor reckons the frame on the played shape, as now.

No note, or the frame is outside the envelope: nothing is lit, the envelope looks as when idle.

Left out: the FM curve cursor on ALL (`gui.c` ~5956) stays green. Whether it follows is the owner's call.

Frame checks: a held note in a loop (editor and ALL); Note Off on a silent step; a 124-frame envelope on ALL; hover
over the playing column; no note.
