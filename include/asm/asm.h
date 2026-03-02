#ifndef __ASM_H__
#define __ASM_H__

#define FUNC_GLOBAL(_name)      \
	.align  2;					\
	.global _name;				\
	.type   _name, %function;	\
    _name:

#define FUNC_END(_name)         \
    .size _name, .- _name;

#define FUNC_STATIC(_name)      \
	.align  2;					\
	.type   _name, %function;	\
    _name:

#define EXPORT(symbol)          \
	.globl  symbol;             \
	symbol:                     \

#endif
