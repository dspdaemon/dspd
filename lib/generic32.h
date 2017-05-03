#ifndef _DSPD_GENERIC32_H_
#define _DSPD_GENERIC32_H_
#define dspd_nop() AO_nop_full()
#define dspd_ts_read(__addr) (*__addr)

#endif
