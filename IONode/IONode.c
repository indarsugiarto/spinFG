/**** IONode.c
*
* SUMMARY
*  for coding/encoding pop-codes
*  The resulting IONode.aplx will run on all cores in chip<0,0>
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

#define KEY_MASK		255

#define VARS_BUF_TAG    1
#define POP_CODES_TAG   2
#define BIN_WIDTH_TAG	3
#define MID_VALS_TAG	4

/* SpiNNaker-architecture related */
uint myCoreID, myChipID;
sdp_msg_t popc_msg;
sdp_msg_t err_msg;

uchar availCore = 0;                // number of available cores for computation
volatile uchar IamBusy = 0;

/* Factor graph related */
uint *msgBuf;					// will be stored in SYSRAM?
uint nStates;
uint nVars;
io_t *vfMap;					// variable and factor map in sysram
uint vfMapCntr;
uint *popCodes;                 // buffer of all population codes in SDRAM
uint *binWidth;					// buffer of all binWidth parameters in SDRAM
uint *midVals;					// buffer of all midVals parameters in SDRAM


/* Forward declaration */
void circulateVal(uint varID, uint val);
void decodeVal(uint varID);
void averagingVal(uint varID);
void receiveSDP(uint mailbox, uint port);

/* Implementation */



/* SYNOPSIS
 *		Basically, we need only these keys for circular pool:
 *		xxxxxynn -> will be sent to core-n for y-operation:
 *		y = 0 -> decoding (creating pop codes), payload is the varID
 *		y = 1 -> encoding (computing expecation), payload is the varID
 *		So, the masking is 0x000000FF
 *		The circular pool will be:
 *		1 -> 2 -> 3 -> ... -> 1
 *		Decoding flow:  core-1 will forward to core-2. if core-2 is busy, core-2
 *						will forward to core-3 etc. until the last core, and if it is
 *						also busy, it will go back again to core-1. If this happens,
 *						core-1 will stop responding to any SDP and do the chores!
 * */
int initRoutingTable()
{
	uint i, e = rtr_alloc(2*availCore);
	if(e==0) rt_error(RTE_ABORT);
	// first, for decoding purpose
	for(i=1; i<availCore; i++) {
		rtr_mc_set(e, i, KEY_MASK, (1 << (i+1+6)));
		e++;
	}
	rtr_mc_set(e, availCore, KEY_MASK, (1 << 7)); e++;
	// then, for the encoding purpose
	for(i=1; i<availCore; i++) {
		rtr_mc_set(e, i+256, KEY_MASK, (1 << (i+1+6)));
		e++;
	}
	rtr_mc_set(e, availCore+256, KEY_MASK, (1 << 7));
    return 0;
}

/* SYNOPSIS
 *		If core-1 receives a crisp value from host, it will forward to "circular-worker". But if none
 *		of the worker is available (all are busy), then core-1 will stop receiving SDP temporary and
 *		handle the decoding by itself.
 *		payload contains the varID
 * */
void receiveMCPL(uint key, uint payload)
{
	if(IamBusy) {
		uint newKey = (key>256) ? (myCoreID+256) : myCoreID;
		spin1_send_mc_packet(newKey, payload, 1);
	}
	else {
		IamBusy = 1;
		if(key>256)
			averagingVal(payload);
		else
			decodeVal(payload);
		IamBusy = 0;
	}
}

// free allocated memory
void freeHeap()
{
    sark_free(vfMap);
    sark_free(popCodes);
	sark_free(binWidth);
	sark_free(midVals);
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
		binWidth = sark_xalloc(sv->sdram_heap, szBufs, BIN_WIDTH_TAG, ALLOC_LOCK);
		midVals = sark_xalloc(sv->sdram_heap, szBufs, MID_VALS_TAG, ALLOC_LOCK);
        for(i=0; i<nVars; i++) {
            vfMap[i].sdramLoc = popCodes + i*nStates;
			vfMap[i].bwLoc = binWidth + i*nStates;
			vfMap[i].mvLoc = midVals + i*nStates;
        }
    }

    // then extract the payload
    ushort uint2ushort[2];
    for(i=0; i<nV; i++) {             // for all variables in the payload,
		sark_mem_cpy((void *)uint2ushort, (void *)param[i], sizeof(uint));
        vfMap[vfMapCntr].varID = uint2ushort[0];
        vfMap[vfMapCntr].destChipX = (uchar)(uint2ushort[1] >> 8);
        vfMap[vfMapCntr].destChipY = (uchar)(uint2ushort[1] & 0xFF);
        vfMapCntr++;
    }

    io_printf(IO_STD, "Parameters are set!\n"); sark_delay_us(DEF_SPIN_DELAY);
}

// sendPopCodes might be called from the worker!
void sendPopCodes(ushort varID, uchar destX, uchar destY)
{
	uint i;
	// initialize SDP for normal operation
	if(destX + destY == 0) {					// send to host
		popc_msg.tag = DEF_IPTAG;				// IPTag 1
		popc_msg.dest_port = PORT_ETH;			// Ethernet
		popc_msg.dest_addr = sv->eth_addr;		// Nearest Ethernet chip
		popc_msg.flags = 0x07;
		popc_msg.srce_port = MASTER_CORE;		// master will ALWAYS in core 1
		popc_msg.srce_addr = sv->p2p_addr;
	}
	else {										// send to core-1 and port-1 to a destination core
		popc_msg.tag = 0;						// should be 0 for internal SpiNNaker machine
		popc_msg.dest_port = (SDP_PORT_SPIN << 5) | MASTER_CORE;
		popc_msg.dest_addr = ((ushort)destX << 8) | (ushort)destY;
		popc_msg.flags = 0x07;
		popc_msg.srce_port = (SDP_PORT_SPIN << 5) | (uchar)myCoreID;			// master will ALWAYS in core 1
		popc_msg.srce_addr = (ushort)myChipID;
	}

	// first determine how many sdp packets will be generated
	uint nChunks = nStates / 64;				// one SDP contains max 64 items
	uint nRest = nStates % 64;
	if(nRest != 0) nChunks++;

	// then copy the corresponding pop-code from SDRAM
	// we don't use dma since the pop-code is small
	uint *sdramBuf;
	uint len;
	for(i=0; i<vfMapCntr; i++) {
		if(varID==vfMap[i].varID) {
			sdramBuf = vfMap[i].sdramLoc;
			break;
		}
	}
	for(i=0; i<nChunks; i++) {
		if(i==nChunks-1 && nRest!=0) len=nRest; else len=64;
		spin1_memcpy(popc_msg.data, sdramBuf + i*len, len*sizeof(uint));
		spin1_send_sdp_msg(&popc_msg, SDP_TIMEOUT);
	}
}

