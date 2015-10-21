/**** FNodeMaster.c
*
* SUMMARY
*  master-core to be run on core-17
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
* 1. SEND and RECEIVE PARAMS -- done
* 2. SEND and RECEIVE FACTOR -- done
* 3. ALLOCATE WORKER REGION
* 4. ALLOCATE SDRAM buffer for each incoming and outgoing messages -- done
*
*******/

// SpiNNaker API
#include <spin1_api.h>
#include "../../Common/spinFG.h"
#include "../../Common/spinErrDef.h"
#include "../../Common/utils.h"

#define DEF_WORKER_KEY	255
#define DEF_MASTER_KEY	1

/* See: how-dma_data_t-works.png
 * allocatedSDRAM = n*allocatedDTCM + allocatedDiff
 */
typedef struct _dma_data {
	uint allocatedSDRAM;			// how much (in bytes) for allocating memory for a vector
	uint nChunk;
	uint chunkCntr;
	uint chunkRest;
	uint *offset;
	uint dtcmBuffer[MAX_SDP_DATA];
} dma_data_t;

/* SpiNNaker-architecture related */
sdp_msg_t my_msg;
sdp_msg_t err_msg;

uchar availCore;                // number of available cores for computation
uchar lW[16];					// load (bins) per worker, initially 0, MAX = 16

/* Factor graph related */
factor_t fMe;

ushort idMsgA;					// when the external node send a message, it will include this
ushort idMsgB;
ushort idMsgC;

uint *msgBufBase;
uint *msgFromA;
uint *msgFromB;
uint *msgFromC;
uint *msgToA;					// will be computed when msgFromB and msgFromC have arrived
uint *msgToB;					// will be computed when msgFromA and msgFromC have arrived
uint *msgToC;					// will be computed when msgFromA and msgFromB have arrived

uchar msgToCompute;				// 0 means want to compute for A,
								// 1 means want to compute for B, and
								// 2 means want to compute for C

uint *sdramVectorBase;			// location in SDRAM to store the vector
uint *sdramIntFactor;			// for internal factor vector
uint *sdramOperand;				// for operand vector
uint *sdramResult;				// for result vector

uchar memIsInitialized;			// 0 means it is still virgin, 1 means not anymore

dma_data_t dmaData;

volatile uchar sdpInProgress;
volatile uchar bRunning;		// don't optimize, might change somewhere

/************** routing stuffs *************/
/* generic key format: [arg2, arg1, cmd, dest] */
uint masterKey;
uint workerKey;				// this is the routing pattern to the available working cores

/* Forward declaration */
void readbackInternalFactor();
void prepareDMA();
int distributeParams();

/* Implementation */


void broadcastPing()
{
	workerKey = MASTER_SEND_PING << 8;
	workerKey |= DEF_WORKER_KEY;
	spin1_send_mc_packet(workerKey, 0, 1);
}


/* SYNOPSIS
 *		freeHeap will return 1 if success, otherwise it will return 0
 *		section == 0 means just SDRAM
 *		section == 1 means including SYSRAM
 */
uint freeHeap(uint section)
{
	if(section==1)
		sark_xfree(sv->sysram_heap, sdramVectorBase, ALLOC_LOCK);
	sark_xfree(sv->sdram_heap, msgBufBase, ALLOC_LOCK);
	return 0;
}

/* SYNOPSIS
 *		prepareDMA(factor_t f) will compute the number of chunk for each dmaCntr and dtcmCntr.
 *		It relies on parameters in the factor_t for determining the size of allocated memory.
 * */
void prepareDMA(factor_t f)
{
	uchar i;
	// compute the necessary size for memory allocation for a vector
	// allocatedVector determines how many bytes in total is required to contain all vector's values
	dmaData.allocatedSDRAM = f.nStates;
	for(i=1; i<f.nScope; i++) {
		dmaData.allocatedSDRAM *= f.nStates;
	}
	dmaData.allocatedSDRAM *= sizeof(uint);

	/* prepare dmaCounter used for moving data to/from SDRAM/DTCM */
	// how many dmaData chunk then?
	dmaData.nChunk = dmaData.allocatedSDRAM / MAX_SDP_DATA;
	dmaData.chunkRest = dmaData.allocatedSDRAM % MAX_SDP_DATA;
	if(dmaData.chunkRest != 0)
		dmaData.nChunk++;
}

