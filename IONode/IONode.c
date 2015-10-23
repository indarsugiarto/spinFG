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
* 1. SEND and RECEIVE PARAMS. All workers should know, where the buffer (popCodes, binWidth, midVals) are located!
*******/

// SpiNNaker API
#include <sark.h>
#include "../Common/spinFG.h"
#include "../Common/spinErrDef.h"
#include "../Common/utils.h"
#include <stdfix.h>

/* for using floating point */
#define REAL			accum
#define REAL_CONST( x )	x##k

/* generic for debugging purpose */
#define DEBUG_VERBOSE
#define VERSION			1
#define REVISION		2
#define PTAG			1
#define PCNTR			8


#define VARS_BUF_TAG    1
#define POP_CODES_TAG   2
#define BIN_WIDTH_TAG	3
#define MID_VALS_TAG	4

// dma transfer tag
#define DMA_STORE_PC_TAG	1
#define DMA_FETCH_PC_TAG	2
#define DMA_FETCH_BW_TAG	4
#define DMA_FETCH_MV_TAG	6

/* SpiNNaker-architecture related */
uint myCoreID, myChipID;
sdp_msg_t popc_msg;
sdp_msg_t err_msg;

uchar availCore = 0;                // number of available cores for computation
volatile uchar IamBusy = 0;

/* Factor graph related */
uint *msgBuf;					// will be stored in SYSRAM?
io_t *vfMap;					// variable and factor map in sysram
uint vfMapCntr;
uint *popCodes;                 // buffer of all population codes in SDRAM
uint *binWidth;					// buffer of all binWidth parameters in SDRAM
uint *midVals;					// buffer of all midVals parameters in SDRAM
uint *pcDTCMbuf;
uint *bwDTCMbuf;
uint *mvDTCMbuf;
volatile uchar flDMAfinish;		// flag to indicate if dma transfer is complete


/* Forward declaration */
void circulateVal(sdp_msg_t *mst);
void decodeVal(ushort varID);
void averagingVal(ushort varID);
void receiveSDP(uint mailbox, uint port);
REAL gar(REAL border1, REAL border2, REAL value);
REAL errf(REAL x);

/* Implementation */



/* SYNOPSIS
 *		Basically, we need these keys for circular pool:
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
 *		We also need broadcast keys to distribute parameters:
 *      1 << 31 : vfMap address
 *		1 << 30 : vfMapCntr value
 *		1 << 29 : popCOdes address
 *		1 << 28 : binWidth address
 *		1 << 27 : midVals address
 * */
#define KEY_MASK_DEC		0x000000FF
#define KEY_MASK_ENC		0x0000FF00
#define KEY_MASK_PAR		0xFF000000
#define KEY_BCAST_RPT_STAT	(1 << 31)		// for debugging purpose, all cores are expected to report their status
#define KEY_VFMAP			(1 << 30)
#define KEY_VFMAP_CNT		(1 << 29)
#define KEY_POPC_ADDR		(1 << 28)
#define KEY_BINW_ADDR		(1 << 27)
#define KEY_MIDV_ADDR		(1 << 26)
#define KEY_VAL_RANGE		(1 << 25)
#define BCAST_ROUTE			(0xFFFF << 8)	// core-1 is excluded
int initRoutingTable()
{
	uint i, e = rtr_alloc(2*availCore);
	if(e==0) rt_error(RTE_ABORT);
	// first, for decoding purpose, key values: 1-17 (assuming availCore=17)
	for(i=1; i<availCore; i++) {
		rtr_mc_set(e, i, KEY_MASK_DEC, (1 << (i+1+6))); // route to the next core
		e++;
	}
	rtr_mc_set(e, availCore, KEY_MASK_DEC, (1 << 7)); e++; // special for the last core, route to core-1
	// then, for the encoding purpose (expectation computation), key values: 257-273 (assuming availCore=17)
	for(i=1; i<availCore; i++) {
		rtr_mc_set(e, i+256, KEY_MASK_ENC, (1 << (i+1+6))); // route to the next core
		e++;
	}
	rtr_mc_set(e, availCore+256, KEY_MASK_ENC, (1 << 7)); // special for the last core, route to core-1
	// finally, keys for broadcast from core-1 to core-2 until core-17
	e = rtr_alloc(7);
	rtr_mc_set(e, KEY_BCAST_RPT_STAT, KEY_MASK_PAR, BCAST_ROUTE); e++;
	rtr_mc_set(e, KEY_VFMAP, KEY_MASK_PAR, BCAST_ROUTE); e++;
	rtr_mc_set(e, KEY_VFMAP_CNT, KEY_MASK_PAR, BCAST_ROUTE); e++;
	rtr_mc_set(e, KEY_POPC_ADDR, KEY_MASK_PAR, BCAST_ROUTE); e++;
	rtr_mc_set(e, KEY_BINW_ADDR, KEY_MASK_PAR, BCAST_ROUTE); e++;
	rtr_mc_set(e, KEY_MIDV_ADDR, KEY_MASK_PAR, BCAST_ROUTE); e++;
	rtr_mc_set(e, KEY_VAL_RANGE, KEY_MASK_PAR, BCAST_ROUTE);
    return 0;
}


