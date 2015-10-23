#include <spin1_api.h>
#include "../../Common/spinFG.h"

/* generic for debugging purpose */
#define DEBUG_VERBOSE
#define VERSION			1
#define REVISION		2
#define PTAG			1
#define PCNTR			8


#define bufTarget IO_BUF

uchar myCoreID;

factor_t fMe;
uint *sdramIntFactor;			// for internal factor vector
uint *sdramResult;				// for result vector
uint *myFactorPart;
uint *myResultPart;
uint *msgFromA;
uint *msgFromB;
uint *msgFromC;
uchar totalWorkersNum;
uint szJPD;						// block-size (in item and not in byte) of my part
//uint jpdIdx;					// starting index of the jpd that belongs to me, i.e, how many items in jpd that I'm responsible with


/* SYNOPSIS
 *		computeMyWorkingLoad() will compute which part will be processed by "this" core.
 * */

void computeMyWorkingLoad()
{
	io_printf(bufTarget, "Working at computing my load...\n");
	uint i, res, temp, offset = 0;
	uint total_Length = fMe.nStates;
	for(i=1; i<fMe.nScope; i++)
		total_Length *= fMe.nStates;
	//total_Length *= sizeof(uint);		// jangan dikalikan 4, karena jpd indexing bukan tentang ukuran byte

	uint *szBlk = (uint *)sark_alloc(totalWorkersNum, sizeof(uint));

	szJPD = total_Length / totalWorkersNum;
	temp = szJPD;				// digunakan untuk iterasi di bawah ini
	res = total_Length % totalWorkersNum;
	if(res != 0)				// misal, jumlah worker = 4, dengan index mulai dari 0, dan ada sisa 3-item
		if(myCoreID < res)		// | sisa-1 | sisa-2 | sisa-3 |
			szJPD++;			// | idx-0  | idx-1  | idx-2  | idx 3 |
								//    ++		++		++		none
	//jpdIdx = myCoreID*szJPD;	// wrong, karena sjJPD bisa tidak sama untuk semua core
	for(i=0; i<totalWorkersNum; i++) {
		if(i < res)
			szBlk[i] = szJPD;	// chunk with additional overhead
		else
			szBlk[i] = temp;	// chunk without additional overhead
	}
	for(i=0; i<myCoreID; i++) {
		offset += szBlk[i];
	}
	myFactorPart = sdramIntFactor + offset;
	myResultPart = sdramResult + offset;

	sark_free(szBlk);
}

void hello()
{
	uint cmd;
	io_printf(bufTarget, "nScope = %d, nStates = %d, sdramBuf at %x\n", fMe.nScope, fMe.nStates, sdramIntFactor);
	for(cmd = 0; cmd<fMe.nScope; cmd++){
		io_printf(bufTarget, "varID[%d] = %d\n", cmd, fMe.scopeID[cmd]);
	}
	io_printf(bufTarget, "sdramResult is stored at %x\n", sdramResult);
	io_printf(bufTarget, "msgFromA is stored at %x\n", msgFromA);
	io_printf(bufTarget, "msgFromB is stored at %x\n", msgFromB);
	io_printf(bufTarget, "msgFromC is stored at %x\n", msgFromC);
	io_printf(bufTarget, "Total workers report = %d\n", totalWorkersNum);
	io_printf(bufTarget, "My Block size = %d\n", szJPD);
	io_printf(bufTarget, "My offset for factor = %x\n", myFactorPart);
	io_printf(bufTarget, "My offset for result = %x\n", myResultPart);
}

void receiveMCPL(uint key, uint payload)
{
	uint cmd = (key >> 8) & 0x000000FF;
	switch(cmd) {
	case MASTER_SEND_PING:
		hello();
		break;
	case MASTER_SEND_PARAM_NS:	// 1 = send nScope, nStates and factor address; arg2 = nScope, arg1 = nStates, payload = factor address in sdram
		fMe.nScope = key >> 24;
		fMe.nStates = (key >> 16) & 0xFF;
		sdramIntFactor = (uint *)payload;
		break;
	case MASTER_SEND_VAR_ID:	// 2 = send varID; arg2.arg1 = scopeID[0], payload: scopeID[1], scopeID[2]
		fMe.scopeID[0] = key >> 24;
		if(fMe.nScope > 1)
			fMe.scopeID[1] = payload >> 16;
		if(fMe.nScope > 2)
			fMe.scopeID[2] = payload & 0xFFFF;
		break;
	case MASTER_SEND_RESULT_ADDR:
		sdramResult = (uint *)payload;
		break;
	case MASTER_SEND_MSGA_ADDR:
		msgFromA = (uint *)payload;
		break;
	case MASTER_SEND_MSGB_ADDR:
		msgFromB = (uint *)payload;
		break;
	case MASTER_SEND_MSGC_ADDR:
		msgFromC = (uint *)payload;
		break;
	case MASTER_SEND_N_CORES_AVAIL:
		totalWorkersNum = (uchar)payload;
		break;
	case MASTER_SEND_UPDATE_PARAMS:
		spin1_schedule_callback(computeMyWorkingLoad, 0, 0, 2);
		break;
	}
}

void checkMemCopy(uint tid, uint tag)
{

}

int initMe()
{
	uint i, myAppID;

	// check if this is core-1 and the app-id is MASTER_ID
	myAppID = sark_app_id();
	myCoreID = spin1_get_core_id();
	if(myCoreID == 1 || myAppID != WORKER_ID) {
		io_printf(IO_STD, "Please don't put me on core-1 and assign me app-ID %d. Thank you!\n", WORKER_ID);
		return -1;
	}
	else {
		myCoreID -= 2;	// so the lowest-id worker is 0
	}

	io_printf(bufTarget, "CPU-%d for FNodeWorker built-%d.%d.%d.%d has passed its initialization!\n", myCoreID, VERSION, REVISION, PTAG, PCNTR);
	return 0;
}

void c_main(void)
{
	if(initMe()==0) {
		spin1_callback_on (MCPL_PACKET_RECEIVED, receiveMCPL, 1);
		spin1_callback_on (DMA_TRANSFER_DONE, checkMemCopy, 0);
		spin1_start(SYNC_NOWAIT);
		//spin1_start(SYNC_WAIT);
	}
}

