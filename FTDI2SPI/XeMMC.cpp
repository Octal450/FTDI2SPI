#include <cstdint>
#include "XSPI.h"
#include <Windows.h>
#include <synchapi.h>

#include <stdio.h>
#include "wrapper_spi.h"

bool is_selected = false;
bool is_block_set = false;
// Utility wrappers for eMMC register access
void xbox_emmc_execute(uint32_t reg_4, uint32_t reg_8, uint32_t reg_c)
{
	XSPIWriteWORD_(0x30, 0xFFFFFFFF);
	XSPIWriteWORD_(0x04, reg_4);
	XSPIWriteWORD_(0x08, reg_8);
	XSPIWriteWORD_(0x0C, reg_c);
}
uint32_t xbox_emmc_get_ints()
{
	return XSPIReadWORD_(0x30);
}
void xbox_emmc_clear_ints(uint32_t value)
{
	return XSPIWriteWORD_(0x30, value);
}
void xbox_emmc_clear_all_ints()
{
	uint32_t ints = xbox_emmc_get_ints();
	if (ints)
		xbox_emmc_clear_ints(ints);
}

#define EMMC_BLOCK_SIZE 512  // eMMC sector size; adjust if needed
#define BLOCK_SIZE 512
#define NUM_BLOCKS (0x3000000 / BLOCK_SIZE) 
unsigned char buffer[BLOCK_SIZE];


#define SD_OK (0)
#define SD_ERR_TIMEOUT (-1)
#define SD_ERR_BAD_RESPONSE (-2)
#define SD_ERR_CRC (-3)
#define SD_ERR_BAD_PARAM (-4)

static int xbox_emmc_wait_ints(uint32_t value, int timeout_ms)
{
	int wait_timeout = 5000, t = 0;
	do
	{
		uint32_t ints = xbox_emmc_get_ints();
		if ((ints & value) == value)
		{
			return SD_OK;
		}
		t++;
	} while (t < wait_timeout);
	return SD_ERR_TIMEOUT;
}
static int xbox_emmc_select_card()
{
	if (is_selected)
		return SD_OK;
	xbox_emmc_execute(0, 0xffff0000, 0x71a0000);
	is_selected = true;
	is_block_set = false;
	return xbox_emmc_wait_ints(1, 100);
}

static int xbox_emmc_set_blocklen(int blocklen)
{
	if (is_block_set)
		return SD_OK;
	is_block_set = true;
	xbox_emmc_execute(0x200, blocklen, 0x101a0000);
	return xbox_emmc_wait_ints(1, 100);
}


bool send_cmd17(unsigned int lba) {
	ClearOutputBuffer();
	XSPIQueueBytes_(0x30, 0xFFFFFFFF);
	XSPIQueueBytes_(0x04, 0x200);             // Block size
	XSPIQueueBytes_(0x08, lba * 512);         // LBA (sector offset)
	XSPIQueueBytes_(0x0C, 0x113A0010);        // CMD17
	//64 bytes queue
	SendBytesToDevice();

	// Poll for data ready (bit 2 of 0x30), with timeout
	//this is *required*, you must wait until emmc says ready
	//const int timeout = 5000;
	//int elapsed = 0;
	//while ((XSPIReadWORD_(0x30) & 0x21) != 0x21) { // Wait for both bits 0 and 5
	//	Sleep(1);
	//	if (++elapsed >= timeout) {
	//		printf("Timeout waiting for 0x21 in 0x30\n");
	//		return false;
	//	}
	//}
	return true;
}

bool send_cmd24(unsigned int lba) {
	ClearOutputBuffer();     // Some control register, reset state
	XSPIQueueBytes_(0x04, 0x10200);             // Set block size = 512 bytes
	XSPIQueueBytes_(0x08, lba << 9);         // Set the LBA/offset
	XSPIQueueBytes_(0x0C, 0x183A0000);        // CMD24, argument/flags may differ for your HW
	SendBytesToDevice();                      // Perform all above over one USB transaction

	const int timeout = 5000;
	int elapsed = 0;
	while ((XSPIReadWORD_(0x30) & 0x1) != 0x1) { // Wait for both bits 0 and 5
		Sleep(1);
		if (++elapsed >= timeout) {
			printf("Timeout waiting for 0x1 in 0x30 WRITE_SINGLE_BLOCK\n");
			return false;
		}
	}
	return true;
}





bool emmc_read_block(uint32_t block, unsigned char* buffer) {




	// Read block with CMD17
	if (!send_cmd17(block)) {
		return false;
	}
	XSPIReadBlock_(0x20, buffer, 128);
	XSPIWriteWORD_(0x30, 0xFFFFFFFF);

	return true;
}
static int xbox_emmc_read_cid_csd(uint8_t* buf, int is_cid)
{
	xbox_emmc_execute(0, 0xffff0000, is_cid ? 0x9010000 : 0xA010000);
	int ret = xbox_emmc_wait_ints(1, 100);
	if (!ret)
	{
		for (int i = 0x10; i < 0x20; i += 4)
		{
			uint32_t data = XSPIReadDWORD_(i);
			memcpy(buf, &data, 4);
			buf += 4;
		}
	}
	return ret;
}

