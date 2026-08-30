/* dgroup.h - THE GAME'S DATA SEGMENT, laid out at its own offsets.
 *
 * PROVENANCE: every field is a byte of vgalemmi.exe's DGROUP, which begins at
 * image offset 0x9A90; a field at offset N is the byte the disassembly calls
 * `ds:N`. Nothing here is the port's own - a variable the original does not
 * have does not belong in this struct, and that boundary is the point of it.
 *
 * WHY A STRUCT RATHER THAN FIFTY GLOBALS. Scattered globals can each carry an
 * address in a comment and still be wrong about it, and nothing can check
 * them. Here the address IS the layout: `_Static_assert(offsetof(...) == N)`
 * on every field means a wrong offset does not compile, and the whole thing
 * can be compared against the guest's own DS in one pass rather than a
 * variable at a time. tools/ can walk DG_FIELDS and diff every byte the port
 * claims to know, at the game's own end-of-frame.
 *
 * THE PADDING IS THE UNREAD PART, and it is most of it: 53 offsets are known
 * out of ~20,000 bytes. Each `_pad` run is bytes nobody has read yet, and
 * naming one is how a field arrives here. The padding is NOT compared - it
 * holds whatever the guest holds and the port never writes it, so a memcmp of
 * the whole struct would report twenty thousand differences and none of them
 * would mean anything. Compare DG_FIELDS, not sizeof(dgroup).
 *
 * THREE THINGS ARE DELIBERATELY ABSENT:
 *
 *   - the LEMMING ARRAY at ds:0x88. The original's records are 0x2D bytes and
 *     this port's are not, so the array cannot sit at its own offsets until
 *     the record layout is transcribed. tools/probe_lemmings.py already dumps
 *     the original's; that is the largest single thing still owed here.
 *   - the TRIGGER MAP, which is not in this segment at all. Image 0x023A6
 *     reads it `ss:[bx]` and image 0x00458 clears 0x642 bytes of it - a fixed
 *     global that happens to live in the stack segment. tools/verify_triggers.py
 *     covers it, and reading it out of DGROUP is a mistake this project has
 *     already made and measured as 47% wrong.
 *   - anything whose REPRESENTATION differs from the original's. Those are
 *     marked in the padding below rather than given a field, because a field
 *     here is a promise that the byte can be compared.
 */
#ifndef LEM_DGROUP_H
#define LEM_DGROUP_H

#include <stdint.h>
#include <stddef.h>

/* ds:0x43 counts the live entries; this is the ARRAY's capacity, which is
 * what the segment has to make room for. */
#define LEMMING_MAX 80

/* ONE LEMMING - image 0x03634 walks the array at ds:0x88, ds:0x43 entries of
 * 0x2D bytes, and image 0x0367F steps it with `add di, 0x2d`.
 *
 * THE FIELDS ARE AT THE ORIGINAL'S OFFSETS, which they were not: every one
 * carried its offset in a comment and they were DECLARED in a different order,
 * so C packed them into 34 bytes with nothing where the original has it. The
 * record could not sit at ds:0x88 in dgroup.h - it is the largest padding run
 * there - and no state check could see a lemming's POSITION at all. When the
 * played comparison came down to "the lemmings differ and the terrain does
 * not", the only witness was a picture.
 *
 * Laid out this way the array is data both sides hold in the same shape, and
 * ENSURE_LEM_AT below means a field that drifts does not compile.
 *
 * The five padding runs are what has not been read. Four of them -
 * 0x0A, 0x0C, 0x10, 0x12 and 0x14 - are the original's SPRITE pointers, into
 * a pre-shifted store this port does not build; they are named in
 * tools/probe_lemmings.py as ink, mask, frame_sz and mask_d. The port reaches
 * the same pictures from MAIN.DAT instead, which is why it needs none of them
 * and why they will very likely stay padding.
 */
