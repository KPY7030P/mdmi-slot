# Env Speed cell (stage 44, D88)

**Place.** PSG page, row 1, the PAN place: x 358, y 56, 60 x 20. It replaces the dim PAN there. FM pages keep PAN as it is. On PSG, the MONO place stays dim.

**Fit.** At size 2, "SPD" plus "1.00" needs 66 px of room, and the cell has 28. So the label is "SPD" at size 1, at x+4, and the value is `patch_format`'s "x1.00" at size 1, ending at x+57. That takes 18 + 30 px, with 5 px between them. This is the row's one size-1 value. The cell is read from its bar, and the hint gives the exact number.

**Bar.** It fills from the middle like TUNE, with a C_LINE tick at 64 (x+30, which is x1). It fills right when faster and left when slower, in PSG dark. No amber "changed" tick: the knob is the console's, not part of a sound, so there is nothing to differ from.

**Channel 10.** SPD stays in row 1 at the same place, and stays live when there is no pitch: the envelope still drives the noise's volume. Only TUNE, BEND, PORTA and TIME go dim.

**Mouse.** Dragging works as on any row cell. A double click returns it to 64 (x1.00). One wheel notch moves it by one value, up is faster.

**Footer.** On hover: `ENV SPEED X0.50: A STEP 33 MS. DOUBLE CLICK: X1`. The step time is 1000/60/speed, rounded. While dragging, the same text without the double-click part. No value tag: that belongs to the graphs.

**Left out.** The cell is not on ALL or BANK pages. For ROADMAP: the envelope's own millisecond hints (STEP, LENGTH, LOOP) still count stock frames and ignore speed.
