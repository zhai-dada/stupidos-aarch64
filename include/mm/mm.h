#ifndef __MM_H__
#define __MM_H__

/* CONFIG_ARM64_VA_BITS = 48*/
#define CONFIG_ARM64_VA_BITS    48
#define VA_BITS	                (CONFIG_ARM64_VA_BITS)

#define PAGE_SHIFT	 		    12
#define TABLE_SHIFT 			9

#define PAGE_SIZE   			(1 << PAGE_SHIFT)	
#define PAGE_MASK               (~(PAGE_SIZE - 1))

#define TOTAL_MEMORY            (512 * 0x100000) // 512mb

/*
 * Memory types available.
 */
#define MT_DEVICE_nGnRnE_IDX	    0
#define MT_DEVICE_nGnRE_IDX		    1
#define MT_DEVICE_GRE_IDX		    2
#define MT_NORMAL_NC_IDX		    3
#define MT_NORMAL_IDX		        4
#define MT_NORMAL_WT_IDX		    5

#define MAIR(attr, mt)	        ((attr) << ((mt) * 8))

/* to align the pointer to the (next) page boundary */
#define PAGE_ALIGN(addr)        (((addr) + PAGE_SIZE - 1) & PAGE_MASK)
#define PAGE_ALIGN_UP(addr)     PAGE_ALIGN(addr)
#define PAGE_ALIGN_DOWN(addr)   (addr & PAGE_MASK)

#endif
