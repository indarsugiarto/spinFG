/*
 * spinFG.h
 *
 *  Created on: 23 Sep 2015
 *      Author: indi
 */

#ifndef SPINFG_H_
#define SPINFG_H_

/* History - Version
 * 1: initial remake of my old factor graph, now with FFG version
 *
 * History - Revision
 * 1: with big dma buffer, remark: too complicated for small core
 * 2: smaller dma buffer (limited to SDP-size)
 *
 * History - Performance tagging
 * 1: USE MC packet for sending parameters
 *
 * History - Performance counter : just to check if I compile the newest code
*/




#define DEF_SPIN_DELAY	50000
#define SDP_TIMEOUT		10		// beware, too big value will yield "WDOG"
#define N_CHIP_ROW		2
#define MAX_NSTATES		192		// 16*12, each core (total 16) will handle 12 bins
#define MAX_SDP_DATA	256		// according to API documentation, in bytes already!!!
#define MAX_SDP_CHUNK	192		// will be used to determine how much memory is allocated in DTCM
								// during DMA transaction, the formula = MAX_SDP_DATA * MAX_SDP_CHUNK
								// eg. 256*100/1024 = 32 kbytes

#define MASTER_ID				128
#define WORKER_ID				192
#define MASTER_CORE				1
#define IO_NODE_ID              168             // FNodeIO will be implemented on all cores in chip<0,0>

//#define DTCM_BUF_SIZE 		MAX_SDP_DATA * MAX_SDP_CHUNK
#define DTCM_BUF_SIZE			49152	// for eficiency, DTCM_BUF_SIZE must be a factor of 256 (MAX_SDP_DATA)
								// 49152 = 256*192

/* SDP-related */
// CMD_RC of the SDP packet
#define HOST_SAY_HELLO			0x000
#define HOST_SEND_PARAMS		0x001
#define HOST_REQUEST_PARAMS		0x002
#define HOST_SEND_FACTOR		0x003
#define HOST_REQUEST_FACTOR		0x004
#define HOST_SIGNALS_START		0x010
#define HOST_SIGNALS_PAUSE		0x011
#define HOST_SIGNALS_STOP		0xFFFF
#define REPLY_SETINTPACT_CHUNK	0x100
#define REPLY_SETINTFACT_DONE 	0x101
#define REPLY_READBACKINTFACT	0x102
#define REPLY_READBACKFACT_DONE	0x103

#define HOST_SEND_NODE_IO_PARAM	0x201
#define HOST_SEND_VAR_TO_DECODE 0x202
#define HOST_REQUEST_POPCODE	0x203
#define HOST_SEND_BIN_WIDTH_PARAM	0x204	// unfortunately, binWidth and midVals will be tightly coupled with the variable itself
#define HOST_SEND_MID_VALS_PARAM	0x205
#define REPLY_SEND_VAR_TO_DECODE	0x212

#define FNODE_SEND_POPCODES_TO_IO	0x301
#define REPLY_FNODE_SEND_POPCODES	0x311

// Others
#define DEF_IPTAG				1		// default IPTag for this application
#define DEF_RECV_PORT			17899
#define DEF_ERR_TAG				2		// use IPTag 2 for sending error
#define STD_ERR_PORT			17900

#define SDP_PORT_HOST			1
#define SDP_PORT_SPIN			2

/* Memory and DMA related */
#define SDRAM2DTCM_TAG		1
#define DTCM2SDRAM_TAG		2
//#define sdramIntFactID		0
//#define sdramOperandID		1
//#define sdramResultID		2
#define SDRAM_HEAP_TAG			0
#define SYSRAM_HEAP_TAG			1

/* MCP routing stuffs */
#define ROUTE_TO_CORE(core)        (1 << (core + 6))

/* cmd decoded in MC packet */
#define MASTER_SEND_PING			0	// can be used as report status as well
#define MASTER_SEND_PARAM_NS		1
#define MASTER_SEND_VAR_ID			2
#define MASTER_SEND_RESULT_ADDR		3
#define MASTER_SEND_MSGA_ADDR		11
#define MASTER_SEND_MSGB_ADDR		12
#define MASTER_SEND_MSGC_ADDR		13
#define MASTER_SEND_UPDATE_PARAMS	14
#define MASTER_SEND_N_CORES_AVAIL	21

#define PMF_GAUSSIAN				0
#define PMF_SINGLE					1
#define PMF_GAUSSIAN_SPREAD_RATIO	2

typedef struct factor {
	ushort ID;
	uchar nScope;
	ushort scopeID[3];				// each factor node has at most 3 variables as its scope
	uchar nStates;					// so, it must less than 256
	uchar Xchip, Ychip;
} factor_t;

typedef struct io {
		ushort nStates;
		ushort nVars;
		ushort popCodesType;				// 0 = Gaussian, 1 = single
		uint valRange;
		ushort varID;
        uint val_in;
        uint val_out;
        uint *sdramLoc;             // this is where the population code is stored in SDRAM
		uint *bwLoc;				// the location for binWidth in sdram
		uint *mvLoc;				// the location of midVals in sdram
		//ushort factorID;            // So, we will know where the population code will be delived
		ushort destChipX;            // later, factorID will be mapped to destChipX and destChipY
		ushort destChipY;
} io_t;


/* The idea of io_t:
 * Chip<0,0> will store the mapping of Factor Node to SpiNNaker chip
 * */

#endif /* SPINFG_H_ */

/* NOTE: Problem with #define MAX_SDP_CHUNK	100
 * Akan menghasilkan residu karena 100 bukan kelipatan 2 pangkat
 *

 *-------------- regarding MC packet format ------------------------
 * /* generic key format: [arg2, arg1, cmd, dest] *
 *
 * masterKey should be used only by NodeMaster to send parameter or command
 * to all worker cores. "cmd" part of the key can be one of the following code:
 * - parameters:
 *    1 = send nScope, nStates and factor address; arg2 = nScope, arg1 = nStates, payload = factor address in sdram
 *    2 = send varID; arg2.arg1 = scopeID[0], payload: scopeID[1], scopeID[2]
 *    3 = send result address, payload: result address in sdram
 *   11 = send msgFromA address, payload: msgFromA address in sysram
 *   12 = send msgFromB address, payload: msgFromB address in sysram
 *   13 = send msgFromC address, payload: msgFromC address in sysram
 * - command:
 *   21 = a new input message has come, please fetch from SDRAM; payload = varID of the incoming message
 *   31 = start factor product on specific output variable; payload = argument-varID
 *   41 = start marginalization on specific output variable; payload = argument-varID
 *

 *  workerKey doesn't use any payload. The possible cmd format:
 *    1 = not yet ready to perform sum-product; arg1 = coreID
 *   21 = acknowledge input message; arg1 = coreID, arg2 = varID
 *   31 = my part in factor product is complete; arg1 = coreID
 *   41 = my part in marginalization is complete; arg1 = coreID
 *------------------------------------------------------------------------
  * */
