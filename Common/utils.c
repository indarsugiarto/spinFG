#include "utils.h"

inline void reportErr(sdp_msg_t *err_msg, uchar errCode)
{
	err_msg->srce_port = errCode;
	spin1_send_sdp_msg(err_msg, SDP_TIMEOUT);
}

uchar countAvailProc()
{
	uchar n, cntr, i;
	//TODO: modify this by reading r4 of the system controller -> not necessary!!!
	n = sv->num_cpus - 2;
	cntr = 0;

	//TODO: read, just like ps, to see how many workers have been up (with app-id = 17)
	// next, get the active workers that's already running app-ID WORKER_ID
	for(i=0; i<n; i++) {
		if(sv->vcpu_base[i+2].app_id == WORKER_ID && sv->vcpu_base[i+2].cpu_state == CPU_STATE_RUN)
			cntr++; //sv->vcpu_base[i+2] because 2 cores have been used for monitor cpu and master
	}
	return cntr;
}

/* SYNOPSIS
 * 		getAssignment() computes the assignment, given the index and the cardinal
 * 		Rumus aslinya: assignment[i] = (index/stride[i]) mod cardinal[i] 
 * 		A is a vector containing the resulting assignment, given the index "idx" on
 * 		the cardinality vector.
 * 		Check: sizeof(A) == sizeof(card) 
 **********************************************************************************/
void getAssignment(uint *A, uint idx, uint *card, uchar nVar)
{
    uchar i;
	uint *stride = (uint *)sark_alloc(nVar, sizeof(uint));
	stride[0] = 1;

    /* First, build the stride vector */
    for(i=1; i<nVar; i++)
        stride[i] = card[i-1] * stride[i-1];

    for(i=0; i<nVar; i++)
        A[i] = idx/stride[i] % card[i];
    sark_free(stride);
}


/* SYNOPSIS
 *		getJPDidx() computes the index of a jpd vector, given it's assignment
 *		with cardinal is 1xn vector and assigment is also 1xn vector
 * 		note: the left most variable is the most repetitive one.
 *		A=assignment */
/* Note: if strideIncluded (==1) is true, then vect is the stride, otherwise it is the card 
**************************************************************************************/
uint getJPDidx(uint *vect, uint *A, uchar nVar, uchar strideIncluded)
{
    uchar i;
    uint Sum_over_i = 0;
    if(strideIncluded == 0)	{	// vect contains a cardinality vector
		uint *stride = (uint *)sark_alloc(nVar, sizeof(uint));
		stride[0] = 1;

        /* First, build the stride vector */
        for(i=1; i<nVar; i++) {
            stride[i] = vect[i-1] * stride[i-1];
            Sum_over_i += A[i]*stride[i];
        }
        Sum_over_i+=A[0]*stride[0];
	    sark_free(stride);
    }
    else {
		for(i=0; i<nVar; i++) 
			Sum_over_i += A[i]*vect[i];
	}
    return Sum_over_i;
}

