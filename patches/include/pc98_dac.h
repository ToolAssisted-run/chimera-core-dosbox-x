
void pc98_update_palette(void);
void pc98_update_digpal(unsigned char ent);
void pc98_set_digpal_entry(unsigned char ent,unsigned char grb);
void pc98_set_digpal_pair(unsigned char start,unsigned char pair);
unsigned char pc98_get_digpal_pair(unsigned char start);
void VGA_DAC_UpdateColor( Bitu index );

extern uint32_t                 pc98_text_palette[8];
extern uint8_t                  pc98_16col_analog_rgb_palette_index;

extern uint8_t                  pc98_pal_vga[256*3];    /* G R B    0x0..0xFF */
extern uint8_t                  pc98_pal_analog[256*3]; /* G R B    0x0..0xF */
extern uint8_t                  pc98_pal_digital[8];    /* G R B    0x0..0x7 */

/* Chimera: 4-bit to 8-bit, as the hardware: level N is N * 17 on 0 to 255
 * (chimera#374). 4 to 6 bits then 6 to 8 bits made 7 into 117, not 119. */
static inline unsigned char dac_4to8(unsigned char c4) {
    return (unsigned char)((c4 & 0xFu) * 0x11u);
}

/* 4-bit to 6-bit expansion */
static inline unsigned char dac_4to6(unsigned char c4) {
    /* a b c d . .
     *
     * becomes
     *
     * a b c d a b */
    return (c4 << 2) | (c4 >> 2);
}
