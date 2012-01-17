#ifndef VCOS_STDBOOL_H
#define VCOS_STDBOOL_H

#ifndef __cplusplus

#if defined(__STDC__) && (__STDC_VERSION__ >= 199901L)
#include <stdbool.h>
#else
typedef enum {
   false,
   true
} bool;
#endif

#endif /* __cplusplus */

#endif