typedef struct {
    int16_t  x;                 /* +0x00 */
    int16_t  y;                 /* +0x02 */
    int16_t  hot_x;             /* +0x04 */
    int16_t  hot_y;             /* +0x06 */
    /* the state bitmask image 0x01F5E dispatches on */
    uint16_t flags;             /* +0x08 */
    /* +0x0A ink and +0x0C mask - sprite pointers, see above */
    uint8_t  _pad_0A[4];
    /* how far into the float table image 0x02A70 has got */
    int16_t  fscript;           /* +0x0E */
    /* +0x10 frame_sz, +0x12 mask_d, +0x14 - more of the same store */
    uint8_t  _pad_10[6];
    /* the nine trigger cells a BLOCKER covers, kept so they can be put back.
     * Image 0x0346E reads each one out and image 0x03471 writes the blocker's
     * own value over it. */
    uint8_t  under[9];          /* +0x16 */
    uint8_t  _pad_1F[1];
    /* image 0x031E1 sets it to 0x4F and the LOOP HEAD counts it down at
     * image 0x01F45 */
    uint8_t  fuse;              /* +0x20 */
    /* image 0x032F2 gives a builder twelve */
    uint8_t  bricks;            /* +0x21 */
    uint8_t  _pad_22[1];
    /* the fall counter, +3 a frame */
    uint8_t  fall;              /* +0x23 */
    /* image 0x027DE sets bit 7 when a builder runs out, and image 0x03DFD
     * reads that bit to keep calling it a BUILDER */
    uint8_t  extra;             /* +0x24 */
    /* image 0x0356B sets it to 0xFF, and image 0x03E0E reads it to call one
     * a FLOATER */
    uint8_t  floater;           /* +0x25 */
    uint8_t  alive;             /* +0x26 - 0xFF is dead */
    int8_t   dir;               /* +0x27 */
    uint8_t  anim;              /* +0x28 */
    uint8_t  state;             /* +0x29 - 4 falling, 9 walking */
    uint8_t  _pad_2A[1];
    /* the trigger cell two rows up and two along the way it faces,
     * image 0x02EFC */
    uint8_t  tahead;            /* +0x2B */
    /* the one under it, image 0x02EEA */
    uint8_t  tcell;             /* +0x2C */
} __attribute__((packed)) lemming;

