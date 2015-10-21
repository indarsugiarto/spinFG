/**** IONode.c
*
* SUMMARY
*  for coding/encoding pop-codes
*
* AUTHOR
*  Indar Sugiarto - indar.sugiarto@manchester.ac.uk
*
* DETAILS
*  Created on       : 3 Sept 2015
*  Version          : $Revision: 2 $
*  Last modified by : $Author: indi $
*
* COPYRIGHT
*  Copyright (c) The University of Manchester, 2011. All rights reserved.
*  SpiNNaker Project
*  Advanced Processor Technologies Group
*  School of Computer Science
*
* TODO:
* 1. SEND and RECEIVE PARAMS 
*******/

// SpiNNaker API
#include <sark.h>
#include "../Common/spinFG.h"
#include "../Common/spinErrDef.h"
#include "../Common/utils.h"

#define DEF_WORKER_KEY	255
#define DEF_MASTER_KEY	1

#define VARS_BUF_TAG    1
#define POP_CODES_TAG   2

/* SpiNNaker-architecture related */
sdp_msg_t my_msg;
sdp_msg_t err_msg;

uchar availCore;                // number of available cores for computation

/* Factor graph related */
uint *msgBuf;					// will be stored in SYSRAM?
uint nStates;
uint nVars;
io_t *vfMap;					// variable and factor map in sysram
uint vfMapCntr;
uint *popCodes;                 // buffer of all population codes in SDRAM

/************** routing stuffs *************/
/* generic key format: [arg2, arg1, cmd, dest] */
uint masterKey;
uint workerKey;				// this is the routing pattern to the available working cores

/* Forward declaration */

/* Implementation */



/* SYNOPSIS
 *		Basically, we need only two keys:
 *		xxxxxx01 -> will be sent to core-1
 *		xxxxxxff -> will be sent to all workers
 *		So, the masking is 0x000000FF
 * */
int initRoutingTable()
{
	return 1; // just for now, modify later

	uint e = rtr_alloc(2);
	masterKey = 1;
	workerKey = 255;
	if(e==0) rt_error(RTE_ABORT);
	rtr_mc_set(  e, masterKey, 255, (1 << 7));	// 1000 0000
	rtr_mc_set(e+1, workerKey, 255, 0xFFFF00);	// 1111 1111 1111 1111 0000 0000
    return 0;
}

/* FNodeMaster will only receive MC packet without payload from workers
 * */
void receiveMC(uint key, uint payload)
{

}

// free allocated memory
void freeHeap()
{
    sark_free(vfMap);
    sark_free(popCodes);
    vfMapCntr = 0;
}

/* Format variables ID via SDP as data payload:
 * Note: varID is ushort. We expect the host to send the varID along with its factor's chip as a pain in a uint chunk.
 * The number of varID-factorChip is determined by nV, which is contained in msg->arg3 of the sdp packet.
 */
void setupParam(ushort seq, uint nV, uint *param)
{
    uint i, j, szBufs;
	// if seq is 0, then initialize memory
	if(seq==0) {
        if(vfMapCntr) freeHeap();
        szBufs = nVars * sizeof(io_t);
        vfMap = sark_xalloc(sv->sysram_heap, szBufs, VARS_BUF_TAG, ALLOC_LOCK);
        szBufs = nVars * nStates * sizeof(uint);
        popCodes = sark_xalloc(sv->sdram_heap, szBufs, POP_CODES_TAG, ALLOC_LOCK);
        for(i=0; i<nVars; i++) {
            vfMap[i].sdramLoc = popCodes + i*nStates;
        }
    }

    // then extract the payload
    ushort uint2ushort[2];
    for(i=0; i<nV; i++) {             // for all variables in the payload,
        sark_mem_cpy((void *)uint2ushort, (void *)data[i], sizeof(uint));
        vfMap[vfMapCntr].varID = uint2ushort[0];
        vfMap[vfMapCntr].destChipX = (uchar)(uint2ushort[1] >> 8);
        vfMap[vfMapCntr].destChipY = (uchar)(uint2ushort[1] & 0xFF);
        vfMapCntr++;
    }

    bRunning = 1;
    io_printf(IO_STD, "Parameters are set!\n"); sark_delay_us(DEF_SPIN_DELAY);
}


/* receiveSDP() will process SDP sent by host to core-1 on port-1
 * port-0 is prohibited because it is dedicated for debugging purpose
 *
 * TODO: port argument
 */
