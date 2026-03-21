#include "debug.h"
#include "pt_regs.h"

void show_ptregs(pt_regs_t* regs)
{
    printk("[x0\t]: %lx\n", regs->s_reg[0]);
    printk("[x1\t]: %lx\n", regs->s_reg[1]);
    printk("[x2\t]: %lx\n", regs->s_reg[2]);
    printk("[x3\t]: %lx\n", regs->s_reg[3]);
    printk("[x4\t]: %lx\n", regs->s_reg[4]);
    printk("[x5\t]: %lx\n", regs->s_reg[5]);
    printk("[x6\t]: %lx\n", regs->s_reg[6]);
    printk("[x7\t]: %lx\n", regs->s_reg[7]);
    printk("[x8\t]: %lx\n", regs->s_reg[8]);
    printk("[x9\t]: %lx\n", regs->s_reg[9]);
    printk("[x10\t]: %lx\n", regs->s_reg[10]);
    printk("[x11\t]: %lx\n", regs->s_reg[11]);
    printk("[x12\t]: %lx\n", regs->s_reg[12]);
    printk("[x13\t]: %lx\n", regs->s_reg[13]);
    printk("[x14\t]: %lx\n", regs->s_reg[14]);
    printk("[x15\t]: %lx\n", regs->s_reg[15]);
    printk("[x16\t]: %lx\n", regs->s_reg[16]);
    printk("[x17\t]: %lx\n", regs->s_reg[17]);
    printk("[x18\t]: %lx\n", regs->s_reg[18]);
    printk("[x19\t]: %lx\n", regs->s_reg[19]);
    printk("[x20\t]: %lx\n", regs->s_reg[20]);
    printk("[x21\t]: %lx\n", regs->s_reg[21]);
    printk("[x22\t]: %lx\n", regs->s_reg[22]);
    printk("[x23\t]: %lx\n", regs->s_reg[23]);
    printk("[x24\t]: %lx\n", regs->s_reg[24]);
    printk("[x25\t]: %lx\n", regs->s_reg[25]);
    printk("[x26\t]: %lx\n", regs->s_reg[26]);
    printk("[x27\t]: %lx\n", regs->s_reg[27]);
    printk("[x28\t]: %lx\n", regs->s_reg[28]);
    printk("[fp\t]: %lx\n", regs->s_fp);
    printk("[lr\t]: %lx\n", regs->s_lr);
    printk("[sp\t]: %lx\n", regs->s_sp);
    printk("[pc\t]: %lx\n", regs->s_pc);
    printk("[pst\t]: %lx\n", regs->s_pstate);
    return;
}
