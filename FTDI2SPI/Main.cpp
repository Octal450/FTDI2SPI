#include "stdafx.h"

#include <windows.h>
#include "wrapper_spi.h"
#include "Flasher.h"
#include "sfc.h"
#include <sys/stat.h>
#include <string>
#include "XeMMC.h"


#define MIN(a, b)  (((a) < (b)) ? (a) : (b))
#define MAX(a, b)  (((a) > (b)) ? (a) : (b))

// unused, but lets leave it here
#define PRINT_PROGRESS printf( "\r%0.1f%%", ((block_flash-StartBlock)*100)/(float)(block_flash_max-StartBlock) )
#define PRINT_BLOCKS printf( "\rBlock: %X", (block_flash/32))

#include <iostream>
#include <fstream>
#include <chrono>
#include "ISD2100.h"
using namespace std;

unsigned int block_flash = 0;
unsigned int block_size;
unsigned int flashConfig = 0;
unsigned int End_Block = 32768;
bool start_spi = false;
bool stop = false;
unsigned int eMMCblocks = 0;

extern "C"
{
	__declspec(dllexport) int spi(int mode, int size, char* file, int startblock, int length) {
		unsigned char* flash_xbox;
		unsigned int addr_raw = 0;

		unsigned int StartBlock = startblock * 32;
		unsigned int block_flash_max = 2048 * size;

		if (length > 0) block_flash_max = StartBlock + (length * 32);

		start_spi = false;
		block_flash = 0;
		stop = false;

		bool Wrong_arg = false;
		bool FlashConfig = false;
		bool EraseEnable = false;
		bool ReadEnable = false;
		bool WriteEnable = false;
		bool PatchECCEnable = false;
		string FileName = file ? file : "";

		if (mode == 0) { // Get Flash Config Only
			FlashConfig = true;
		}
		else if (mode == 1) { // -r
			block_size = 0x210;
			ReadEnable = true;
		}
		else if (mode == 2) { // -R
			block_size = 0x200;
			ReadEnable = true;
		}
		else if (mode == 3) { // -w
			block_size = 0x210;
			WriteEnable = true;
		}
		else if (mode == 4) { // +w
			PatchECCEnable = true;
			block_size = 0x210;
			WriteEnable = true;
		}
		else if (mode == 5) { // -e
			EraseEnable = true;
		}

		//////////////////////////////////////////////////////////////////////////////////////////

		if (!spi_init()) {
			closeDevice();
			return -2; // UNABLE TO INIT FTDI
		}

		flashConfig = 0;
		FlashDataInit((unsigned char*)&flashConfig);


		unsigned char block[512];
		const uint32_t num_blocks = /* set this to your eMMC's total blocks */ 98432;

		if (((char*)&flashConfig)[0] == ((char*)&flashConfig)[1] &&
			((char*)&flashConfig)[1] == ((char*)&flashConfig)[2] &&
			((char*)&flashConfig)[2] == ((char*)&flashConfig)[3]) {
			closeDevice();
			return -3; // BAD CONNECTION TO NAND
		}

		if (SFC_init(flashConfig) != OK) {
			closeDevice();
			return -4; // UNKNOWN NAND
		}

		//////////////////////////////////////////////////////////////////////////////////////////

		if (FlashConfig) {
			closeDevice();
			return 0;
		}

		//////////////////////////////////////////////////////////////////////////////////////////

		if (EraseEnable) {
			End_Block = block_flash_max;

			start_spi = true;
			for (block_flash = StartBlock; block_flash < block_flash_max; block_flash += Sfc.PageCountInBlock) {
				if (stop) break;

				FlashDataErase(block_flash);
				//PRINT_BLOCKS;
			}
			//PRINT_BLOCKS;

			closeDevice();

			if (stop) return -1;
			else return 0;
		}

		//////////////////////////////////////////////////////////////////////////////////////////

		else if (WriteEnable) {
			FILE* fileW = fopen(FileName.c_str(), "rb");

			if (fileW == NULL) {
				closeDevice();
				return -8; // NO FILE
			}

			flash_xbox = (unsigned char*)malloc(block_flash_max * 528);

			if (flash_xbox == NULL) {
				closeDevice();
				return -9; // NO MEMORY
			}

			fseek(fileW, 0L, SEEK_END);
			int File_Size = ftell(fileW);
			fseek(fileW, 0L, SEEK_SET);

			unsigned int File_Blocks = File_Size / block_size;
			End_Block = MIN(block_flash_max, File_Blocks);

			memset(flash_xbox, 0, sizeof(flash_xbox));

			fseek(fileW, 0, SEEK_SET);
			fread((char*)flash_xbox, block_size, End_Block - StartBlock, fileW);

			start_spi = true;
			addr_raw = 0;
			for (block_flash = StartBlock; block_flash < End_Block; ++block_flash) {
				if (stop) break;

				//if ((block_flash % 64) == 0 || block_flash == StartBlock) {
				//	PRINT_BLOCKS;
				//}

				if (PatchECCEnable)
					fixSpare_ECC(&flash_xbox[addr_raw], block_flash);

				FlashDataWrite(&flash_xbox[addr_raw], block_flash, block_size);

				addr_raw += block_size;
			}
			//PRINT_BLOCKS;

			fclose(fileW);
			free(flash_xbox);

			closeDevice();

			if (stop) return -1;
			else return 0;
		}

		//////////////////////////////////////////////////////////////////////////////////////////

		else if (ReadEnable) {
			FILE* fileR = fopen(FileName.c_str(), "wb");
			if (fileR == NULL) {
				closeDevice();
				return -8; // NO FILE
			}

			flash_xbox = (unsigned char*)malloc(block_flash_max * block_size);

			if (flash_xbox == NULL) {
				closeDevice();
				return -9; // NO MEMORY
			}

			int byteWrite = fwrite((char*)flash_xbox, 1, 1, fileR);
			if (byteWrite != 1) {
				closeDevice();
				return -11; // COULDN'T OPEN FILE
			}
			fseek(fileR, 0L, SEEK_SET);

			End_Block = block_flash_max;

			start_spi = true;
			addr_raw = 0;
			for (block_flash = StartBlock; block_flash < block_flash_max; ++block_flash) {
				if (stop) break;

				FlashDataRead(&flash_xbox[addr_raw], block_flash, block_size);

				//if ((block_flash % 64) == 0 && (block_flash != 0)) {
				//	PRINT_BLOCKS;
				//}

				addr_raw += block_size;
			}

			if (stop) {
				closeDevice();
				fclose(fileR);
				free(flash_xbox);
				return -1;
			}

			unsigned int block_file = 0;
			if (addr_raw) {
				addr_raw = 0;
				int byteWrite;
				for (block_file = StartBlock; block_file < block_flash_max; ++block_file) {
					do {
						fseek(fileR, addr_raw, SEEK_SET);
						byteWrite = fwrite((char*)&flash_xbox[addr_raw], 1, block_size, fileR);
					} while (byteWrite != block_size);

					addr_raw += block_size;
				}
			}

			//PRINT_BLOCKS;
			fclose(fileR);
			free(flash_xbox);

			closeDevice();
			return 0;
		}

		closeDevice();
		return -10; // NO MODE
	}
	__declspec(dllexport) int emmc_read(const char* file, int startblock, int length) {

		if (!spi_init()) {
			closeDevice();
			return -2; // UNABLE TO INIT FTDI
		}
		flashConfig = 0;
		FlashDataInit((unsigned char*)&flashConfig);

		if (emmc_init()) {
			closeDevice();
			return -3; // EMMC INIT FAILED
		}
		const uint32_t block_size = 512;
		const uint32_t total_blocks = /* set appropriately */ length;
		const uint32_t StartBlock = startblock;
		const uint32_t BlockCount = (length > 0) ? length : (total_blocks - StartBlock);

		unsigned char* buffer = (unsigned char*)malloc(block_size);
		if (!buffer) {
			closeDevice();
			return -9; // NO MEMORY
		}
		FILE* fout = fopen(file, "wb");
		if (!fout) {
			free(buffer);
			closeDevice();
			return -8; // COULDN'T OPEN FILE
		}
		for (uint32_t i = 0; i < BlockCount; ++i) {
			uint32_t lba = StartBlock + i;
			eMMCblocks = lba;

			//printf("eMMC Block: %u\n", lba);
		if (xbox_emmc_read_block(lba, buffer)!=0) {
				printf("Failed to read block %u\n", lba);
				fclose(fout);
				free(buffer);
				closeDevice();
				return -20; // READ ERROR
			}
			if (fwrite(buffer, 1, block_size, fout) != block_size) {
				fclose(fout);
				free(buffer);
				closeDevice();
				return -21; // FILE WRITE ERROR
			}
		}
		if (false) {
			return -22; //DESELECT ERROR
		}
		fclose(fout);
		free(buffer);
		closeDevice();
		return 0;
	}
	__declspec(dllexport) int emmc_write(const char* file, int startblock) {

		const uint32_t block_size = 512;
		unsigned char* buffer = (unsigned char*)malloc(block_size);

		if (!spi_init()) {
			closeDevice();
			return -2; // UNABLE TO INIT FTDI
		}

		flashConfig = 0;
		FlashDataInit((unsigned char*)&flashConfig);

		if (emmc_init()) {
			closeDevice();
			return -3; // EMMC INIT FAILED
		}

		if (!buffer) {
			closeDevice();
			return -9; // NO MEMORY
		}
		FILE* fin = fopen(file, "rb");
		if (!fin) {
			free(buffer);
			closeDevice();
			return -8; // COULDN'T OPEN FILE
		}
		// Get file size
		fseek(fin, 0, SEEK_END);
		long filesize = ftell(fin);
		if (filesize < 0) {
			fclose(fin);
			free(buffer);
			closeDevice();
			return -30; // FILE SIZE ERROR
		}
		rewind(fin);

		uint32_t BlockCount = (filesize + block_size - 1) / block_size; // Round up if not a multiple
		for (uint32_t i = 0; i < BlockCount; ++i) {
			size_t n = fread(buffer, 1, block_size, fin);
			if (n != block_size) {
				printf("File too short or read error at block %u\n", i);
				fclose(fin);
				free(buffer);
				closeDevice();
				return -21; // FILE READ ERROR
			}
			
			uint32_t lba = startblock + i;
			eMMCblocks = lba; // If this is needed for progress
			int ret = xbox_emmc_write_block(lba, buffer);
			if (ret != 0) {
				printf("Failed to write block %u\n", lba);
				fclose(fin);
				free(buffer);
				closeDevice();
				return -20; // WRITE ERROR
			}
		}
		if (false) {
			return -22; //DESELECT ERROR
		}
		fclose(fin);
		free(buffer);
		closeDevice();
		return 0;
	}
	__declspec(dllexport) int spiGetBlocks() {
		if (start_spi) {
			return block_flash / 32; // Decimal not hex
		}
		else {
			return -1;
		}
	}
	__declspec(dllexport) int spiGetConfig() {
		return flashConfig; // Decimal not hex
	}
	__declspec(dllexport) unsigned int emmcGetBlocks() {
		return eMMCblocks; // Decimal not hex
	}
	__declspec(dllexport) void spiStop() {
		if (start_spi) stop = true;
	}
	__declspec(dllexport) int progressISD2100() {
		return getPercentage();
	}
	__declspec(dllexport) bool FTDI_INIT(int clockHz) {
		return FTDI_AutoInitialize(clockHz);
	}
	__declspec(dllexport) void ISD2100_Readback(const char* path, bool verbose) {
		DumpISD2100(path, verbose);
	}
	__declspec(dllexport) void ISD2100_Play(int index) {
		Play((unsigned short)index);
		Sleep(2500);
	}
	__declspec(dllexport) void ISD_AUTO_INIT() {
		ISD_INIT();
	}
	__declspec(dllexport) void ISD2100_Wipe() {
		ISD_EraseMass();
	}
	__declspec(dllexport) bool ISD2100_Write(const char* filename, bool verbose) {
		return WriteISD2100(filename, verbose);
	}
	__declspec(dllexport) bool ISD2100_Verify(const char* filename, bool verbose) {
		return VerifyISD2100(filename, verbose);
	}
	__declspec(dllexport) void FTDI_DEINIT_IMMEDIATELY() {
		FTDI_DEINIT();
	}
	__declspec(dllexport) void ISD2100_DeInitialize() {
		ISD2100_PowerDown();
	}
	__declspec(dllexport) void ISD2100_Reboot() {
		ISD2100_Reset();
	}
}

int main()
{
	FTDI_INIT(1000000); // Initialize ISD2100 with 1MHz clock

	ISD_AUTO_INIT(); // Initialize ISD2100
	ISD2100_Play(6); // Play from index 6
	ISD2100_Write("C:\\Users\\Mena\\Desktop\\FTDI2SPI_emmc-master\\FTDI2SPI-master\\Debug\\Leon.bin", true);
	printf("EXE Mode unavailable");
	return 1;
}
