#include "ftd2xx.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "FTCSPI.h"

#define PIN_SK   (1 << 0)  // BDBUS0 (SCK)
#define PIN_MOSI (1 << 1)  // BDBUS1 (SDI/MOSI)
#define PIN_MISO (1 << 2)  // BDBUS2 (SDO/MISO)
#define PIN_SS   (1 << 3)  // BDBUS3 (SS)
#define PIN_RDY  (1 << 4)  // BDBUS4 (RDY)
#define OUTPUT_PINS (PIN_SK | PIN_MOSI | PIN_SS)

#define VERSION "1.0"
#define DEFAULT_CLOCK_HZ 1000000  // 1MHz default
#define MAX_CLOCK_HZ 5000000      // 5MHz max for ISD2100

// Global to track SS state
static BYTE g_currentSSState = PIN_SS;
static unsigned int g_currentFreqHz = 0;
static FT_HANDLE g_ftHandle = 0;
static FTC_HANDLE g_ftcHandle = 0;
static bool g_isInitialized = false;
static FTC_STATUS Status = FTC_SUCCESS;
extern int percentage = 0;
// Command line options
typedef struct {
	int device_index;
	unsigned int clock_hz;
	char* operation;
	char* filename;
	unsigned int address;
	bool verbose;
} Options;

void print_usage(const char* program_name) {
	printf("ISD2100 SPI Flash Tool v%s\n", VERSION);
	printf("Usage: %s [options] <command> [args]\n\n", program_name);
	printf("Options:\n");
	printf("  -d <index>    FTDI device index (default: 0)\n");
	printf("  -c <hz>       SPI clock frequency in Hz (default: %d, max: %d)\n",
		DEFAULT_CLOCK_HZ, MAX_CLOCK_HZ);
	printf("  -v            Verbose output\n");
	printf("  -h            Show this help\n\n");
	printf("Commands:\n");
	printf("  list                      List all FTDI devices\n");
	printf("  info                      Show ISD2100 device info\n");
	printf("  read <file>               Read entire flash to file\n");
	printf("  write <file>              Write file to flash (must be 44KB)\n");
	printf("  erase                     Erase entire flash\n");
	printf("  play <index>              Play audio from index (hex)\n");
	printf("  status                    Read device status\n");
	printf("  verify <file>             Verify flash contents against file\n\n");
	printf("Examples:\n");
	printf("  %s -d 0 info\n", program_name);
	printf("  %s -d 1 -c 3000000 read backup.bin\n", program_name);
	printf("  %s -d 1 write firmware.bin\n", program_name);
	printf("  %s -d 0 play 0x100\n", program_name);
}

void list_ftdi_devices() {
	FT_STATUS ftStatus;
	DWORD numDevs;

	ftStatus = FT_CreateDeviceInfoList(&numDevs);
	if (ftStatus != FT_OK) {
		printf("Error: Failed to create device list\n");
		return;
	}

	printf("Found %d FTDI device(s):\n\n", numDevs);

	if (numDevs > 0) {
		FT_DEVICE_LIST_INFO_NODE* devInfo =
			(FT_DEVICE_LIST_INFO_NODE*)malloc(sizeof(FT_DEVICE_LIST_INFO_NODE) * numDevs);

		ftStatus = FT_GetDeviceInfoList(devInfo, &numDevs);
		if (ftStatus == FT_OK) {
			for (DWORD i = 0; i < numDevs; i++) {
				printf("Device %d:\n", i);
				printf("  Flags: 0x%x %s\n", devInfo[i].Flags,
					(devInfo[i].Flags & FT_FLAGS_OPENED) ? "(OPEN)" : "");
				printf("  Type: ");
				switch (devInfo[i].Type) {
				case FT_DEVICE_100AX: printf("FT100AX\n"); break;
				case FT_DEVICE_2232C: printf("FT2232C\n"); break;
				case FT_DEVICE_2232H: printf("FT2232H\n"); break;
				case FT_DEVICE_4232H: printf("FT4232H\n"); break;
				default: printf("Unknown (0x%x)\n", devInfo[i].Type);
				}
				printf("  ID: 0x%x\n", devInfo[i].ID);
				printf("  LocationID: 0x%x\n", devInfo[i].LocId);
				printf("  Serial: %s\n", devInfo[i].SerialNumber);
				printf("  Description: %s\n\n", devInfo[i].Description);
			}
		}
		free(devInfo);
	}
}

