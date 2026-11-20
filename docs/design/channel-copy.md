# Channel copy: drag a rail row onto another (stage 43)

A channel's whole sound goes to another channel of the same kind: FM 1-6 to FM, PSG 7-10 to PSG.
The DAC row is neither a source nor a target.

## Press and threshold

- The press selects the channel, as today. Nothing else happens.
- Past DRAG_SLOP (3 px, as BANK rows) it is a drag. Released within it, it was a click.
- The press starts no drag on the DAC row.

## During the drag

- At the mouse: the source's name x2, C_BG on a C_TEXT box, as a dragged BANK row.
- Source row: keeps its selected look, not dim, not framed.
- Rows of the same kind: the double amber frame, the file-drop "can take" look.
- The row under the mouse, if one of those: amber fill, ink C_BG (the file-drop `target`).
- Rows of the other kind and the DAC row: dim (`dim`).
- On ALL a row reaches the page's right edge, as for drops (`row_at`).
- The wheel does nothing while dragging (`busy`).

## Footer during the drag

- Over a valid row, amber: «COPY CH2 TO CH5, REPLACES <name>».
- Over the source: «DROP ON ANOTHER FM ROW» (PSG for PSG), C_LABEL.
- Over a dim row: «FM GOES TO FM 1-6» or «PSG GOES TO PSG 7-10», C_LABEL.
- Elsewhere: «LET GO OUTSIDE A ROW TO CANCEL», C_LABEL.

## Release

- On a valid row: the copy. That row flashes as after a file drop.
  - Footer, amber, MESSAGE_TICKS: «CH5 NOW PLAYS CH2'S SOUND. UNDO TAKES IT BACK».
  - One UNDO step restores CH5 whole.
- On the source, on a dim row, or outside the rail: nothing happens and the footer says nothing.
- The right button during the drag, or the mouse captured by another window: the drag ends, nothing is copied, the rail is plain at once. Esc does nothing: the window gets no keys but the arrows (VST3 wrapper).
- A second quick press on a rail row arrives as a double click and starts no drag: press again after a moment.

## Selection after the drop

The target becomes the selected channel. You copy a sound in order to change the copy, so the editor shows
CH5 at once. The source was selected only by the press.

## Checkable in frames

1. A drag from CH2 held over CH5.
2. Held over CH8.
3. Held over CH2 itself.
4. Just after the drop.
5. After UNDO.

## Left out

- No swap.
- No Ctrl to swap or to copy one part only.
- No copy across kinds.
- No auto-scroll.
- No drop onto BANK lists.

## For the architect

What "whole sound" means for CH3 in Ch3 Special and for CH6 next to the DAC switch: the voice and its
envelopes or arp only, or the channel's mode switches too. This note assumes the sound only, without the
mode switches.