void receiveSDP(uint mailbox, uint port)
{
	sdp_msg_t *msg = (sdp_msg_t *) mailbox;
	uint *data = (uint *) msg->data;

	//io_printf(IO_STD, "I receive an SDP with cmd = %x\n", msg->cmd_rc);
	switch (msg->cmd_rc) {
	case HOST_SAY_HELLO:
        io_printf(IO_STD, "Hello host...\n"); spin1_delay_us(DEF_SPIN_DELAY);
		break;
    case HOST_SEND_NODE_IO_PARAM:									// set variables ID
        nVars = msg->arg1;
		nStates = msg->arg2;
        setupParam(msg->seq, msg->arg3, data);      // msg->arg3 contains the number of variables carried in the payload
		break;

	case HOST_REQUEST_POPCODE:										//
		sendPopCodeToHost();
		break;

	case HOST_SIGNALS_STOP:
        if(vfMapCntr) freeHeap();
        spin1_exit(0);
		break;

	default:
	  // unexpected packet!
		io_printf (IO_STD, "NodeIO receives unknown SDP!\n");
		spin1_delay_us(DEF_SPIN_DELAY);
		break;
	  }
	  spin1_msg_free (msg);
}

uint initMe()
{
	uint success = 1;			// in c, true is any value except 0; false is zero!!!
    vfMapCntr = 0;

    if(leadAp) {
		uint myAppID, myCoreID, myChipID;

		// check if this is in chip <0,0> and the app-id is IO_MASTER_ID
		myAppID = sark_app_id();
		myChipID = sark_chip_id();
		myCoreID = sark_core_id();

		if(myChipID !=0 && myAppID != IO_NODE_ID) {
			io_printf(IO_STD, "Please put me on chip<0,0> and assign me app-ID %d. Thank you!\n", IO_NODE_ID);
			return 0;
		}

		if (!initRoutingTable()) {
			io_printf(IO_STD, "Fail to init router!\n");
			spin1_delay_us(DEF_SPIN_DELAY);
			return 0;
		}

		// about memory allocation
		dmaData.allocatedSDRAM = 0;				//memory is not yet allocated

		// TODO: check if heap is already allocated in memory
		memIsInitialized = 0;

		availCore = countAvailProc();
		if(availCore==0) {
			io_printf(IO_STD, "No worker is available.\n");
			spin1_delay_us(DEF_SPIN_DELAY);
		}
		else {
			io_printf(IO_STD, "Found %d workers ready to work!\n", availCore);
			spin1_delay_us(DEF_SPIN_DELAY);
		}

		// initialize SDP for normal operation
		my_msg.tag = DEF_IPTAG;					// IPTag 1
		my_msg.dest_port = PORT_ETH;			// Ethernet
		my_msg.dest_addr = sv->eth_addr;		// Nearest Ethernet chip
		my_msg.flags = 0x07;
		my_msg.srce_port = MASTER_CORE;			// master will ALWAYS in core 17
		my_msg.srce_addr = sv->p2p_addr;

		// initialize SDP for error reporting
		err_msg.tag = DEF_ERR_TAG;
		err_msg.dest_port = PORT_ETH;
		err_msg.dest_addr = sv->eth_addr;
		err_msg.flags = 0x07;
		//err_msg.srce_port = MASTER_CORE;		// we will use srce_port as errCode
		err_msg.srce_addr = sv->p2p_addr;
		err_msg.length = sizeof(sdp_hdr_t);
		reportErr(&err_msg, 0);							// test the "errTub.py" error capture

	}

	io_printf(IO_STD, "FNodeIO built-%d.%d.%d.%d has passed its initialization!\n", VERSION, REVISION, PTAG, PCNTR);
	return success;
}

void c_main()
{
	if(initMe()) {
		//spin1_callback_on (MC_PACKET_RECEIVED, receiveMC, 0);
		spin1_callback_on (MCPL_PACKET_RECEIVED, receiveMCPL, 0);
		spin1_callback_on (SDP_PACKET_RX, receiveSDP, 2);
		spin1_callback_on (DMA_TRANSFER_DONE, checkMemCopy, 1);
	    spin1_start(SYNC_NOWAIT);
	}
}

/* NOTE: in c, true is any value except 0; false is zero!!!
 *
 * */