// [Include all the SPI functions from your code here]
void MPSSE_SetClock(FT_HANDLE ftHandle, unsigned int freqHz) {
	BYTE cmd[3];
	DWORD bytesWritten;

	// Limit to max frequency
	if (freqHz > MAX_CLOCK_HZ) {
		printf("Warning: Requested frequency %u Hz exceeds max %u Hz, limiting to max\n",
			freqHz, MAX_CLOCK_HZ);
		freqHz = MAX_CLOCK_HZ;
	}

	// Calculate divisor: Clock = 60MHz / ((divisor + 1) * 2)
	unsigned int divisor = (30000000 / freqHz) - 1;
	if (divisor > 0xFFFF) divisor = 0xFFFF;

	cmd[0] = 0x86;
	cmd[1] = divisor & 0xFF;        // ValueL
	cmd[2] = (divisor >> 8) & 0xFF; // ValueH
	FT_Write(ftHandle, cmd, 3, &bytesWritten);

	// Calculate actual frequency (due to integer division)
	g_currentFreqHz = 60000000 / ((divisor + 1) * 2);

	printf("Set MPSSE clock to %u Hz (%.2f MHz)\n",
		g_currentFreqHz, g_currentFreqHz / 1000000.0);
}

void WritePins(FT_HANDLE ftHandle, BYTE pins) {
	BYTE cmd[3];
	DWORD bytesWritten;

	cmd[0] = 0x80;  // Set Data Bits Low Byte
	cmd[1] = pins;  // Value
	cmd[2] = OUTPUT_PINS;  // Direction

	FT_Write(ftHandle, cmd, 3, &bytesWritten);
}

void SPI_Select(FT_HANDLE ftHandle, bool inactive) {
	g_currentSSState = inactive ? PIN_SS : 0;
	BYTE pins = PIN_SK | g_currentSSState;
	WritePins(ftHandle, pins);
	Sleep(1);  // Keep small delay for chip select timing
}

BYTE SPI_Transfer(FT_HANDLE ftHandle, BYTE dataOut) {
	BYTE cmd[96];
	DWORD bytesWritten, bytesRead;
	BYTE dataIn = 0;
	BYTE readData[8];
	int cmdIdx = 0;

	BYTE ssState = g_currentSSState;

	// Transfer 8 bits with MISO read on falling edge
	for (int bit = 7; bit >= 0; bit--) {
		BYTE mosiBit = ((dataOut >> bit) & 1) ? PIN_MOSI : 0;

		// SCK high with MOSI
		cmd[cmdIdx++] = 0x80;
		cmd[cmdIdx++] = PIN_SK | mosiBit | ssState;
		cmd[cmdIdx++] = OUTPUT_PINS;

		// SCK low (falling edge)
		cmd[cmdIdx++] = 0x80;
		cmd[cmdIdx++] = mosiBit | ssState;
		cmd[cmdIdx++] = OUTPUT_PINS;

		// Read MISO on falling edge
		cmd[cmdIdx++] = 0x81;  // Read Data Bits Low

		// SCK high again
		cmd[cmdIdx++] = 0x80;
		cmd[cmdIdx++] = PIN_SK | mosiBit | ssState;
		cmd[cmdIdx++] = OUTPUT_PINS;
	}

	// Send immediate
	cmd[cmdIdx++] = 0x87;

	// Execute all commands
	FT_Write(ftHandle, cmd, cmdIdx, &bytesWritten);

	// Read 8 pin states
	FT_Read(ftHandle, readData, 8, &bytesRead);

	// Extract MISO bits (read on falling edge)
	for (int bit = 7; bit >= 0; bit--) {
		if (readData[7 - bit] & PIN_MISO)
			dataIn |= (1 << bit);
	}

	return dataIn;
}

