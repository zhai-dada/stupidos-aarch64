#ifndef __TIMER_H__
#define __TIMER_H__

extern volatile uint64_t jiffies;

void timer_init(void);
void timer_init_secondary(void);

#endif