int distributeParams()
{
	int success = 0;
	uint payload;
	// 1 = send nScope, nStates and factor address; arg2 = nScope, arg1 = nStates, payload = factor address in sdram
	workerKey = (uint)fMe.nScope << 24;
	workerKey |= (uint)fMe.nStates << 16;
	workerKey |= (uint)MASTER_SEND_PARAM_NS << 8;
	workerKey |= DEF_WORKER_KEY;
	payload = (uint)sdramIntFactor;
	if (!spin1_send_mc_packet(workerKey, payload, 1)) {
		success = -1;
		reportErr(&err_msg, SEND_MC_PACKET_NS_ERR_ID);
	}

	// 2 = send varID; arg2.arg1 = scopeID[0], payload: scopeID[1], scopeID[2]
	workerKey = (uint)fMe.scopeID[0] << 24;
	workerKey |= (uint)MASTER_SEND_VAR_ID << 8;
	workerKey |= DEF_WORKER_KEY;
	payload = 0;
	if(fMe.nScope>1)
		payload = (uint)fMe.scopeID[1] << 16;
	if(fMe.nScope>2)
		payload |= (uint)fMe.scopeID[2];
	if(!spin1_send_mc_packet(workerKey, payload, 1)) {
		success = -1;
		reportErr(&err_msg, SEND_MC_PACKET_VARID_ERR_ID);
	}

	// 3 = send result address, payload: result address in sdram
	workerKey = MASTER_SEND_RESULT_ADDR << 8;
	workerKey |= DEF_WORKER_KEY;
	payload = (uint)sdramResult;
	if(!spin1_send_mc_packet(workerKey, payload, 1)) {
		success = -1;
		reportErr(&err_msg, SEND_MC_PACKET_RESULT_ADDR_ERR_ID);
	}

	// 11 = send msgFromA address, payload: msgFromA address in sysram
	workerKey = MASTER_SEND_MSGA_ADDR << 8;
	workerKey |= DEF_WORKER_KEY;
	payload = (uint)msgFromA;
	if(!spin1_send_mc_packet(workerKey, payload, 1)) {
		success = -1;
		reportErr(&err_msg, SEND_MC_PACKET_MSGA_ADDR_ERR_ID);
	}

	// 12 = send msgFromB address, payload: msgFromB address in sysram
	workerKey = MASTER_SEND_MSGB_ADDR << 8;
	workerKey |= DEF_WORKER_KEY;
	payload = (uint)msgFromB;
	if(!spin1_send_mc_packet(workerKey, payload, 1)) {
		success = -1;
		reportErr(&err_msg, SEND_MC_PACKET_MSGB_ADDR_ERR_ID);
	}

	// 13 = send msgFromC address, payload: msgFromC address in sysram
	workerKey = MASTER_SEND_MSGC_ADDR << 8;
	workerKey |= DEF_WORKER_KEY;
	payload = (uint)msgFromC;
	if(!spin1_send_mc_packet(workerKey, payload, 1)) {
		success = -1;
		reportErr(&err_msg, SEND_MC_PACKET_MSGC_ADDR_ERR_ID);
	}

	// 14 = send command to workers to compute their part
	workerKey = MASTER_SEND_UPDATE_PARAMS << 8;
	workerKey |= DEF_WORKER_KEY;
	if(!spin1_send_mc_packet(workerKey, 0, 1)) {
		success = -1;
		reportErr(&err_msg, SEND_MC_PACKET_CMD_TO_UPDATE_PARAMS_ID);
	}

	if(success != -1)
		broadcastPing();

	return success;
}

/* SYNOPSIS
 *		Basically, we need only two keys:
 *		xxxxxx01 -> will be sent to core-1
 *		xxxxxxff -> will be sent to all workers
 *		So, the masking is 0x000000FF
 * */