void circulateVal(uint varID, uint val)
{
	uint i;
	// first, store the val_in in the vfMap, assuming varID exist in the vfMap
	for(i=0; i<vfMapCntr; i++) {
		if(vfMap[i].varID == (ushort)varID) {
			vfMap[i].val_in = val;
			break;
		}
	}
	// then send notification to the neighbor worker
	spin1_send_mc_packet(myCoreID, varID, 1);
}

/* decodeVal() computes the population code from a crisp value
*/
void decodeVal(uint varID)
{
	// disable sdp communication
	spin1_callback_off(SDP_PACKET_RX);

	// TODO: then do the computation

	// finally, enable sdp communication again
	spin1_callback_on(SDP_PACKET_RX, receiveSDP, 0);
}

/* averagingVal() computes the crisp value from a population code
*/
void averagingVal(uint varID)
{

}

/* getBinWidthParam() and getMidValsParam() will use the following format:
 * seq : the sequence number of data stream from the host
 * arg1: total number of seq
 * arg2: number of items in the payload
 * arg3: varID
*/
void getBinWidthParam(sdp_msg_t *msg)
{
	uint i, len, offset;
	uint *ptr;
	for(i=0; i<vfMapCntr; i++){
		if(vfMap[i].varID==(ushort)msg->arg3) {
			ptr = vfMap[i].bwLoc;
			break;
		}
	}
	if(msg->arg1 > 1) offset = 64; else offset = 0;
	if(msg->seq < (ushort)msg->arg1) len = 64; else len = msg->arg2;
	spin1_memcpy(ptr + (msg->seq-1)*offset, msg->data, len*sizeof(uint));
}

void getMidValsParam(sdp_msg_t *msg)
{
	uint i, len, offset;
	uint *ptr;
	for(i=0; i<vfMapCntr; i++){
		if(vfMap[i].varID==(ushort)msg->arg3) {
			ptr = vfMap[i].mvLoc;
			break;
		}
	}
	if(msg->arg1 > 1) offset = 64; else offset = 0;
	if(msg->seq < (ushort)msg->arg1) len = 64; else len = msg->arg2;
	spin1_memcpy(ptr + (msg->seq-1)*offset, msg->data, len*sizeof(uint));
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

	case HOST_SEND_NODE_IO_PARAM:					// set variables ID
        nVars = msg->arg1;
		nStates = msg->arg2;
        setupParam(msg->seq, msg->arg3, data);      // msg->arg3 contains the number of variables carried in the payload
		break;

	case HOST_SEND_VAR_TO_DECODE:
		circulateVal(msg->arg1, msg->arg2);

	case HOST_REQUEST_POPCODE:
		sendPopCodes((ushort)msg->arg1,0,0);
		break;

	case HOST_SEND_BIN_WIDTH_PARAM:
		getBinWidthParam(msg);

	case HOST_SEND_MID_VALS_PARAM:
		getMidValsParam(msg);

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
		uint myAppID;

		// check if this is in chip <0,0> and the app-id is IO_MASTER_ID
		myAppID = sark_app_id();
		myChipID = sark_chip_id();
		myCoreID = sark_core_id();

		if(myChipID !=0 && myAppID != IO_NODE_ID) {
			io_printf(IO_STD, "Please put me on chip<0,0> and assign me app-ID %d. Thank you!\n", IO_NODE_ID);
			return 0;
		}

		availCore = countAvailProc(IO_NODE_ID);
		availCore++;	// count myself as a working core as well!!!
		if(availCore==0) {
			io_printf(IO_STD, "No worker is available.\n");
			spin1_delay_us(DEF_SPIN_DELAY);
		}
		else {
			io_printf(IO_STD, "Found %d workers ready to work!\n", availCore);
			spin1_delay_us(DEF_SPIN_DELAY);
		}

		if (!initRoutingTable()) {
			io_printf(IO_STD, "Fail to init router!\n");
			spin1_delay_us(DEF_SPIN_DELAY);
			return 0;
		}

		// initialize SDP for normal operation
		popc_msg.tag = DEF_IPTAG;					// IPTag 1
		popc_msg.dest_port = PORT_ETH;			// Ethernet
		popc_msg.dest_addr = sv->eth_addr;		// Nearest Ethernet chip
		popc_msg.flags = 0x07;
		popc_msg.srce_port = MASTER_CORE;			// master will ALWAYS in core 17
		popc_msg.srce_addr = sv->p2p_addr;

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
		spin1_callback_on (MCPL_PACKET_RECEIVED, receiveMCPL, 1);
		spin1_callback_on (SDP_PACKET_RX, receiveSDP, 0);
	    spin1_start(SYNC_NOWAIT);
	}
}

/* NOTE: in c, true is any value except 0; false is zero!!!
 *
 * */
