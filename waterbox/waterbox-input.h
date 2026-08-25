/* The per-frame input block, shared VERBATIM by the guest adapter
 * (waterbox.cpp) and the host driver (run-wbx.c): the host writes this
 * struct into guest memory (GetInputBuffer) before each FrameAdvance.
 * Plain C, fixed-width fields only - the two sides may be built by
 * different compilers and must agree on every byte.
 *
 * DOSBox's keyboard is 101 keys, which is why input cannot ride the packed
 * uint64 alone; the wide-input engine extension (docs/PLAN.md) will drive
 * this same block from the frontend side. */
#ifndef WBX_DOSBOX_INPUT_H
#define WBX_DOSBOX_INPUT_H

#include <stdint.h>

#define WBX_KEY_COUNT 0x65 /* KBD_KEYS */

typedef struct {
	uint8_t up, down, left, right;
	uint8_t button1, button2;
	uint8_t pad[2];
} WbxJoy;

typedef struct {
	int32_t posX, posY;   /* absolute, 800x600 driver range */
	int32_t speedX, speedY;
	uint8_t leftPressed, middlePressed, rightPressed;
	uint8_t leftReleased, middleReleased, rightReleased;
	uint8_t pad[2];
	float sensitivity;
} WbxMouse;

typedef struct {
	uint8_t keys[WBX_KEY_COUNT]; /* 1 = held, indexed by KBD_KEYS */
	uint8_t pad[3];
	WbxJoy joy1, joy2;
	WbxMouse mouse;
	int32_t insertFloppyDisk; /* >= 0: swap drive A to image N this frame */
	int32_t insertCDROM;      /* >= 0: swap drive D to disc N this frame */
	int32_t framerateNumerator;   /* 0 = the DOS default 3146888/44900 */
	int32_t framerateDenominator;
} WbxInput;

#endif
