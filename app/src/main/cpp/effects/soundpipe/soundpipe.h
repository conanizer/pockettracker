// Minimal Soundpipe stub — only the types and declarations needed for pareq.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef float SPFLOAT;
#define SP_OK      0
#define SP_NOT_OK -1
typedef struct sp_data {
    float sr;
} sp_data;

#ifdef __cplusplus
}
#endif

#include "pareq.h"
