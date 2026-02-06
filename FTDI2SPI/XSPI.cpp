
#include "stdafx.h"
#include <windows.h>

#include "XSPI.h"
#include "wrapper_spi.h"
#include <stdio.h>

#define GPIO_DIR_XBOX		0x03
#define GPIO_EJ_XBOX		0x02
#define GPIO_XX_XBOX		0x01

unsigned char GPIO_STATUS = 0x00;

unsigned char reverse( unsigned char n )
{
	n = ((n >> 1) & 0x55) | ((n << 1) & 0xaa) ;
	n = ((n >> 2) & 0x33) | ((n << 2) & 0xcc) ;
	n = ((n >> 4) & 0x0f) | ((n << 4) & 0xf0) ;
	return n;
}

void reverse_array( unsigned char *buf, unsigned int size )
{
	unsigned int i;

	for( i=0; i!=size; ++i )
	{
		buf[i] = reverse( buf[i] );
	}
}

void XSPIInit()
{
}

void XSPIPowerUp()
{
}

void XSPIShutdown()
{

}

void XSPIEnterFlashMode()
{
	spi_setGPIO( false, true );
	spi_SetCS( true );
	Sleep(35);	
	spi_setGPIO( false, false );
	spi_SetCS( false );
	Sleep(35);	
	spi_setGPIO( true, true );
	Sleep(35);	
	

}

void XSPILeaveFlashMode()
{

}

void XSPIRead_(unsigned char reg, unsigned char Data[])
{
	unsigned char writeBuf[2] = { (reg << 2) | 1, 0xFF };	
	//6 byte			
	EnableSPIChip();
	AddWriteOutBuffer( sizeof(writeBuf)*8, writeBuf );
	AddReadOutBuffer( 4*8 );	
	DisableSPIChip();
	
}

void XSPIRead_sync(unsigned char reg, unsigned char Data[])
{
	unsigned char writeBuf[2] = { (reg << 2) | 1, 0xFF };	
		
	ClearOutputBuffer();
	
	EnableSPIChip();
	AddWriteOutBuffer( sizeof(writeBuf)*8, writeBuf );
	AddReadOutBuffer( 4*8 );	
	DisableSPIChip();

	SetAnswerFast();
	SendBytesToDevice();
	GetDataFromDevice( 4, Data );
	
}

unsigned int XSPIReadWORD_(unsigned char reg)
{
	unsigned char res[2] = {0,0};
	unsigned char writeBuf[2] = { (reg << 2) | 1, 0xFF };	

	ClearOutputBuffer();
		
	EnableSPIChip();
	AddWriteOutBuffer( sizeof(writeBuf)*8, writeBuf );
	AddReadOutBuffer( 2*8 );	
	DisableSPIChip();

	SetAnswerFast();
	SendBytesToDevice();
	GetDataFromDevice( 2, res );

	return res[0] | ((unsigned int)res[1]<<8);
}

unsigned int XSPIReadDWORD_(unsigned char reg)
{
	unsigned char res[4] = { 0, 0, 0, 0 };
	unsigned char writeBuf[2] = { (reg << 2) | 1, 0xFF };

	ClearOutputBuffer();

	EnableSPIChip();
	AddWriteOutBuffer(sizeof(writeBuf) * 8, writeBuf);
	AddReadOutBuffer(4 * 8);
	DisableSPIChip();

	SetAnswerFast();
	SendBytesToDevice();
	GetDataFromDevice(4, res);

	return  ((unsigned int)res[0]) |
		(((unsigned int)res[1]) << 8) |
		(((unsigned int)res[2]) << 16) |
		(((unsigned int)res[3]) << 24);
}
void XSPIReadBlock_(unsigned char reg, unsigned char* buf, int num_words) {
	// For 512-byte block: reg = 0x20, bytes = 512

	unsigned char writeBuf[2] = { (reg << 2) | 1, 0xFF };

	ClearOutputBuffer();
	
	// Batch 128 reads of the register (for 512 bytes)
	for (int i = 0; i < num_words; ++i) {
		EnableSPIChip();
		AddWriteOutBuffer(sizeof(writeBuf) * 8, writeBuf);
		AddReadOutBuffer(4 * 8); // Queue a read of 4 bytes
		DisableSPIChip();
	}

	SetAnswerFast();
	// Send the entire batch over USB
	SendBytesToDevice();

	// Get all 512 bytes back at once
	GetDataFromDevice(num_words * 4, buf);
}

unsigned char XSPIReadBYTE_(unsigned char reg)
{
	unsigned char res;
	unsigned char writeBuf[2] = { (reg << 2) | 1, 0xFF };	
	
	ClearOutputBuffer();
		
	EnableSPIChip();
	AddWriteOutBuffer( sizeof(writeBuf)*8, writeBuf );
	AddReadOutBuffer( 1*8 );	
	DisableSPIChip();

	SetAnswerFast();
	SendBytesToDevice();
	GetDataFromDevice( 1, &res );
	return res;
}

void XSPIWrite_(unsigned char reg, unsigned char Data[] )
{
	unsigned char writeBuf[5] = { (reg << 2) | 2, 0,0,0,0 };	
	memcpy( &writeBuf[1], Data, 4 );
	
	EnableSPIChip();
	AddWriteOutBuffer( sizeof(writeBuf)*8, writeBuf );
	DisableSPIChip();

}


void XSPIWrite_sync(unsigned char reg, unsigned char Data[] )
{
	unsigned char writeBuf[5] = { (reg << 2) | 2, 0,0,0,0 };	
	memcpy( &writeBuf[1], Data, 4 );

	ClearOutputBuffer();
	
	EnableSPIChip();
	AddWriteOutBuffer( sizeof(writeBuf)*8, writeBuf );
	DisableSPIChip();

	SendBytesToDevice();
}

void XSPIWriteWORD_(unsigned char reg, unsigned int Data)
{	
	unsigned char writeBuf[5] = { (reg << 2) | 2, 0,0,0,0 };	
	memcpy( &writeBuf[1], &Data, 4 );

	ClearOutputBuffer();
	
	EnableSPIChip();
	AddWriteOutBuffer( sizeof(writeBuf)*8, writeBuf );
	DisableSPIChip();

	SendBytesToDevice();
}

void XSPIQueueBytes_(unsigned char reg, unsigned int Data) {
	unsigned char writeBuf[5] = { (reg << 2) | 2, 0,0,0,0 };
	memcpy(&writeBuf[1], &Data, 4);
	EnableSPIChip();
	AddWriteOutBuffer(sizeof(writeBuf) * 8, writeBuf);
	DisableSPIChip();

}

void XSPIWriteBlock_(unsigned char reg, unsigned char* data) {
	// Now send the data block (512 bytes)
	ClearOutputBuffer();
	for (int i = 0; i < 0x200; i += 4) {
		EnableSPIChip();
	
		AddWriteOutBuffer(0x20, &data[i]);  // Send 4 bytes at a time, 32 bits
		DisableSPIChip();
	}
	SetAnswerFast();
	SendBytesToDevice();

}

void XSPIWrite0_(unsigned char reg)
{
	unsigned char writeBuf[5] = { (reg << 2) | 2, 0,0,0,0 };	

	EnableSPIChip();
	AddWriteOutBuffer( sizeof(writeBuf)*8, writeBuf );	
	DisableSPIChip();

}

void XSPIWriteBYTE_(unsigned char reg, unsigned char d)
{
	unsigned char writeBuf[5] = { (reg << 2) | 2, d,0,0,0 };	

	EnableSPIChip();
	AddWriteOutBuffer( sizeof(writeBuf)*8, writeBuf );	
	DisableSPIChip();

}