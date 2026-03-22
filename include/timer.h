#ifndef __TIMER_H__
#define __TIMER_H__

#define STUPIDOS_TIMER_HZ 100U

extern volatile uint64_t jiffies;

void timer_init(void);
void timer_init_secondary(void);

#endif