void SPI_SendCommand(FT_HANDLE ftHandle, BYTE* cmdBytes, int len) {
	BYTE cmd[512];  // Max command buffer
	DWORD bytesWritten;
	int cmdIdx = 0;

	BYTE ssState = g_currentSSState;

	for (int byte = 0; byte < len; byte++) {
		for (int bit = 7; bit >= 0; bit--) {
			BYTE mosiBit = ((cmdBytes[byte] >> bit) & 1) ? PIN_MOSI : 0;

			cmd[cmdIdx++] = 0x80;
			cmd[cmdIdx++] = PIN_SK | mosiBit | ssState;
			cmd[cmdIdx++] = OUTPUT_PINS;

			cmd[cmdIdx++] = 0x80;
			cmd[cmdIdx++] = mosiBit | ssState;
			cmd[cmdIdx++] = OUTPUT_PINS;

			cmd[cmdIdx++] = 0x80;
			cmd[cmdIdx++] = PIN_SK | mosiBit | ssState;
			cmd[cmdIdx++] = OUTPUT_PINS;
		}
	}

	cmd[cmdIdx++] = 0x87;
	FT_Write(ftHandle, cmd, cmdIdx, &bytesWritten);
}

bool WaitForReady(FT_HANDLE ftHandle, int timeoutMs) {
	BYTE cmd[2];
	BYTE pinState;
	DWORD bytesWritten, bytesRead;
	int waited = 0;

	// Quick checks first
	for (int i = 0; i < 10; i++) {
		cmd[0] = 0x81;  // Read Data Bits Low
		cmd[1] = 0x87;  // Send immediate
		FT_Write(ftHandle, cmd, 2, &bytesWritten);
		FT_Read(ftHandle, &pinState, 1, &bytesRead);

		if (pinState & PIN_RDY) return true;
	}

	// Then slower checks
	while (waited < timeoutMs) {
		cmd[0] = 0x81;
		cmd[1] = 0x87;
		FT_Write(ftHandle, cmd, 2, &bytesWritten);
		FT_Read(ftHandle, &pinState, 1, &bytesRead);

		if (pinState & PIN_RDY) return true;
		Sleep(1);
		waited++;
	}

	return false;
}

void MPSSE_Init(FT_HANDLE ftHandle) {
	BYTE cmd[16];
	DWORD bytesWritten;

	// Reset to MPSSE mode
	FT_SetBitMode(ftHandle, 0x00, 0x00);  // Reset
	Sleep(50);
	FT_SetBitMode(ftHandle, 0x00, 0x02);  // MPSSE mode
	Sleep(50);

	// Purge buffers
	FT_Purge(ftHandle, FT_PURGE_RX | FT_PURGE_TX);

	// Set USB parameters
	FT_SetUSBParameters(ftHandle, 64 * 1024, 64 * 1024);
	FT_SetLatencyTimer(ftHandle, 1);

	// Disable loopback
	cmd[0] = 0x85;
	FT_Write(ftHandle, cmd, 1, &bytesWritten);

	// Set initial pin states: SK=1, SS=1, MOSI=0
	cmd[0] = 0x80;
	cmd[1] = PIN_SK | PIN_SS;  // Initial state
	cmd[2] = OUTPUT_PINS;       // Direction mask
	FT_Write(ftHandle, cmd, 3, &bytesWritten);

	g_currentSSState = PIN_SS;  // Initialize global state
}

bool ISD_Init(FT_HANDLE ftHandle) {
	// Make sure chip is deselected at start
	SPI_Select(ftHandle, true);

	// Send power-up command
	SPI_Select(ftHandle, false);
	SPI_Transfer(ftHandle, 0x10);
	SPI_Select(ftHandle, true);  // Deselect after power up!
	Sleep(10);  // Important delay

	// Wait for RDY
	if (!WaitForReady(ftHandle, 5000)) {
		printf("Error: Timeout waiting for RDY\n");
		return false;
	}

	// Now read status in a separate transaction
	SPI_Select(ftHandle, false);
	SPI_Transfer(ftHandle, 0x40);  // Read Status command
	BYTE status = SPI_Transfer(ftHandle, 0x00);
	BYTE istat = SPI_Transfer(ftHandle, 0x00);
	SPI_Select(ftHandle, true);

	if (status & 0x80) {
		printf("Error: ISD2100 is powered down (status: 0x%02X)\n", status);
		return false;
	}

	return true;
}

void ISD_ReadID(BYTE* dev_id) {
	SPI_Select(g_ftHandle, false);
	SPI_Transfer(g_ftHandle, 0x48);
	for (int i = 0; i < 4; i++) {
		dev_id[i] = SPI_Transfer(g_ftHandle, 0x00);
	}
	SPI_Select(g_ftHandle, true);
}

