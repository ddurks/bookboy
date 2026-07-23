/* Art-derived layout constants, shared by the builder and the GBA runtime.
 * Derived from the hand-drawn frames in assets/art/ (all 240x160, drawn in
 * one coordinate space): re-measure if the art changes.
 */
#ifndef BB_ART_H
#define BB_ART_H

/* book.png spine, also where grab_book3 grips: the selected book's home */
#define AR_SPINE_X 18
#define AR_SPINE_Y 58
#define AR_SPINE_W 204
#define AR_SPINE_H 24
#define AR_PITCH 22                 /* solid ink is rows 59-80: touch exactly */

/* held-book pose is composed at runtime: cover2 + white face + outline,
 * bottom corners nested into the show_hand pockets */
#define AR_COVER_W 116              /* max cover2 fit box */
#define AR_COVER_H 118
#define AR_POCKET_X 17              /* finger column inner edge (measured) */
#define AR_POCKET_Y 56
#define AR_BOOK_BOTTOM 152          /* held book outer bottom edge on screen */

/* speak.png bubble interior */
#define AR_SPEAK_X 10
#define AR_SPEAK_Y 4
#define AR_SPEAK_W 220
#define AR_SPEAK_H 24

/* title screen: blank book on the bottom-left shelf (PRESS START home) */
#define AR_PSTART_X 24
#define AR_PSTART_Y 119
#define AR_PSTART_W 90
#define AR_PSTART_H 17

/* art.bin sprite ids, in file order */
enum { ART_BOOK, ART_HANDS, ART_GRAB1, ART_GRAB2, ART_GRAB3, ART_GRAB4,
       ART_SHOWL, ART_SHOWR, ART_SPEAK, ART_COUNT };

/* palette banks (mode 4): 0-15 reader text, 16.. spine tints, white last */
#define PAL_SPINE_BASE 16
#define PAL_SPINE_BANKS 4
#define PAL_WHITE (PAL_SPINE_BASE + PAL_SPINE_BANKS * 16)   /* 112 */
#define PAL_COVER 128               /* per-book quantized cover palette */

#endif
