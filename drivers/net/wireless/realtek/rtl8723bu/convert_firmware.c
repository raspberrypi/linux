/* Program to convert header into a binary file
 * Coded by Larry Finger
 * August 2015
 *
 * There is no Makefile, build with:
 *	gcc -o convert_firmware convert_firmware.c
 */

#include <stdio.h>
#define u8 char
#define u32 int

/* Get the firmware data that has been copied from hal/HalHWImg8723B_FW.c */
#include "convert_firmware.h"

void output_bin(FILE *outb, const u8 *array, int len)
{
	int i;
	for (i = 0; i < len; i++)
		fwrite(&array[i], 1, 1, outb);
}

int main(int argc, char **argv)
{
	FILE *outb;
	int i;

	/* convert firmware */
	outb = fopen("rtl8723bu_ap_wowlan.bin", "w");
	if (!outb) {
		fprintf(stderr, "File open error\n");
		return 1;
	}
	output_bin(outb, Array_MP_8723B_FW_AP_WoWLAN,
		   ArrayLength_MP_8723B_FW_AP_WoWLAN);
	fclose(outb);

	outb = fopen("rtl8723bu_bt.bin", "w");
	if (!outb) {
		fprintf(stderr, "File open error\n");
		return 1;
	}
	output_bin(outb, Array_MP_8723B_FW_BT,
		   ArrayLength_MP_8723B_FW_BT);
	fclose(outb);

	outb = fopen("rtl8723bu_nic.bin", "w");
	if (!outb) {
		fprintf(stderr, "File open error\n");
		return 1;
	}
	output_bin(outb, Array_MP_8723B_FW_NIC,
		   ArrayLength_MP_8723B_FW_NIC);
	fclose(outb);

	outb = fopen("rtl8723bu_wowlan.bin", "w");
	if (!outb) {
		fprintf(stderr, "File open error\n");
		return 1;
	}
	output_bin(outb, Array_MP_8723B_FW_WoWLAN,
		   ArrayLength_MP_8723B_FW_WoWLAN);
	fclose(outb);

	return 0;
}


