#ifndef SPINERRDEF_H
#define SPINERRDEF_H

#define DUMMY_ERR_ID					0
#define DUMMY_ERR_MSG					"This is a hello message for error reporting"

/********************* heap related ******************/
/* SDRAM heap related */
#define SDRAM_VECTOR_RELEASE_ERR_ID		1
#define SDRAM_VECTOR_RELEASE_ERR_MSG	"Fail to release SDRAM heap for vectors"

#define SDRAM_VECTOR_ALLOC_ERR_ID		11
#define SDRAM_VECTOR_ALLOC_ERR_MSG		"Fail to allocate SDRAM heap for vectors"

/* SYSRAM heap related */
#define SYSRAM_MSGBUF_RELEASE_ERR_ID	21
#define SYSRAM_MSGBUF_RELEASE_ERR_MSG	"Fail to release SYSRAM heap for Message Buffers"

#define SYSRAM_MSGBUF_ALLOC_ERR_ID		31
#define SYSRAM_MSGBUF_ALLOC_ERR_MSG		"Fail to allocate SYSRAM heap for Message Buffers"

/*****************************************************/

/****************** MC packet related ****************/

#define SEND_MC_PACKET_NS_ERR_ID			41
#define SEND_MC_PACKET_NS_ERR_MSG			"Fail to send an MC for nScope, nStates, sdramIntFactor"

#define SEND_MC_PACKET_VARID_ERR_ID			42
#define SEND_MC_PACKET_VARID_ERR_MSG		"Fail to send an MC for varIDs"

#define SEND_MC_PACKET_RESULT_ADDR_ERR_ID	43
#define SEND_MC_PACKET_RESULT_ADDR_ERR_MSG	"Fail to send an MC for Result address in SDRAM"

#define SEND_MC_PACKET_MSGA_ADDR_ERR_ID		44
#define SEND_MC_PACKET_MSGA_ADDR_ERR_MSG	"Fail to send an MC for msgFromA address in sysram"
#define SEND_MC_PACKET_MSGB_ADDR_ERR_ID		45
#define SEND_MC_PACKET_MSGB_ADDR_ERR_MSG	"Fail to send an MC for msgFromB address in sysram"
#define SEND_MC_PACKET_MSGC_ADDR_ERR_ID		46
#define SEND_MC_PACKET_MSGC_ADDR_ERR_MSG	"Fail to send an MC for msgFromC address in sysram"

#define SEND_MC_PACKET_N_CORES_AVAIL_ERR_ID	46
#define SEND_MC_PACKET_N_CORES_AVAIL_ERR_MSG	"Fail to send an MC informing workers how cores are active"

#define SEND_MC_PACKET_CMD_TO_UPDATE_PARAMS_ID	51
#define SEND_MC_PACKET_CMD_TO_UPDATE_PARAMS_MSG	"Fail to send an MC instructing workers to update their params"
/*****************************************************/

#endif // SPINERRDEF_H