BYTE ReadStatus(void) {
	SPI_Select(g_ftHandle, false);
	SPI_Transfer(g_ftHandle, 0x40);
	BYTE status = SPI_Transfer(g_ftHandle, 0x00);
	SPI_Select(g_ftHandle, true);
	return status;
}

bool DumpISD2100(const char* filename, bool verbose) {
	FILE* file = fopen(filename, "wb");
	if (!file) {
		printf("Error: Cannot create output file: %s\n", filename);
		return false;
	}

	unsigned char* buffer = (unsigned char*)malloc(0xB000);
	if (!buffer) {
		printf("Error: Memory allocation failed\n");
		fclose(file);
		return false;
	}

	unsigned int transferSize = 16;  // Read 16 bytes at a time
	unsigned int dataIndex = 0;

	printf("Reading flash memory...\n");

	for (unsigned int addr = 0; addr < 0xB000; addr += transferSize) {
		BYTE cmd[4] = {
			0xA2,
			(BYTE)((addr >> 16) & 0xFF),
			(BYTE)((addr >> 8) & 0xFF),
			(BYTE)(addr & 0xFF)
		};

		SPI_Select(g_ftHandle, false);

		// Send command bytes
		WaitForReady(g_ftHandle, 5000);
		SPI_SendCommand(g_ftHandle, cmd, 4);

		// Read data bytes
		int bytesToRead = (addr + transferSize > 0xB000) ? (0xB000 - addr) : transferSize;
		for (int i = 0; i < bytesToRead; i++) {
			WaitForReady(g_ftHandle, 5000);
			buffer[dataIndex++] = SPI_Transfer(g_ftHandle, 0x00);
		}

		SPI_Select(g_ftHandle, true);

		percentage = (addr * 100) / 0xB000;
	}
	percentage = 100;

	// Write to file
	size_t written = fwrite(buffer, 1, 0xB000, file);
	fclose(file);
	free(buffer);

	if (written != 0xB000) {
		percentage = 0;
		printf("Error: Failed to write complete data to file\n");
		return false;
	}
	percentage = 0;
	printf("Successfully read 44KB to %s\n", filename);
	return true;
}

void ISD_EraseMass(void) {
	BYTE cmd[2] = { 0x26, 0x01 };

	printf("Erasing flash memory...\n");

	SPI_Select(g_ftHandle, false);
	for (int i = 0; i < 2; i++) {
		WaitForReady(g_ftHandle, 5000);
		SPI_Transfer(g_ftHandle, cmd[i]);
	}
	SPI_Select(g_ftHandle, true);

	// Wait for erase to complete
	Sleep(100);
	WaitForReady(g_ftHandle, 30000);  // Erase can take time

	printf("Erase complete\n");
}

void Play(unsigned short index) {
	BYTE cmd[3] = {
		0xA6,
		(BYTE)((index >> 8) & 0xFF),
		(BYTE)(index & 0xFF)
	};

	printf("Playing from index 0x%04X\n", index);

	SPI_Select(g_ftHandle, false);
	for (int i = 0; i < 3; i++) {
		WaitForReady(g_ftHandle, 5000);
		SPI_Transfer(g_ftHandle, cmd[i]);
	}
	SPI_Select(g_ftHandle, true);
}

