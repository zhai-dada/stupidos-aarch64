#ifndef __BARRIER_H__
#define __BARRIER_H__

#define isb()		asm volatile("isb"          : : : "memory")
#define dmb(opt)	asm volatile("dmb " #opt    : : : "memory")
#define dsb(opt)	asm volatile("dsb " #opt    : : : "memory")

#endif