int initRoutingTable()
{
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

/* Format parameter sent via SDP as data payload:
 * factor ID, nScope, Scope-ID, nStates
 */
void setParam(uint *param)
{
	uint msgLength;
	spin1_memcpy((void *)fMe.scopeID, (void *)param, fMe.nScope*sizeof(ushort));

	// then allocate memory, beware that the memory will be freed
	// "sark_xfree_id" will return number of blocks freed. --> wrong, it will return 1 if success
	if(memIsInitialized != 0) {
		if(freeHeap(1) == 0) {
			spin1_exit(-1);
			return;
		}
	}

	// First, prepare dmaData parameters. Here, the allocatedSDRAM (in bytes) is computed.
	prepareDMA(fMe);

	// second, allocate vector that contains the factor value in SDRAM
	msgLength = dmaData.allocatedSDRAM * 3;
	sdramVectorBase = (uint *)sark_xalloc (sv->sdram_heap, msgLength, SDRAM_HEAP_TAG, ALLOC_LOCK);
	if(sdramVectorBase == NULL) {
		reportErr(&err_msg, SDRAM_VECTOR_ALLOC_ERR_ID);
		spin1_exit(-1);	return;
	}
	else {
		// then split to the corresponding purpose
		msgLength = dmaData.allocatedSDRAM / sizeof(uint);
		sdramIntFactor = sdramVectorBase;
		sdramOperand = sdramVectorBase + msgLength;
		sdramResult = sdramOperand + msgLength;
	}

	// then allocate buffers in sysram to hold msgIn and msgOut
	msgLength = fMe.nStates * sizeof(uint) * 6;
	// we compute the length 6 times because we have 3 input messages and 3 output messages
	msgBufBase = (uint *)sark_xalloc(sv->sysram_heap, msgLength, SYSRAM_HEAP_TAG, ALLOC_LOCK);
	if(msgBufBase == NULL) {
		reportErr(&err_msg, SYSRAM_MSGBUF_ALLOC_ERR_ID);
		// then release previous allocated memory
		freeHeap(0); // just SDRAM
	}
	else {
		// then split to the corresponding input/output buffer
		msgFromA = msgBufBase;
		msgFromB = msgFromA + fMe.nStates;
		msgFromC = msgFromB + fMe.nStates;
		msgToA = msgFromC + fMe.nStates;
		msgToB = msgToA + fMe.nStates;
		msgToC = msgToC + fMe.nStates;
	}

	// if everything OK, nScope and nStates should remain
	memIsInitialized = 1;

#ifdef DEBUG_VERBOSE
	io_printf(IO_STD, "Allocated SDRAM = %d-bytes\n", dmaData.allocatedSDRAM); spin1_delay_us(SDP_TIMEOUT);
	io_printf(IO_STD, "DTCM buffer location : %08x\n", dmaData.dtcmBuffer); spin1_delay_us(SDP_TIMEOUT);
	io_printf(IO_STD, "FACT buffer location : %08x\n", sdramIntFactor); spin1_delay_us(SDP_TIMEOUT);
	io_printf(IO_STD, "OPND buffer location : %08x\n", sdramOperand); spin1_delay_us(SDP_TIMEOUT);
	io_printf(IO_STD, "REST buffer location : %08x\n", sdramResult); spin1_delay_us(SDP_TIMEOUT);
	io_printf(IO_STD, "msgFromA buffer location : %08x\n", msgFromA); spin1_delay_us(SDP_TIMEOUT);
	io_printf(IO_STD, "msgFromB buffer location : %08x\n", msgFromB); spin1_delay_us(SDP_TIMEOUT);
	io_printf(IO_STD, "msgFromC buffer location : %08x\n", msgFromC); spin1_delay_us(SDP_TIMEOUT);
	io_printf(IO_STD, "msgToA buffer location : %08x\n", msgToA); spin1_delay_us(SDP_TIMEOUT);
	io_printf(IO_STD, "msgToB buffer location : %08x\n", msgToB); spin1_delay_us(SDP_TIMEOUT);
	io_printf(IO_STD, "msgToC buffer location : %08x\n", msgToC); spin1_delay_us(SDP_TIMEOUT);
#endif

	// then distribute to workers
	if(distributeParams()==0)
		io_printf(IO_STD, "Parameters are set!\n");
}

void readbackParam()
{
	uint i;
	io_printf(IO_STD, "My factor ID is %d in chip <%d,%d>\n", fMe.ID, fMe.Xchip, fMe.Ychip);
	io_printf(IO_STD, "nScope = %d\n", fMe.nScope);
	io_printf(IO_STD, "Scope-ID = [ ");
	for(i=0; i<fMe.nScope; i++)
		io_printf(IO_STD, "%d ", fMe.scopeID[i]);
	io_printf(IO_STD, "]\nnStates = %d\n", fMe.nStates);
}


void checkMemCopy(uint tid, uint tag)
{
	if(tag==SDRAM2DTCM_TAG) {
		uint len;
		dmaData.chunkCntr++;

		if(dmaData.chunkCntr == dmaData.nChunk && dmaData.chunkRest > 0)
			len = dmaData.chunkRest;
		else
			len = MAX_SDP_DATA;
		my_msg.cmd_rc = REPLY_READBACKINTFACT;
		my_msg.arg1 = fMe.nStates;
		my_msg.arg2 = len / sizeof(uint);
		my_msg.arg3 = len;
		// copy into SDP buffer and then send via SDP
		spin1_memcpy((void *)my_msg.data, (void *)dmaData.dtcmBuffer, len);
		my_msg.length = sizeof(sdp_hdr_t) + sizeof(cmd_hdr_t) + len;
		spin1_send_sdp_msg(&my_msg, SDP_TIMEOUT); // beware, too big value of SDP_TIMEOUT will yield "WDOG"

		// then adjust the offset
		dmaData.offset += len/sizeof(uint);

		if(dmaData.chunkCntr < dmaData.nChunk) {
			spin1_schedule_callback (readbackInternalFactor, 0, 0, 1);
		}
		else {
			io_printf(IO_STD, "All packets have been sent! Check with REPLY_READBACKFACT_DONE !!!\n", my_msg.length);
			spin1_delay_us(DEF_SPIN_DELAY);

			my_msg.cmd_rc = REPLY_READBACKFACT_DONE;
			my_msg.arg1 = fMe.nStates;
			my_msg.arg2 = dmaData.nChunk;
			my_msg.arg3 = dmaData.chunkCntr;
			my_msg.length = sizeof(sdp_hdr_t) + sizeof(cmd_hdr_t);
			spin1_send_sdp_msg(&my_msg, SDP_TIMEOUT);
		}
	}
}

/* SUMMARY
 * 		readbackInternalFactor() is used to send internal factor function
 * 		(as a vector) to host PC.
 *
 * SYNOPSIS
 * 		Internal factor will be sent to host using the following format:
 * 		nStates will be in arg1
 * 		nData will be in arg2
 * 		payload = data[0], data[1], ...
 */
void readbackInternalFactor()
{
	// simply request dma
	//io_printf(IO_STD, "readbackInternalFactor() is triggered! Start requesting dma...\n");
	//spin1_delay_us(DEF_SPIN_DELAY);

	spin1_dma_transfer(SDRAM2DTCM_TAG, dmaData.offset, dmaData.dtcmBuffer, DMA_READ, MAX_SDP_DATA);
}



/* SUMMARY
 * 		getInternalFactor() is used to retrieve internal factor function
 * 		(as a vector) for a factor node
 *
 * SYNOPSIS
 * 		Internal factor will be sent from host using the following format:
 * 		data[allocatedVector_cntr], data[allocatedVector_cntr+1], ...
 * 		where nd (number of data) is determined by msg.arg2, while ns (nStates)
 * 		should be mentioned in msg.arg1
 * 		During this process, we assume that host PC doesn't send run command.
 *              nd = number of data in the SDP packet
 */
void getInternalFactor(uint nd, uint *param)
{
	uint chk;
	uint len = nd*sizeof(uint);

	dmaData.chunkCntr++;
	/* test-case: reply to host for every packet. later, it is better only after all packets have been received	 */
	my_msg.cmd_rc = REPLY_SETINTPACT_CHUNK;
	my_msg.arg1 = fMe.nStates;								// the expected nStates
	my_msg.arg2 = dmaData.chunkCntr;							// my counter
	my_msg.arg3 = dmaData.nChunk - dmaData.chunkCntr;				// how many chunks left are still expected to arrive?
	my_msg.length = sizeof(sdp_hdr_t) + sizeof(cmd_hdr_t);	// send without data
	chk = spin1_send_sdp_msg (&my_msg, SDP_TIMEOUT);
#ifdef DEBUG_VERBOSE
	if(chk==0)
		io_printf(IO_STD, "Fail to send sdp!");
	//else
	//io_printf(IO_STD, "Send back the acknowledge to host.\n");
	spin1_delay_us(DEF_SPIN_DELAY);
#endif

	// first, copy into DTCM buffer
	spin1_memcpy((void *)dmaData.dtcmBuffer, (void *)param, len);

	// second, send to SDRAM via dmaData
	spin1_dma_transfer(DTCM2SDRAM_TAG, dmaData.offset, dmaData.dtcmBuffer, DMA_WRITE, len);

	// third, increase the offset
	/* dtcmCntr.offset += len; //ini menghasilkan offset yang salah */
	dmaData.offset += nd;

	// fourth, check if all data have been stored in SDRAM
	if(dmaData.chunkCntr == dmaData.nChunk) {
		sdpInProgress = 0;
#ifdef DEBUG_VERBOSE
		io_printf(IO_STD, "All chunks have been stored in SDRAM\n");
		spin1_delay_us(DEF_SPIN_DELAY);
#endif
		// then report to the host
		my_msg.cmd_rc = REPLY_SETINTFACT_DONE;
		my_msg.arg1 = fMe.nStates;
		my_msg.arg2 = (dmaData.offset - sdramIntFactor);	// how many bytes have been stored?
		my_msg.length = sizeof(sdp_hdr_t) + sizeof(cmd_hdr_t);
		chk = spin1_send_sdp_msg (&my_msg, SDP_TIMEOUT);
#ifdef DEBUG_VERBOSE
	if(chk==0)
		io_printf(IO_STD, "Fail to send a report via SDP!");
	else
		io_printf(IO_STD, "Send report with code-%d to host that all data have arrived!\n", my_msg.cmd_rc);
	spin1_delay_us(DEF_SPIN_DELAY);
#endif
	}
}

void hello()
{
	io_printf(IO_STD, "Hello host...\n"); spin1_delay_us(DEF_SPIN_DELAY);
	broadcastPing();
}

/* receiveSDP() will process SDP sent by host to core-17 on port-1
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
		hello();
		break;
	case HOST_SEND_PARAMS: // set parameter data
		if(bRunning == 0) {
			fMe.ID = msg->arg1;
			fMe.nStates = msg->arg2;
			fMe.nScope = msg->arg3;
			setParam(data);
		}
		else {
			io_printf(IO_STD, "NO! BP is still in running mode!\n");
			spin1_delay_us(DEF_SPIN_DELAY);
		}
		break;

	case HOST_REQUEST_PARAMS:										// command to read back the parameter data
		readbackParam();
		break;

	case HOST_SEND_FACTOR:											// set internal factor data
		if(msg->arg1 != fMe.nStates) {
			io_printf(IO_STD, "Wrong number of states!\n");
			spin1_delay_us(DEF_SPIN_DELAY);
		}
		else {
			if(bRunning == 0) {										// check if BP is running
				if(sdpInProgress==0) {								// is this the first delivery, if yes
					sdpInProgress = 1;								// indicate the first delivery is processed
					dmaData.chunkCntr = 0;							// prepare chunkCntr
					dmaData.offset = sdramIntFactor;				// point to the factor's vector in SDRAM
				}
				getInternalFactor(msg->arg2, data);
			}
			else {
				io_printf(IO_STD, "NO! BP is still in running mode!\n");
				spin1_delay_us(DEF_SPIN_DELAY);
			}
		}
	break;

	case HOST_REQUEST_FACTOR:										// command to read internal factor
		if(bRunning == 0) {
			dmaData.chunkCntr = 0;									// prepare the dmaCntr
			dmaData.offset = sdramIntFactor;						// point to the factor's vector in SDRAM

			io_printf(IO_STD, "Triggering readbackInternalFactor()\n");
			spin1_delay_us(DEF_SPIN_DELAY);

			spin1_schedule_callback (readbackInternalFactor, 0, 0, 1);
		}
		else {
			io_printf(IO_STD, "NO! BP is still in running mode!\n");
			spin1_delay_us(DEF_SPIN_DELAY);
		}
	  break;

	case HOST_SIGNALS_START:										// command to start factor graph
		// first, check if memory is allocated
		if(memIsInitialized == 0) {
			io_printf(IO_STD, "Memory is not initialized. Have you sent the parameters?\n");
			spin1_delay_us(DEF_SPIN_DELAY);
		}
		else {
			bRunning = 1;
		}
	  break;

	case 5: // command to pause
		bRunning = 0;
		break;

	case HOST_SIGNALS_STOP:
		if(memIsInitialized == 1)
			freeHeap(1);
		bRunning = 0;
		break;

	default:
	  // unexpected packet!
		io_printf (IO_STD, "Factor node ID %d in chip <%d,%d> receive unknown SDP!\n", fMe.ID, fMe.Xchip, fMe.Ychip);
		spin1_delay_us(DEF_SPIN_DELAY);
		break;
	  }
	  spin1_msg_free (msg);
}

int computeWorkingCoreLoad()
{
	// then compute, how much load for each worker
	int success = 0;
	uint payload;
	// 13 = send msgFromC address, payload: msgFromC address in sysram
	workerKey = MASTER_SEND_N_CORES_AVAIL << 8;
	workerKey |= DEF_WORKER_KEY;
	payload = (uint)availCore;
	if(!spin1_send_mc_packet(workerKey, payload, 1)) {
		success = -1;
		reportErr(&err_msg, SEND_MC_PACKET_N_CORES_AVAIL_ERR_ID);
	}

	return success;
}

int initMe()
{
	uint myAppID, myCoreID;

	// check if this is core-1 and the app-id is MASTER_ID
	myAppID = sark_app_id();
	myCoreID = spin1_get_core_id();
	if(myCoreID != 1 && myAppID != MASTER_ID) {
		io_printf(IO_STD, "Please put me on core-1 and assign me app-ID 16. Thank you!\n");
		return -1;
	}

	// get this chip's ID
	uint chipID = spin1_get_chip_id();
	fMe.Xchip = chipID >> 8;
	fMe.Ychip = chipID & 0xff;


	if (initRoutingTable()!=0) {
		io_printf(IO_STD, "Fail to init router!\n");
		spin1_delay_us(DEF_SPIN_DELAY);
	    return -1;
	}


	// about memory allocation
	dmaData.allocatedSDRAM = 0;				//memory is not yet allocated

	// TODO: check if heap is already allocated in memory
	memIsInitialized = 0;

	bRunning = 0;							//indicate that the belief propagation is not running

	availCore = countAvailProc();
	if(availCore==0) {
		io_printf(IO_STD, "No worker is available.\n");
		spin1_delay_us(DEF_SPIN_DELAY);
	}
	else {
		io_printf(IO_STD, "Found %d workers ready to work!\n", availCore);
		spin1_delay_us(DEF_SPIN_DELAY);
		computeWorkingCoreLoad();
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

	io_printf(IO_STD, "FNodeMaster built-%d.%d.%d.%d has passed its initialization!\n", VERSION, REVISION, PTAG, PCNTR);
	return 0;
}

void c_main()
{
	if(initMe()==0) {
		spin1_callback_on (MC_PACKET_RECEIVED, receiveMC, 0);
		//spin1_callback_on (MCPL_PACKET_RECEIVED, receiveMCPL, 0);
		spin1_callback_on (SDP_PACKET_RX, receiveSDP, 2);
		spin1_callback_on (DMA_TRANSFER_DONE, checkMemCopy, 1);
	    spin1_start(SYNC_NOWAIT);
		//spin1_start(SYNC_WAIT);
	}
}

/* NOTE:
 * 1. SDP will be used to send myID, parameters (such as local factor),
 *    and start the synchronization because timing problem!
 * 2. Memory allocation
 *    nStates = 12*16 = 192
 *    factor vector = 192*192*192*4/(1024*1024) = 27MB
 *    total 3 vectors = 27*3 = 81MB   //3 vectors: factor, operand, result
 *
 *
 */