int xbox_emmc_write_block(int lba, uint8_t* buf)
{

	int ret = xbox_emmc_select_card();
	if (ret)
		return ret;
	ret = xbox_emmc_set_blocklen(0x200);
	if (ret)
		return ret;
	xbox_emmc_execute(0x10200, lba << 9, 0x183a0000);

	ret = xbox_emmc_wait_ints(1, 10000);

	ClearOutputBuffer();
	for (int i = 0; i < 0x200; i += 4)
	{
		uint32_t data;
		memcpy(&data, buf + i, 4);
		XSPIQueueBytes_(0x20, data);
	}
	SetAnswerFast();
	SendBytesToDevice();
	ret = xbox_emmc_wait_ints(0x12, 1500);

	//xbox_emmc_deselect_card();
	return ret;
}

bool emmc_init() {
	XSPIWriteWORD_(0x2C, XSPIReadDWORD_(0x2C) | (1 << 24));
	int init_timeout = 5000, t = 0;
	while (t < init_timeout)
	{
		if (XSPIReadDWORD_(0x3C) & 0x1000000)
			break;
	}
	if (t  == (init_timeout))
	{
		return SD_ERR_TIMEOUT;
	}
	is_selected = false;
	is_block_set = false;
	return SD_OK;

}
bool xbox_emmc_deselect_card() {
	// Deselect (CMD7 w/ 0 arg)
	xbox_emmc_execute(0, 0, 0x7000000);

	int timeout = 5000, t = 0;
	while (t++ < timeout) {
		if (XSPIReadWORD_(0x30) & 1) {
			return true;

		}
		Sleep(1);
	}
	return false;
}
bool CMD7_SET = true;

bool emmc_set_blocklen() {

	ClearOutputBuffer();
	XSPIQueueBytes_(0x30, 0xFFFFFFFF);    // Clear status
	//========================CLEAR STATUS REG ABOVE================================
	XSPIQueueBytes_(0x08, 0x200);         // 512 bytes
	XSPIQueueBytes_(0x0C, 0x10A00010);    // CMD16 flag
	SendBytesToDevice();
	int timeout = 5000, t = 0;
	while (t++ < timeout) {
		if (XSPIReadWORD_(0x30) & 1) break;
		Sleep(1);
	}
	//========================SET BLOCK LEN ABOVE===================================
	ClearOutputBuffer();
	XSPIQueueBytes_(0x30, 0xFFFFFFFF);    // Clear status
	//========================CLEAR STATUS REG ABOVE================================
	XSPIQueueBytes_(0x04, 0x200);         // 512 bytes (may be redundant)
	XSPIQueueBytes_(0x08, 0xFFFF0000);    // RCA (argument)
	XSPIQueueBytes_(0x0C, 0x71A0000);     // CMD7 flag
	//========================SELECT CARD ABOVE=====================================
	SendBytesToDevice();
	t = 0;
	while (t++ < timeout) {
		if (XSPIReadWORD_(0x30) & 1)
			XSPIWriteWORD_(0x30, 0xFFFFFFFF);
		break;
		Sleep(1);
	}

	return true;
}

int xbox_emmc_read_cid(uint8_t* cid)
{
	xbox_emmc_deselect_card();
	return xbox_emmc_read_cid_csd(cid, 1);
}
int xbox_emmc_read_csd(uint8_t* csd)
{
	xbox_emmc_deselect_card();
	return xbox_emmc_read_cid_csd(csd, 0);
}





int xbox_emmc_read_block_ext_csd(unsigned char* buf, int block, int is_block)
{
	int ret = xbox_emmc_select_card();
	if (ret)
		return ret;
	if (is_block)
	{
		ret = xbox_emmc_set_blocklen(0x200);
		if (ret)
		{
			xbox_emmc_deselect_card();
			return ret;
		}
	}
	xbox_emmc_execute(0x10200, block << 9, is_block ? 0x113a0010 : 0x83A0010);
	ret = xbox_emmc_wait_ints(0x21, 1500);
	if (!ret)
	{
		XSPIReadBlock_(0x20, buf, 128);
	}
	//xbox_emmc_deselect_card();
	return ret;
}

int xbox_emmc_read_block(int lba, unsigned char* buf)
{
	return xbox_emmc_read_block_ext_csd(buf, lba, 1);
}
int xbox_emmc_read_ext_csd(uint8_t* ext_csd)
{
	return xbox_emmc_read_block_ext_csd(ext_csd, 0, 0);
}