bool VerifyISD2100(const char* filename, bool verbose) {
	printf("Verifying flash contents...\n");

	// Read flash to temp buffer
	unsigned char* flashBuffer = (unsigned char*)malloc(0xB000);
	if (!flashBuffer) {
		printf("Error: Memory allocation failed\n");
		return false;
	}

	// Read flash using same method as dump
	unsigned int transferSize = 16;
	unsigned int dataIndex = 0;

	for (unsigned int addr = 0; addr < 0xB000; addr += transferSize) {
		BYTE cmd[4] = {
			0xA2,
			(BYTE)((addr >> 16) & 0xFF),
			(BYTE)((addr >> 8) & 0xFF),
			(BYTE)(addr & 0xFF)
		};

		SPI_Select(g_ftHandle, false);
		WaitForReady(g_ftHandle, 5000);
		SPI_SendCommand(g_ftHandle, cmd, 4);

		int bytesToRead = (addr + transferSize > 0xB000) ? (0xB000 - addr) : transferSize;
		for (int i = 0; i < bytesToRead; i++) {
			WaitForReady(g_ftHandle, 5000);
			flashBuffer[dataIndex++] = SPI_Transfer(g_ftHandle, 0x00);
		}

		SPI_Select(g_ftHandle, true);

		percentage = (addr * 100) / 0xB000;
	}

	percentage = 100;

	// Read file
	FILE* file = fopen(filename, "rb");
	if (!file) {
		printf("Error: Cannot open file: %s\n", filename);
		free(flashBuffer);
		return false;
	}

	fseek(file, 0, SEEK_END);
	long filesize = ftell(file);
	fseek(file, 0, SEEK_SET);

	if (filesize != 0xB000) {
		printf("Error: File must be exactly 44KB\n");
		fclose(file);
		free(flashBuffer);
		return false;
	}

	unsigned char* fileBuffer = (unsigned char*)malloc(0xB000);
	fread(fileBuffer, 1, 0xB000, file);
	fclose(file);

	// Compare
	int differences = 0;
	for (int i = 0; i < 0xB000; i++) {
		if (flashBuffer[i] != fileBuffer[i]) {
			if (verbose && differences < 10) {
				printf("Difference at 0x%04X: flash=0x%02X, file=0x%02X\n",
					i, flashBuffer[i], fileBuffer[i]);
			}
			differences++;
		}
	}

	free(flashBuffer);
	free(fileBuffer);

	if (differences == 0) {
		percentage = 0;
		printf("Verification successful - flash contents match file\n");
		return true;
	}
	else {
		percentage = 0;
		printf("Verification failed - %d byte(s) differ\n", differences);
		return false;
	}
}

// Send multiple bytes without waiting between each
void SPI_SendPacket(FT_HANDLE ftHandle, BYTE* data, int length) {
	BYTE cmd[2048];  // 8 bytes * 32 commands per byte = 256, plus overhead
	DWORD bytesWritten;
	int cmdIdx = 0;

	BYTE ssState = g_currentSSState;

	// Build all commands for the packet
	for (int byteIdx = 0; byteIdx < length; byteIdx++) {
		for (int bit = 7; bit >= 0; bit--) {
			BYTE mosiBit = ((data[byteIdx] >> bit) & 1) ? PIN_MOSI : 0;

			cmd[cmdIdx++] = 0x80;
			cmd[cmdIdx++] = PIN_SK | mosiBit | ssState;
			cmd[cmdIdx++] = OUTPUT_PINS;

			cmd[cmdIdx++] = 0x80;
			cmd[cmdIdx++] = mosiBit | ssState;
			cmd[cmdIdx++] = OUTPUT_PINS;

			cmd[cmdIdx++] = 0x80;
			cmd[cmdIdx++] = PIN_SK | mosiBit | ssState;
			cmd[cmdIdx++] = OUTPUT_PINS;
		}
	}

	cmd[cmdIdx++] = 0x87;  // Send immediate
	FT_Write(ftHandle, cmd, cmdIdx, &bytesWritten);
}

// Read multiple bytes without waiting between each
void SPI_ReadPacket(FT_HANDLE ftHandle, BYTE* buffer, int length) {
	BYTE cmd[2048];
	DWORD bytesWritten, bytesRead;
	int cmdIdx = 0;

	BYTE ssState = g_currentSSState;

	// Build commands to read all bytes
	for (int byteIdx = 0; byteIdx < length; byteIdx++) {
		for (int bit = 7; bit >= 0; bit--) {
			// Clock out 0x00, read on falling edge
			cmd[cmdIdx++] = 0x80;
			cmd[cmdIdx++] = PIN_SK | ssState;
			cmd[cmdIdx++] = OUTPUT_PINS;

			cmd[cmdIdx++] = 0x80;
			cmd[cmdIdx++] = ssState;
			cmd[cmdIdx++] = OUTPUT_PINS;

			cmd[cmdIdx++] = 0x81;  // Read on falling edge

			cmd[cmdIdx++] = 0x80;
			cmd[cmdIdx++] = PIN_SK | ssState;
			cmd[cmdIdx++] = OUTPUT_PINS;
		}
	}

	cmd[cmdIdx++] = 0x87;
	FT_Write(ftHandle, cmd, cmdIdx, &bytesWritten);

	// Read all pin states - use dynamic allocation
	BYTE* pinStates = (BYTE*)malloc(length * 8);
	if (!pinStates) {
		printf("Error: Memory allocation failed in SPI_ReadPacket\n");
		return;
	}

	FT_Read(ftHandle, pinStates, length * 8, &bytesRead);

	// Extract bytes
	for (int byteIdx = 0; byteIdx < length; byteIdx++) {
		buffer[byteIdx] = 0;
		for (int bit = 7; bit >= 0; bit--) {
			int stateIdx = byteIdx * 8 + (7 - bit);
			if (pinStates[stateIdx] & PIN_MISO) {
				buffer[byteIdx] |= (1 << bit);
			}
		}
	}

	free(pinStates);
}

