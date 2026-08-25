/* The deterministic mouse + joystick exercise pattern, shared VERBATIM by
 * run-native (which feeds the driver's input struct) and run-wbx (which
 * drives the guest's SetAxis/SetButton exports) - the gate compares the two
 * machines frame for frame, so the pattern must be one function.
 *
 * Levels, not edges: the guest adapter converts mouse-button levels to the
 * press/release events the driver wants, and the native runner does the same
 * conversion locally. Starts late enough that the gate's typed command has
 * finished (typing begins at frame 140; commands are short). */
#ifndef WBX_EXERCISE_INPUT_H
#define WBX_EXERCISE_INPUT_H

#include <stdint.h>

#define EX_START_FRAME 300

/* the config's button index blocks (gen-config.py's order) */
#define EX_BTN_JOY1 100 /* up down left right b1 b2 */
#define EX_BTN_JOY2 106
#define EX_BTN_MOUSE 112 /* left middle right */

typedef struct {
	int32_t posX, posY, spdX, spdY;
	uint8_t mouseL, mouseR;
	uint8_t joyUp, joyDown, joyLeft, joyRight, joyB1, joyB2;
} ExLevels;

static inline ExLevels exercise_levels(long frame)
{
	ExLevels e;
	e.posX = 400; e.posY = 300; e.spdX = 0; e.spdY = 0;
	e.mouseL = 0; e.mouseR = 0;
	e.joyUp = 0; e.joyDown = 0; e.joyLeft = 0; e.joyRight = 0;
	e.joyB1 = 0; e.joyB2 = 0;
	if (frame < EX_START_FRAME) return e;

	uint64_t x = (uint64_t)frame * 6364136223846793005ULL + 1442695040888963407ULL;
	x ^= x >> 33;
	e.spdX = (int32_t)(x % 21) - 10;
	e.spdY = (int32_t)((x >> 8) % 21) - 10;
	e.posX = (int32_t)((x >> 16) % 801);
	e.posY = (int32_t)((x >> 24) % 601);
	e.mouseL = (uint8_t)((frame >> 4) & 1);
	e.mouseR = (uint8_t)((frame >> 5) & 1);
	e.joyUp = (uint8_t)((frame >> 2) & 1);
	e.joyLeft = (uint8_t)((frame >> 3) & 1);
	e.joyB1 = (uint8_t)((frame >> 3) & 1);
	e.joyB2 = (uint8_t)((frame >> 4) & 1);
	return e;
}

#endif
