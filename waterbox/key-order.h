/* key-order.h - written by gen-config.py; do not edit.
 * The keyboard's keys are listed as they sit on the keyboard, row by row
 * from the top, then the keys beside the rows, the arrows and the keypad
 * (chimera#337). kListedKbd[i] is the KBD_KEYS value of the key in place i
 * of the list; kKbdListed[k] is the place of KBD_KEYS value k (KBD_NONE has
 * none, -1). */
#pragma once
static const int kListedKbd[102] = {
	49, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 75, 61, 76,
	63, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 64, 65, 51, 50, 11,
	12, 13, 14, 15, 16, 17, 18, 19, 20, 67, 68, 66, 60, 21, 22, 23,
	24, 25, 26, 27, 28, 29, 69, 70, 52, 58, 74, 30, 31, 32, 33, 34,
	35, 36, 72, 71, 73, 59, 56, 54, 53, 55, 57, 77, 78, 79, 80, 81,
	82, 84, 83, 85, 86, 62, 97, 98, 99, 93, 94, 95, 100, 90, 91, 92,
	87, 88, 89, 101, 96, 102,
};
static const int kKbdListed[103] = {
	-1, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 31, 32, 33, 34, 35,
	36, 37, 38, 39, 40, 45, 46, 47, 48, 49, 50, 51, 52, 53, 59, 60,
	61, 62, 63, 64, 65, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
	12, 0, 30, 29, 56, 72, 71, 73, 70, 74, 57, 69, 44, 14, 85, 16,
	27, 28, 43, 41, 42, 54, 55, 67, 66, 68, 58, 13, 15, 75, 76, 77,
	78, 79, 80, 82, 81, 83, 84, 96, 97, 98, 93, 94, 95, 89, 90, 91,
	100, 86, 87, 88, 92, 99, 101,
};