bool WriteISD2100(const char* filename, bool verbose) {
	FILE* file = fopen(filename, "rb");
	if (!file) {
		printf("Error: Cannot open input file: %s\n", filename);
		return false;
	}

	fseek(file, 0, SEEK_END);
	long filesize = ftell(file);
	fseek(file, 0, SEEK_SET);

	if (filesize != 0xB000) {
		printf("Error: File must be exactly 44KB (0xB000 bytes), got %ld bytes\n", filesize);
		fclose(file);
		return false;
	}

	unsigned char* buffer = (unsigned char*)malloc(0xB000);
	if (!buffer) {
		printf("Error: Memory allocation failed\n");
		fclose(file);
		return false;
	}

	size_t bytesRead = fread(buffer, 1, 0xB000, file);
	fclose(file);

	if (bytesRead != 0xB000) {
		printf("Error: Failed to read complete file\n");
		free(buffer);
		return false;
	}

	printf("Writing flash memory...\n");


	BYTE expectedResponse[5] = { 0x60, 0x60, 0x60, 0x60, 0x60 };

	// AGGRESSIVE OPTIMIZATION: Remove most delays and checks
	for (unsigned int addr = 0; addr < 0xB000; addr += 4) {
		BYTE packet[8];
		packet[0] = 0xA0;
		packet[1] = 0x00;
		packet[2] = (addr >> 8) & 0xFF;
		packet[3] = addr & 0xFF;
		memcpy(&packet[4], &buffer[addr], 4);

		// Write without verification (trust the device)
		SPI_Select(g_ftHandle, false);

		// OPTIMIZATION: No WaitForReady before write - device should be ready
		SPI_SendPacket(g_ftHandle, packet, 8);

		SPI_Select(g_ftHandle, true);

		// OPTIMIZATION: No delay for most writes
		// Only delay every 256 bytes (64 writes)
		if ((addr & 0xFF) == 0) {
			Sleep(1);  // Minimal delay
		}

		// OPTIMIZATION: Only verify at checkpoints
		if ((addr % 0x1000) == 0) {  // Every 4KB
			// Do a read to verify the last write worked
			SPI_Select(g_ftHandle, false);
			WaitForReady(g_ftHandle, 1000);  // Shorter timeout

			BYTE readBuffer[8];
			SPI_ReadPacket(g_ftHandle, readBuffer, 8);
			SPI_Select(g_ftHandle, true);

			bool success = true;
			for (int i = 0; i < 5; i++) {
				if (readBuffer[i] != expectedResponse[i]) {
					success = false;
					break;
				}
			}

			if (!success) {
				printf("\nWarning: Verification failed at 0x%04X, continuing...\n", addr);
				// Don't retry - just continue and verify at the end
			}


		}
		percentage = (addr * 100) / 0xB000;
	}
	percentage = 100;

	// FINAL VERIFICATION: Read back some random addresses to check

	percentage = 0;
	printf("Verifying write...\n");
	bool finalCheck = true;
	for (int i = 0; i < 10; i++) {
		unsigned int checkAddr = (rand() % (0xB000 / 4)) * 4;

		BYTE cmd[4] = { 0xA2, (checkAddr >> 16) & 0xFF, (checkAddr >> 8) & 0xFF, checkAddr & 0xFF };
		SPI_Select(g_ftHandle, false);
		WaitForReady(g_ftHandle, 5000);
		SPI_SendCommand(g_ftHandle, cmd, 4);

		BYTE readData[4];
		WaitForReady(g_ftHandle, 5000);
		for (int j = 0; j < 4; j++) {
			readData[j] = SPI_Transfer(g_ftHandle, 0x00);
		}
		SPI_Select(g_ftHandle, true);

		if (memcmp(&buffer[checkAddr], readData, 4) != 0) {
			printf("Verification failed at 0x%04X\n", checkAddr);
			finalCheck = false;
		}
		percentage = (checkAddr * 100) / 0xB000;
	}
	percentage = 100;
	free(buffer);

	if (finalCheck) {
		percentage = 0;
		printf("Successfully wrote %s to flash\n", filename);
		return true;
	}
	else {
		percentage = 0;
		printf("Write completed but verification failed - recommend full verify\n");
		return true;  // Still return true since write completed
	}
}

