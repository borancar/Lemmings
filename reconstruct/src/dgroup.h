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

#define DG_END 0x4DFDu          /* one past the last byte any field claims */

#pragma pack(push, 1)
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
    uint8_t  _pad_0087[0x0EF1];
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
    /* The ten typed cells of the access-code screen. */
    char     code_cells[11];                /* ds:0x4DF2 */
} dgroup;
#pragma pack(pop)

/* THE ADDRESSES, ASSERTED. A field that drifts from the offset its comment
 * claims does not compile - which is the whole reason for laying the segment
 * out rather than keeping fifty globals that each say where they came from
 * and cannot be held to it. */
#define DG_AT(f, off) \
    _Static_assert(offsetof(dgroup, f) == (off), #f " is not at ds:" #off)

DG_AT(mouse_x,      0x0010);  DG_AT(mouse_y,      0x0012);
DG_AT(buttons,      0x0014);  DG_AT(cursor_box,   0x001A);
DG_AT(paused,       0x002B);  DG_AT(saved_pct,    0x0040);
DG_AT(needed_pct,   0x0041);  DG_AT(released,     0x0043);
DG_AT(saved,        0x0046);  DG_AT(to_release,   0x0047);
DG_AT(release_wait, 0x0048);  DG_AT(release_step, 0x0049);
DG_AT(nuke,         0x004B);  DG_AT(nuke_at,      0x004C);
DG_AT(level_phase,  0x004E);  DG_AT(level_over,   0x004F);
DG_AT(level_frame,  0x0052);  DG_AT(pause_at,     0x0053);
DG_AT(tune,         0x0054);  DG_AT(entrance_open,0x0055);
DG_AT(under_count,  0x0063);  DG_AT(rate_min,     0x0064);
DG_AT(rate,         0x0067);  DG_AT(rate_div,     0x0069);
DG_AT(skill,        0x0082);  DG_AT(selected,     0x0083);
DG_AT(skill_div,    0x0084);  DG_AT(tick,         0x0085);
DG_AT(tick_load,    0x0086);  DG_AT(key_down,     0x0F78);
DG_AT(tune_pick,    0x0FE8);  DG_AT(level_index,  0x1F6B);
DG_AT(tune_rot,     0x1F6C);  DG_AT(tune_advance, 0x1F6D);
DG_AT(level_rating, 0x1F6E);  DG_AT(rating_drawn, 0x1F6F);
DG_AT(fx_dirty,     0x1F70);  DG_AT(snd_setting,  0x1F71);
DG_AT(fx_drawn,     0x1F72);  DG_AT(cheat,        0x1F74);
DG_AT(key_held,     0x1FC7);  DG_AT(tick_pending, 0x1FCD);
DG_AT(key_last,     0x1FD0);  DG_AT(nuke_tick,    0x1FD3);
DG_AT(nuke_last,    0x1FD4);  DG_AT(snd_driver,   0x2010);
DG_AT(input_kind,   0x25A6);  DG_AT(snd_bits,     0x25A7);
DG_AT(code_pct,     0x491D);  DG_AT(anim_ctr,     0x4960);
DG_AT(code_cells,   0x4DF2);
_Static_assert(sizeof(dgroup) == DG_END, "the segment is not DG_END bytes");

extern dgroup DG;

#endif
