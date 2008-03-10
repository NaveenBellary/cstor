#ifndef _SPL_TIME_H
#define _SPL_TIME_H

/*
 * Structure returned by gettimeofday(2) system call,
 * and used in other calls.
 */

#ifdef  __cplusplus
extern "C" {
#endif

#include <linux/module.h>
#include <linux/time.h>
#include <sys/types.h>

extern unsigned long long monotonic_clock(void);
extern void __gethrestime(timestruc_t *);

#define gethrestime(ts)			__gethrestime(ts)

#define TIME32_MAX			INT32_MAX
#define TIME32_MIN			INT32_MIN

#define SEC				1
#define MILLISEC			1000
#define MICROSEC			1000000
#define NANOSEC				1000000000

#define hz					\
({						\
        BUG_ON(HZ < 100 || HZ > MICROSEC);	\
        HZ;					\
})

static __inline__ time_t
gethrestime_sec(void)
{
        timestruc_t now;

        __gethrestime(&now);
        return now.tv_sec;
}

static __inline__ hrtime_t
gethrtime(void) {
        /* BUG_ON(cur_timer == timer_none); */

        /* Solaris expects a long long here but monotonic_clock() returns an
         * unsigned long long.  Note that monotonic_clock() returns the number
	 * of nanoseconds passed since kernel initialization.  Even for a signed
         * long long this will not "go negative" for ~292 years.
         */
        return monotonic_clock();
}


#ifdef  __cplusplus
}
#endif

#endif  /* _SPL_TIME_H */