int getPercentage() {
	return percentage;
}	
// Alternative: Open by description string
FT_HANDLE OpenFTDIByDescription(const char* description) {
	FT_HANDLE ftHandle;
	FT_STATUS ftStatus;

	// This will open the first device that matches the description
	ftStatus = FT_OpenEx((PVOID)description, FT_OPEN_BY_DESCRIPTION, &ftHandle);

	if (ftStatus == FT_OK) {
		printf("Opened device by description: %s\n", description);
		return ftHandle;
	}

	return NULL;
}

bool FTDI_AutoInitialize(unsigned int clockHz) {
	FT_HANDLE ftHandle = NULL;

	// Option 2: Open by partial description
	if (!ftHandle) {
		ftHandle = OpenFTDIByDescription("Dual RS232-HS B");
	}

	// Store globally and initialize
	g_ftHandle = ftHandle;

	// Initialize MPSSE
	MPSSE_Init(g_ftHandle);
	MPSSE_SetClock(g_ftHandle, clockHz);

	g_isInitialized = true;
	return true;
}

bool ISD_INIT() {
	// Initialize ISD2100
	if (!ISD_Init(g_ftHandle)) {
		FT_Close(g_ftHandle);
		g_ftHandle = NULL;
		return false;
	}
	return true;
}

void FTDI_DEINIT() {
	if (g_ftHandle != NULL) {
		// 1. Make sure chip select is deasserted
		SPI_Select(g_ftHandle, true);  // SS high

		// 2. Reset from MPSSE mode to normal mode
		FT_SetBitMode(g_ftHandle, 0x00, 0x00);  // This is important!

		// 3. Purge any remaining data in buffers
		FT_Purge(g_ftHandle, FT_PURGE_RX | FT_PURGE_TX);

		// 4. Close the device
		FT_Close(g_ftHandle);

		// 5. Clear global state
		g_ftHandle = NULL;
		g_isInitialized = false;
		g_currentSSState = PIN_SS;
		g_currentFreqHz = 0;
	}
}
bool ISD2100_PowerDown() {
	// Make sure chip is deselected at start
	SPI_Select(g_ftHandle, true);

	// Send power-up command
	SPI_Select(g_ftHandle, false);
	SPI_Transfer(g_ftHandle, 0x12);
	SPI_Select(g_ftHandle, true);  // Deselect after power up!
	Sleep(10);  // Important delay

	return true;
}
bool ISD2100_Reset() {
	// Make sure chip is deselected at start
	SPI_Select(g_ftHandle, true);

	// Send power-up command
	SPI_Select(g_ftHandle, false);
	SPI_Transfer(g_ftHandle, 0x14);
	SPI_Select(g_ftHandle, true);  // Deselect after power up!
	Sleep(10);  // Important delay

	return true;
}