/* all cores except core-1 will receive MC packets for updating global knowledge on parameters */
void update_status(uint key, uint payload)
{
	switch(key) {
	case KEY_BCAST_RPT_STAT:
		if(vfMapCntr==0)
			io_printf(IO_BUF, "[Warning] No knowledge about global parameters!\n");
		io_printf(IO_BUF, "vfMap address   : %x\n", vfMap);
		io_printf(IO_BUF, "vfMapCntr       : %u\n", vfMapCntr);
		io_printf(IO_BUF, "popCodes buffer : %x\n", popCodes);
		io_printf(IO_BUF, "binWidth buffer : %x\n", binWidth);
		io_printf(IO_BUF, "midVals buffer  : %x\n", midVals);
		break;
	case KEY_VFMAP:
		vfMap = (io_t *)payload;
		break;
	case KEY_VFMAP_CNT:
		vfMapCntr = payload;
		break;
	case KEY_POPC_ADDR:
		popCodes = (uint *)payload;
		break;
	case KEY_BINW_ADDR:
		binWidth = (uint *)payload;
		break;
	case KEY_MIDV_ADDR:
		midVals = (uint *)payload;
		break;
	}
}

/* SYNOPSIS
 *		Three types of keys:
 *		- broadcast key: for update_status
 *		- decode key (low byte)
 *		- encode/averaging key (high byte)
 *		Working on decode or encode keys:
 *			If core-1 receives a crisp value from host, it will forward to "circular-worker".
 *			But if none of the worker is available (all are busy), then core-1 will stop
 *			receiving SDP temporary and	handle the decoding by itself.
 *			In this case, payload contains the varID
 * */
