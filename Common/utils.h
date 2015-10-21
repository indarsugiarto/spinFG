#ifndef _UTILS_H_
#define _UTILS_H_

#include <spin1_api.h>
#include "spinFG.h"

void reportErr(sdp_msg_t *err_msg, uchar errCode);
uchar countAvailProc();
void getAssignment(uint *A, uint idx, uint *card, uchar nVar);
uint getJPDidx(uint *vect, uint *A, uchar nVar, uchar strideIncluded);

#endif
