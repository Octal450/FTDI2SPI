#include <cstdint>
#include <stdbool.h>
#pragma once

bool emmc_read_block(uint32_t block, unsigned char* buffer);
bool emmc_write_block(uint32_t block, unsigned char* buffer);
int xbox_emmc_write_block(int lba, uint8_t* buf);
bool emmc_init();
int xbox_emmc_read_block(int lba, uint8_t* buf);
int xbox_emmc_read_ext_csd(uint8_t* ext_csd);