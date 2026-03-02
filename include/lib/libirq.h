#ifndef __LIBIRQ_H__
#define __LIBIRQ_H__

#define DAIF_F  (1UL << 6)
#define DAIF_I  (1UL << 7)
#define DAIF_A  (1UL << 8)
#define DAIF_D  (1UL << 9)

void enable_irq(void);
void disable_irq(void);

#endif