void receiveMCPL(uint key, uint payload)
{
	if(key>0xFFFF) {
		update_status(key, payload);
	}
	else {
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
}

// free allocated memory
void freeHeap()
{
    sark_free(vfMap);
    sark_free(popCodes);
	sark_free(binWidth);
	sark_free(midVals);
	sark_free(pcDTCMbuf);
	sark_free(bwDTCMbuf);
	sark_free(mvDTCMbuf);
    vfMapCntr = 0;
}

/* Use sdp to initialize the parameters as follows:
 * seq  : the current stream/chunk number sent by host,
 *		  seq=0 will trigger initialization, seg=0xFFFF indicates the last stream
 * arg1 : nVars and nStates
 * arg2 : popCodesType
 * arg3 : current number of variableIDs contained in the payload
 * Format variables ID in SDP payload: [ varID, [X-chip-addr, Y-chip-addr] ]
 *		Note: varID is ushort. We expect the host to send the varID along
 *		with its factor's chip as a a uint package.
 */
//void setupParam(ushort seq, uint nV, uint *param)
void setupParam(sdp_msg_t *msg)
{
	// total number of variables in factor graph (nVars) and nStates are contained in arg1
	ushort nVars = msg->arg1 >> 16;
	ushort nStates = msg->arg1 & 0xFFFF;
	// value range is in arg2 --> later on, it must be converted into accum
	uint valRange = msg->arg2;
	// popCodesType and current number of variableIDs in the payload (cnV) is in arg3
	ushort popCodesType = msg->arg2 >> 16;
	ushort cnV = msg->arg3 & 0xFFFF;
	uint i, j, szBufs;
	// if seq is 0, then initialize memory
	if(msg->seq==0) {
		if(vfMapCntr) freeHeap();
		vfMapCntr = 0;	// reset the total io_t data counter
		szBufs = nVars * sizeof(io_t);	// each variable will have an associated io_t data stored in sysram_heap
        vfMap = sark_xalloc(sv->sysram_heap, szBufs, VARS_BUF_TAG, ALLOC_LOCK);
        szBufs = nVars * nStates * sizeof(uint);
        popCodes = sark_xalloc(sv->sdram_heap, szBufs, POP_CODES_TAG, ALLOC_LOCK);
		binWidth = sark_xalloc(sv->sdram_heap, szBufs, BIN_WIDTH_TAG, ALLOC_LOCK);
		midVals = sark_xalloc(sv->sdram_heap, szBufs, MID_VALS_TAG, ALLOC_LOCK);
		// DTCM buffer for holding temporary data: pcDTCMbuf, bwDTCMbuf, mvDTCMbuf
		pcDTCMbuf = sark_alloc(nStates, sizeof(uint));
		bwDTCMbuf = sark_alloc(nStates, sizeof(uint));
		mvDTCMbuf = sark_alloc(nStates, sizeof(uint));
		// now, let's fill the pointer in each io_t data with the direct address in SDRAM buffers
        for(i=0; i<nVars; i++) {
            vfMap[i].sdramLoc = popCodes + i*nStates;
			vfMap[i].bwLoc = binWidth + i*nStates;
			vfMap[i].mvLoc = midVals + i*nStates;
			vfMap[i].nStates = nStates;
			vfMap[i].nVars = nVars;
			vfMap[i].valRange = valRange;
			vfMap[i].popCodesType = popCodesType;
			// the other io_t items of the current vfMap will be specified in the payload part of the sdp packet
        }
    }

	// then extract the payload to get the other io_t items
    ushort uint2ushort[2];
	uint *param = (uint *)msg->data;
	// for the specified number of variableIDs in payload, do...
	for(i=0; i<cnV; i++) {             // for all variables in the payload,
		sark_mem_cpy((void *)uint2ushort, (void *)param[i], sizeof(uint));
        vfMap[vfMapCntr].varID = uint2ushort[0];
		vfMap[vfMapCntr].destChipX = (uint2ushort[1] >> 8);		// X-chip-addr is in high byte
		vfMap[vfMapCntr].destChipY = (uint2ushort[1] & 0xFF);	// Y-chip-addr is in low byte
        vfMapCntr++;
    }
	// finally, broadcast params
	spin1_send_mc_packet(KEY_VFMAP, (uint)vfMap, 1);
	spin1_send_mc_packet(KEY_VFMAP_CNT, vfMapCntr, 1);
	spin1_send_mc_packet(KEY_POPC_ADDR, (uint)popCodes, 1);
	spin1_send_mc_packet(KEY_BINW_ADDR, (uint)binWidth, 1);
	spin1_send_mc_packet(KEY_MIDV_ADDR, (uint)midVals, 1);

	// it's done!
	if(msg->seq==0xFFFF) {
		io_printf(IO_STD, "Parameters are set!\n"); sark_delay_us(DEF_SPIN_DELAY);
	}
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
	uint *sdramBuf;
	uint nChunks, nRest;
	for(i=0; i<vfMapCntr; i++) {
		if(varID==vfMap[i].varID) {
			sdramBuf = vfMap[i].sdramLoc;
			nChunks = vfMap[i].nStates / 64;
			nRest = vfMap[i].nStates % 64;
			if(nRest != 0) nChunks++;
			break;
		}
	}

	// then copy the corresponding pop-code from SDRAM
	// we don't use dma since the pop-code is small
	uint len;
	for(i=0; i<nChunks; i++) {
		if(i==nChunks-1 && nRest!=0) len=nRest; else len=64;
		spin1_memcpy(popc_msg.data, sdramBuf + i*len, len*sizeof(uint));
		spin1_send_sdp_msg(&popc_msg, SDP_TIMEOUT);
	}
}

void circulatePopCodes()
{

}

void circulateVal(sdp_msg_t *msg)
{
	ushort varID = (ushort)msg->arg1;	// put the varID in arg1
	uint val = msg->arg2;				// and the value in arg2
	// NO: don't stream variable more than 1 in sdp. Why, because core-1 will generate the same key
	// that might not be received by minions --> will introduce packet drops!!! and RTE!!!
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
void decodeVal(ushort varID)
{
	// disable sdp communication
	if(leadApp) spin1_callback_off(SDP_PACKET_RX);

	// TODO: then do the computation
	ushort i, destX, destY, popCodesType, nStates;
	uint *result, *bw;
	REAL value, _valRange, _extValRange, _cardinal, _m, _b;
	REAL border1, border2, tmp;
	for(i=0; i<vfMapCntr; i++){
		if(vfMap[i].varID==varID) {
			nStates = vfMap[i].nStates;
			popCodesType = vfMap[i].popCodesType;
			destX = vfMap[i].destChipX;
			destY = vfMap[i].destChipY;
			value = vfMap[i].val_in;
			_valRange = REAL(vfMap[i].valRange);
			_cardinal = REAL(vfMap[i].nStates);
			result = vfMap[i].sdramLoc;
			bw = vfMap[i].bwLoc;
			break;
		}
	}
	uint ActiveState;
	switch(popCodesType) {
	case PMF_SINGLE: {
			_m = (_cardinal-1)/(2*_valRange);
			_b = (_cardinal-1)-(_m*_valRange);

			// TODO: round() is not found!!! **************************************************

			//ActiveState = uint(round(_m*value+_b));
			for(i=0; i<nStates; i++)
				pcDTCMbuf[i] = 0;
			pcDTCMbuf[ActiveState] = 1;
		}
		break;

	case PMF_GAUSSIAN: {
			// get the binwidth
			flDMAfinish = 0;
			spin1_dma_transfer(DMA_FETCH_BW_TAG, (void *)bw, (void *)bwDTCMbuf, DMA_READ, nStates*sizeof(uint));
			while(!flDMAfinish){}
			_extValRange = PMF_GAUSSIAN_SPREAD_RATIO * _valRange;
			for(i=0; i<_cardinal; i++) {
				tmp = getRealFromUint(bw[i]);
				border1 = i * tmp - _extValRange;
				border2 = border1 + tmp-1;
				tmp = gar(border1, border2, value);
				pcDTCMbuf[i] = getUintFromREAL(tmp);
			}
			normalizeStates(pcDTCMbuf);
		}
		break;
		//flDMAfinish = 0; --> no need to wait, it just store to sdram
		spin1_dma_transfer(DMA_STORE_PC_TAG, (void *)result, (void *)pcDTCMbuf, DMA_WRITE, nStates*sizeof(uint));

	}

	// next, send notification to host that the value has been processed

	popc_msg.tag = DEF_IPTAG;				// IPTag 1
	popc_msg.dest_port = PORT_ETH;			// Ethernet
	popc_msg.dest_addr = sv->eth_addr;		// Nearest Ethernet chip
	popc_msg.flags = 0x07;
	popc_msg.srce_port = MASTER_CORE;		// master will ALWAYS in core 1
	popc_msg.srce_addr = sv->p2p_addr;
	popc_msg.cmd_rc = REPLY_SEND_VAR_TO_DECODE;
	popc_msg.arg1 = varID;
	spin1_send_sdp_msg(&popc_msg, SDP_TIMEOUT);

	// then send to the corresponding chip containing the FNode
	sendPopCodes(varID, destX, destY);

	// finally, enable sdp communication again
	if(leadApp) spin1_callback_on(SDP_PACKET_RX, receiveSDP, 0);
}

void normalizeStates(uint *vector)
{

}

uint getUintFromREAL(REAL val)
{

}

REAL getRealFromUint(uint val)
{

}

/* averagingVal() computes the crisp value from a population code
*/
void averagingVal(uint varID)
{

}

/*--------------------------------------------------------------------------------*/
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
	// TODO: re-write using DMA
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
	// TODO: re-write using DMA
}
/*--------------- end of getBinWidthParam() and getMidValsParam() -----------------*/

/* receiveSDP() will process SDP sent by host to core-1 on port-1
 * port-0 is prohibited because it is dedicated for debugging purpose
 *
 * TODO: port argument (see SDP_PORT_HOST and SDP_PORT_SPIN in spinFG.h)
 * port-1 : from HOST
 * port-2 : internal spiNNaker machine
 */
void receiveSDP(uint mailbox, uint port)
{
	sdp_msg_t *msg = (sdp_msg_t *) mailbox;

	if(port==SDP_PORT_HOST){
		switch (msg->cmd_rc) {
		case HOST_SAY_HELLO:
			io_printf(IO_STD, "Hello host...\n"); spin1_delay_us(DEF_SPIN_DELAY);
			break;

		case HOST_SEND_NODE_IO_PARAM:					// set variables ID
			setupParam(msg);
			break;

		case HOST_SEND_VAR_TO_DECODE:
			circulateVal(msg);							// the payload can be a stream of several variableIDs

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
	}
	else if(port==SDP_PORT_SPIN) {
		switch(msg->cmd_rc) {
		case FNODE_SEND_POPCODES_TO_IO:
			circulatePopCodes(msg->seq);	// seq contains variID
			break;
		}
	}
	spin1_msg_free (msg);
}

uint initMe()
{
	uint success = 1;			// in c, true is any value except 0; false is zero!!!
    vfMapCntr = 0;

	uint myAppID;

	// check if this is in chip <0,0> and the app-id is IO_MASTER_ID
	myAppID = sark_app_id();
	myChipID = sark_chip_id();
	myCoreID = sark_core_id();

	if(myChipID !=0 && myAppID != IO_NODE_ID) {
		io_printf(IO_STD, "Please put me on chip<0,0> and assign me app-ID %d. Thank you!\n", IO_NODE_ID);
		return 0;
	}

	if(leadAp) {
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


	io_printf(IO_STD, "IONode built-%d.%d.%d.%d has passed its initialization!\n", VERSION, REVISION, PTAG, PCNTR);
	return success;
}

void dmaDone(uint tid, uint tag)
{
	flDMAfinish = 1;
}

void c_main()
{
	if(initMe()) {
		//spin1_callback_on (MC_PACKET_RECEIVED, receiveMC, 0);
		spin1_callback_on (MCPL_PACKET_RECEIVED, receiveMCPL, 1);
		spin1_callback_on (SDP_PACKET_RX, receiveSDP, 0);
		spin1_callback_on (DMA_TRANSFER_DONE, dmaDone, 1);
		spin1_start(SYNC_NOWAIT);
	}
}

REAL gar(REAL border1, REAL border2, REAL value)
{

}

REAL errf(REAL x)
{

}

/* dari myFG:
double CPMF::errf(double x)
{
	double result;
	double t = 1/(1+0.5*fabs(x));
	double x2 = pow(x,2);
	double t2 = t*t;
	double t3 = t*t2;
	double t4 = t*t3;
	double t5 = t*t4;
	double t6 = t*t5;
	double t7 = t*t6;
	double t8 = t*t7;
	double t9 = t*t8;
	double tau = t*exp(-x2 - 1.26551223 + 1.00002368*t + 0.37409196*t2 + 0.09678418*t3 - 0.18628806*t4 + 0.27886807*t5 - 1.13520398*t6 + 1.48851587*t7 - 0.82215223*t8 + 0.17087277*t9);
	if(x>=0) result = 1.0 - tau; else result = tau -1.0;
	return result;
}

double CPMF::gar(double a, double b, double i)
{
	double tv = _TwoVariance;
	double y = 0.5*(errf(3.0*(b-i)/sqrt(2.0*tv)) - errf(3.0*(a-i)/sqrt(2.0*tv)));
	return y;
}
*/

/* NOTE: in c, true is any value except 0; false is zero!!!
 *
 * */