#define ENSURE_LEM_AT(f, off) \
    _Static_assert(offsetof(lemming, f) == (off), #f " is not at +" #off)

ENSURE_LEM_AT(x, 0x00);      ENSURE_LEM_AT(y, 0x02);
ENSURE_LEM_AT(hot_x, 0x04);  ENSURE_LEM_AT(hot_y, 0x06);
ENSURE_LEM_AT(flags, 0x08);  ENSURE_LEM_AT(fscript, 0x0E);
ENSURE_LEM_AT(under, 0x16);  ENSURE_LEM_AT(fuse, 0x20);
ENSURE_LEM_AT(bricks, 0x21); ENSURE_LEM_AT(fall, 0x23);
ENSURE_LEM_AT(extra, 0x24);  ENSURE_LEM_AT(floater, 0x25);
ENSURE_LEM_AT(alive, 0x26);  ENSURE_LEM_AT(dir, 0x27);
ENSURE_LEM_AT(anim, 0x28);   ENSURE_LEM_AT(state, 0x29);
ENSURE_LEM_AT(tahead, 0x2B); ENSURE_LEM_AT(tcell, 0x2C);
/* image 0x0367F's `add di, 0x2d` - the stride, and the whole point. */
_Static_assert(sizeof(lemming) == 0x2D, "a lemming is not 0x2D bytes");

#define DG_END 0x4DFDu          /* one past the last byte any field claims */

typedef struct {
    uint8_t  _pad_0000[0x0010];
    /* Where the pointer is, in the ORIGINAL's coordinates - eight more than
     * the screen position, clamped the way image 0x0426D clamps them. Image
     * 0x00F41 starts it at (150, 100). */
    uint16_t mouse_x;                       /* ds:0x0010 */
    uint16_t mouse_y;                       /* ds:0x0012 */
    /* Image 0x04269 stores INT 33h AX=3's BX here: bit 0 the left button, bit
     * 1 the right. Every screen polls it. */
    uint8_t  buttons;                       /* ds:0x0014 */
    uint8_t  _pad_0015[0x0005];
    /* Is the pointer over a lemming - image 0x03DE8 sets it, image 0x03DAF
     * clears it, image 0x05854 is what it changes. */
    uint8_t  cursor_box;                    /* ds:0x001A */
    uint8_t  _pad_001B[0x0010];
    uint8_t  paused;                        /* ds:0x002B */
    uint8_t  _pad_002C[0x0014];
    uint8_t  saved_pct;                     /* ds:0x0040 */
    uint8_t  needed_pct;                    /* ds:0x0041 */
    uint8_t  _pad_0042[1];
    uint8_t  released;                      /* ds:0x0043 */
    uint8_t  _pad_0044[2];
    uint8_t  saved;                         /* ds:0x0046 */
    uint8_t  to_release;                    /* ds:0x0047 */
    uint8_t  release_wait;                  /* ds:0x0048 */
    uint8_t  release_step;                  /* ds:0x0049 */
    uint8_t  _pad_004A[1];
    uint8_t  nuke;                          /* ds:0x004B */
    uint8_t  nuke_at;                       /* ds:0x004C */
    uint8_t  _pad_004D[1];
    uint8_t  level_phase;                   /* ds:0x004E */
    uint8_t  level_over;                    /* ds:0x004F */
    uint8_t  _pad_0050[2];
    uint8_t  level_frame;                   /* ds:0x0052 */
    uint8_t  pause_at;                      /* ds:0x0053 */
    uint8_t  tune;                          /* ds:0x0054 */
    uint8_t  entrance_open;                 /* ds:0x0055 */
    uint8_t  _pad_0056[0x000D];
    /* ds:0x0063 - how many lemmings the last scan found under the pointer. */
    uint8_t  under_count;                   /* ds:0x0063 */
    uint8_t  rate_min;                      /* ds:0x0064 - the level's floor */
    uint8_t  _pad_0065[2];
    uint8_t  rate;                          /* ds:0x0067 */
    /* ds:0x0068 IS NOT A FIELD, and that is a finding rather than an omission.
     * It is the rate the PANEL is showing, which image 0x03B86 compares with
     * ds:0x0067 to decide whether to redraw; the port models it as a FLAG
     * because only "does it match" is ever asked. A flag and a rate are not
     * the same byte, so it cannot be compared until it is a rate. */
    uint8_t  _pad_0068[1];
    uint8_t  rate_div;                      /* ds:0x0069 */
    /* ds:0x006A IS THE EIGHT SKILL COUNTS, THREE BYTES APART - not eight
     * contiguous bytes, which is how the port stores them. Two more bytes a
     * skill are unread. Until the stride is transcribed these cannot be
     * compared, and the port's g_skills is the right VALUES at the wrong
     * addresses. */
    uint8_t  _pad_006A[0x0018];
    uint8_t  skill;                         /* ds:0x0082 - the armed one */
    uint8_t  selected;                      /* ds:0x0083 - the DRAWN one */
    uint8_t  skill_div;                     /* ds:0x0084 - the repeat divider */
    uint8_t  tick;                          /* ds:0x0085 */
    uint8_t  tick_load;                     /* ds:0x0086 */
    /* ds:0x0088 IS THE LEMMING ARRAY - see the header. Not a field yet. */
    uint8_t  _pad_0087[1];
    /* IMAGE 0x03634's ARRAY. It was this file's largest padding run - 0xEF1
     * bytes with a note saying the record could not sit here because the
     * port's was 34 bytes and the original's is 0x2D. The record is laid out
     * at its own offsets now, so the array is data both sides hold in the same
     * shape and a lemming's POSITION can be compared as a byte rather than
     * inferred from a picture. */
    lemming  lem[LEMMING_MAX];  /* ds:0x0088 */
    uint8_t  _pad_0E98[0x00E0];
    /* image 0x016D4's per-scancode table: 0x94 while a key is held, 0 when it
     * comes up, and image 0x04D1A scans it with the repeat countdown in the
     * low seven bits. */
    uint8_t  key_down[0x53];                /* ds:0x0F78 */
    uint8_t  _pad_0FCB[0x001D];
    /* The level's EXTENDED graphics set, which image 0x04836 turns into a
     * tune and image 0x03742 into a different terrain build. */
    uint8_t  tune_pick;                     /* ds:0x0FE8 */
    uint8_t  _pad_0FE9[0x0F82];
    uint8_t  level_index;                   /* ds:0x1F6B - the running order */
    uint8_t  tune_rot;                      /* ds:0x1F6C */
    uint8_t  tune_advance;                  /* ds:0x1F6D */
    uint8_t  level_rating;                  /* ds:0x1F6E */
    uint8_t  rating_drawn;                  /* ds:0x1F6F - the plate's guard */
    uint8_t  fx_dirty;                      /* ds:0x1F70 - "something changed" */
    uint8_t  snd_setting;                   /* ds:0x1F71 - 0, 1 or 2 */
    uint8_t  fx_drawn;                      /* ds:0x1F72 - the plate's guard */
    uint8_t  _pad_1F73[1];
    uint8_t  cheat;                         /* ds:0x1F74 */
    uint8_t  _pad_1F75[0x0052];
    /* The sixteen watched keys as a HELD mask - image 0x0176E sets a bit and
     * image 0x01776 clears it. Bit 5 is Escape and bit 6 is Enter. */
    uint16_t key_held;                      /* ds:0x1FC7 */
    uint8_t  _pad_1FC9[4];
    uint8_t  tick_pending;                  /* ds:0x1FCD - five to a frame */
    uint8_t  _pad_1FCE[2];
    uint8_t  key_last;                      /* ds:0x1FD0 - the last scancode */
    uint8_t  _pad_1FD1[2];
    uint8_t  nuke_tick;                     /* ds:0x1FD3 */
    uint8_t  nuke_last;                     /* ds:0x1FD4 */
    uint8_t  _pad_1FD5[0x003B];
    uint8_t  snd_driver;                    /* ds:0x2010 - 0xFF if one answered */
    uint8_t  _pad_2011[0x0595];
    uint8_t  input_kind;                    /* ds:0x25A6 */
    uint8_t  snd_bits;                      /* ds:0x25A7 - the two gate bits */
    uint8_t  _pad_25A8[0x2375];
    uint8_t  code_pct;                      /* ds:0x491D */
    uint8_t  _pad_491E[0x0042];
    /* image 0x0E3F0's counters, one per menu animation. */
    int16_t  anim_ctr[10];                  /* ds:0x4960 */
    uint8_t  _pad_4974[0x047E];
    /* THE TEN TYPED CELLS of the access-code screen, and ten is the whole
     * field. The eleventh byte at ds:0x4DFC is the text record's own line
     * terminator - the original keeps 0x0D there and this port writes its C
     * string's NUL over it - so it belongs to neither side's idea of a cell,
     * and comparing it reports a difference of PURPOSE as one of value.
     *
     * The ARRAY is eleven because this port needs the NUL - it treats the
     * cells as a C string - so the eleventh byte stays here and
     * tools/verify_dgroup.py compares ten. Shrinking the field instead put
     * that write out of bounds, which -Warray-bounds said at once. */
    char     code_cells[11];                /* ds:0x4DF2 */
} __attribute__((packed)) dgroup;

/* THE ADDRESSES, ASSERTED. A field that drifts from the offset its comment
 * claims does not compile - which is the whole reason for laying the segment
 * out rather than keeping fifty globals that each say where they came from
 * and cannot be held to it. */
#define ENSURE_DG_AT(f, off) \
    _Static_assert(offsetof(dgroup, f) == (off), #f " is not at ds:" #off)

ENSURE_DG_AT(mouse_x,      0x0010);  ENSURE_DG_AT(mouse_y,      0x0012);
ENSURE_DG_AT(buttons,      0x0014);  ENSURE_DG_AT(cursor_box,   0x001A);
ENSURE_DG_AT(paused,       0x002B);  ENSURE_DG_AT(saved_pct,    0x0040);
ENSURE_DG_AT(needed_pct,   0x0041);  ENSURE_DG_AT(released,     0x0043);
ENSURE_DG_AT(saved,        0x0046);  ENSURE_DG_AT(to_release,   0x0047);
ENSURE_DG_AT(release_wait, 0x0048);  ENSURE_DG_AT(release_step, 0x0049);
ENSURE_DG_AT(nuke,         0x004B);  ENSURE_DG_AT(nuke_at,      0x004C);
ENSURE_DG_AT(level_phase,  0x004E);  ENSURE_DG_AT(level_over,   0x004F);
ENSURE_DG_AT(level_frame,  0x0052);  ENSURE_DG_AT(pause_at,     0x0053);
ENSURE_DG_AT(tune,         0x0054);  ENSURE_DG_AT(entrance_open,0x0055);
ENSURE_DG_AT(under_count,  0x0063);  ENSURE_DG_AT(rate_min,     0x0064);
ENSURE_DG_AT(rate,         0x0067);  ENSURE_DG_AT(rate_div,     0x0069);
ENSURE_DG_AT(skill,        0x0082);  ENSURE_DG_AT(selected,     0x0083);
ENSURE_DG_AT(skill_div,    0x0084);  ENSURE_DG_AT(tick,         0x0085);
ENSURE_DG_AT(tick_load,    0x0086);  ENSURE_DG_AT(key_down,     0x0F78);
ENSURE_DG_AT(lem,         0x0088);
ENSURE_DG_AT(tune_pick,    0x0FE8);  ENSURE_DG_AT(level_index,  0x1F6B);
ENSURE_DG_AT(tune_rot,     0x1F6C);  ENSURE_DG_AT(tune_advance, 0x1F6D);
ENSURE_DG_AT(level_rating, 0x1F6E);  ENSURE_DG_AT(rating_drawn, 0x1F6F);
ENSURE_DG_AT(fx_dirty,     0x1F70);  ENSURE_DG_AT(snd_setting,  0x1F71);
ENSURE_DG_AT(fx_drawn,     0x1F72);  ENSURE_DG_AT(cheat,        0x1F74);
ENSURE_DG_AT(key_held,     0x1FC7);  ENSURE_DG_AT(tick_pending, 0x1FCD);
ENSURE_DG_AT(key_last,     0x1FD0);  ENSURE_DG_AT(nuke_tick,    0x1FD3);
ENSURE_DG_AT(nuke_last,    0x1FD4);  ENSURE_DG_AT(snd_driver,   0x2010);
ENSURE_DG_AT(input_kind,   0x25A6);  ENSURE_DG_AT(snd_bits,     0x25A7);
ENSURE_DG_AT(code_pct,     0x491D);  ENSURE_DG_AT(anim_ctr,     0x4960);
ENSURE_DG_AT(code_cells,   0x4DF2);
_Static_assert(sizeof(dgroup) == DG_END, "the segment is not DG_END bytes");

extern dgroup DG;

#endif