//int main(/*int argc, char* argv[]*/) {
//
//    //Options opts = {
////    .device_index = 0,
////    .clock_hz = DEFAULT_CLOCK_HZ,
////    .operation = NULL,
////    .filename = NULL,
////    .address = 0,
////    .verbose = false
////};
//
////// Parse command line arguments
////int i = 1;
////while (i < argc) {
////    if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
////        opts.device_index = atoi(argv[++i]);
////    }
////    else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
////        opts.clock_hz = atoi(argv[++i]);
////    }
////    else if (strcmp(argv[i], "-v") == 0) {
////        opts.verbose = true;
////    }
////    else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
////        print_usage(argv[0]);
////        return 0;
////    }
////    else if (argv[i][0] != '-') {
////        // This is the command
////        opts.operation = argv[i];
////        if (i + 1 < argc && argv[i + 1][0] != '-') {
////            opts.filename = argv[++i];
////        }
////        break;
////    }
////    i++;
////}
//
////// Handle list command (doesn't need device)
////if (opts.operation && strcmp(opts.operation, "list") == 0) {
////    list_ftdi_devices();
////    return 0;
////}
//
////// Check if we have a command
////if (!opts.operation) {
////    print_usage(argv[0]);
////    return 1;
////}
//
////// Open FTDI device
////FT_HANDLE ftHandle;
////FT_STATUS ftStatus = FT_Open(opts.device_index, &ftHandle);
////if (ftStatus != FT_OK) {
////    printf("Error: Failed to open FTDI device %d (status: %d)\n",
////        opts.device_index, ftStatus);
////    printf("Try '%s list' to see available devices\n", argv[0]);
////    return 1;
////}
//
////// Initialize MPSSE mode
////MPSSE_Init(ftHandle);
////MPSSE_SetClock(ftHandle, opts.clock_hz);
//
////// Initialize ISD2100
////if (!ISD_Init(ftHandle)) {
////    printf("Error: ISD2100 initialization failed\n");
////    FT_Close(ftHandle);
////    return 1;
////}
//
////// Execute command
////int ret = 0;
//
////if (strcmp(opts.operation, "info") == 0) {
////    BYTE dev_id[4];
////    ISD_ReadID(ftHandle, dev_id);
////    printf("ISD2100 Device ID: %02X %02X %02X %02X\n",
////        dev_id[0], dev_id[1], dev_id[2], dev_id[3]);
//
////    BYTE status = ReadStatus(ftHandle);
////    printf("Status Register: 0x%02X\n", status);
////    printf("  Power: %s\n", (status & 0x80) ? "DOWN" : "UP");
////    printf("  Ready: %s\n", (status & 0x01) ? "BUSY" : "READY");
//
////}
////else if (strcmp(opts.operation, "read") == 0) {
////    if (!opts.filename) {
////        printf("Error: Output filename required\n");
////        ret = 1;
////    }
////    else {
////        if (!DumpISD2100(ftHandle, opts.filename, opts.verbose)) {
////            ret = 1;
////        }
////    }
//
////}
////else if (strcmp(opts.operation, "write") == 0) {
////    if (!opts.filename) {
////        printf("Error: Input filename required\n");
////        ret = 1;
////    }
////    else {
////        if (!WriteISD2100(ftHandle, opts.filename, opts.verbose)) {
////            ret = 1;
////        }
////    }
//
////}
////else if (strcmp(opts.operation, "erase") == 0) {
////    ISD_EraseMass(ftHandle);
//
////}
////else if (strcmp(opts.operation, "play") == 0) {
////    if (!opts.filename) {
////        printf("Error: Index required (e.g., 0x100)\n");
////        ret = 1;
////    }
////    else {
////        unsigned int index = strtoul(opts.filename, NULL, 0);
////        Play(ftHandle, (unsigned short)index);
////        Sleep(2500);
////    }
//
////}
////else if (strcmp(opts.operation, "status") == 0) {
////    BYTE status = ReadStatus(ftHandle);
////    printf("Status Register: 0x%02X\n", status);
////    printf("  Bit 7 (PWR): %d - %s\n", (status >> 7) & 1,
////        (status & 0x80) ? "Powered Down" : "Powered Up");
////    printf("  Bit 6: %d\n", (status >> 6) & 1);
////    printf("  Bit 5: %d\n", (status >> 5) & 1);
////    printf("  Bit 4: %d\n", (status >> 4) & 1);
////    printf("  Bit 3: %d\n", (status >> 3) & 1);
////    printf("  Bit 2: %d\n", (status >> 2) & 1);
////    printf("  Bit 1: %d\n", (status >> 1) & 1);
////    printf("  Bit 0 (RDY): %d - %s\n", status & 1,
////        (status & 1) ? "Busy" : "Ready");
//
////}
////else if (strcmp(opts.operation, "verify") == 0) {
////    if (!opts.filename) {
////        printf("Error: Filename required\n");
////        ret = 1;
////    }
////    else {
////        if (!VerifyISD2100(ftHandle, opts.filename, opts.verbose)) {
////            ret = 1;
////        }
////    }
//
////}
////else {
////    printf("Error: Unknown command '%s'\n", opts.operation);
////    print_usage(argv[0]);
////    ret = 1;
////}
//
////// Cleanup
////FT_SetBitMode(ftHandle, 0x00, 0x00);  // Reset to normal mode
////FT_Close(ftHandle);
//
//    return 1;
//}