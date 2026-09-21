/*
 * mp4tag.h - stamps colour signalling into a finished MP4.
 *
 * The Media Foundation MP4 sink writes no 'colr' box, and no HDR metadata
 * either. The colour attributes set on the output media type reach the
 * encoder, which puts them in the HEVC VUI, but nothing in the container says
 * what the picture is - and a player that trusts the container reads BT.2020
 * PQ as Rec.709 and shows it washed out, which is exactly the fault an HDR
 * recording is meant to cure.
 *
 * 'mdcv' and 'clli' matter for a second, subtler reason. Handed HDR with no
 * light-level metadata, a player has to assume the content might be graded for
 * a very bright display and tone-maps defensively. A screen recording never
 * exceeds SDR white, so that guess costs real brightness and saturation.
 * Saying plainly how bright the content is stops the guessing.
 *
 * So the boxes are written afterwards, by hand. This is safe here only because
 * the sink puts 'moov' last: inserting bytes inside it leaves every 'stco'
 * chunk offset - which all point into the earlier 'mdat' - still correct. The
 * code checks that layout and declines rather than corrupting a recording.
 */
#ifndef NITSHOT_MP4TAG_H
#define NITSHOT_MP4TAG_H

#include "nitshot.h"

typedef struct {
    /* ISO/IEC 23001-8 code points. For what this records: 9 (BT.2020),
       16 (SMPTE ST 2084), 9 (BT.2020 non-constant luminance). */
    UINT16 primaries, transfer, matrix;
    BOOL   fullRange;

    /* The display the content was made on, as CIE xy and nits. Zero x/y means
       "not known", and the box is then left out rather than guessed at. */
    float  redX, redY, greenX, greenY, blueX, blueY, whiteX, whiteY;
    float  maxMasteringNits, minMasteringNits;

    /* Measured from the frames themselves: the brightest pixel in the whole
       recording, and the brightest frame average. Zero leaves the box out. */
    float  maxCLL, maxFALL;
} Mp4ColourInfo;

/*
 * Adds the colour boxes the sink did not write. Returns FALSE and leaves the
 * file untouched if the layout is not one that can be edited safely.
 */
BOOL Mp4Tag_WriteColour(const wchar_t *path, const Mp4ColourInfo *info);

#endif /* NITSHOT_MP4TAG_H */
