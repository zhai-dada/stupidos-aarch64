#ifndef __FW_CFG_H__
#define __FW_CFG_H__

#include "asm/types.h"

#define FW_CFG_BASE         0x09020000
#define FW_CFG_DATA8        (FW_CFG_BASE + 0x00)
#define FW_CFG_DATA16       (FW_CFG_BASE + 0x00)
#define FW_CFG_DATA32       (FW_CFG_BASE + 0x00)
#define FW_CFG_DATA64       (FW_CFG_BASE + 0x00)
#define FW_CFG_SELECTOR     (FW_CFG_BASE + 0x08)
#define FW_CFG_DMA_ADDR     (FW_CFG_BASE + 0x10)
#define FW_CFG_DMA_ADDR_HI  (FW_CFG_BASE + 0x10)
#define FW_CFG_DMA_ADDR_LO  (FW_CFG_BASE + 0x14)

#define FW_CFG_DMA_CTL_READ     0x02
#define FW_CFG_DMA_CTL_SELECT   0x08
#define FW_CFG_DMA_CTL_WRITE    0x10
#define FW_CFG_DMA_CTL_ERROR    0x01

// fw_cfg selectors
#define FW_CFG_SIGNATURE    0x0000
#define FW_CFG_ID           0x0001
#define FW_CFG_FILE_DIR     0x0019

/**
    struct FWCfgFiles
    {

        uint32_t count;
        struct FWCfgFile f[];   
    };

    struct FWCfgFile
    {       
        uint32_t size;          
        uint16_t select;     
        uint16_t reserved;
        char name[56];         
    };
 */

// File directory entry
typedef struct
{
    uint32_t size;
    uint16_t select;
    uint16_t reserved;
    char name[56];
}__attribute__((packed)) fw_cfg_file_t;

typedef struct
{
    uint32_t control;
    uint32_t len;
    uint64_t addr;
}__attribute__((packed)) fw_cfg_dma_t;
               
void fw_cfg_dma_transfer(uint32_t control, uint32_t len, uint64_t addr);

uint16_t to_be16(uint16_t v);
uint32_t to_be32(uint32_t v);
uint64_t to_be64(uint64_t v);

#endif
