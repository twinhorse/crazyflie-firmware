
#include <string.h>

#include "uwbp2pdist.h"

#include "FreeRTOS.h"
#include "task.h"

#include "debug.h"

#include "physicalConstants.h"

#define ANTENNA_OFFSET 154.6   // In meter

static int ANTENNA_DELAY = (ANTENNA_OFFSET*499.2e6*128)/299792458.0; // In radio tick

// Timestamps for ranging
static dwTime_t poll_tx;
static dwTime_t poll_rx;
static dwTime_t answer_tx;
static dwTime_t answer_rx;
static dwTime_t final_tx;
static dwTime_t final_rx;

static packet_t txPacket;
static volatile uint8_t curr_seq = 0;
static int curr_peer = 0;

float pressure = 0;
float temperature = 0;
float asl = 0;
bool pressure_ok = true;
int ertcnt = 20;
int cntout = 0;

static uint32_t timeout_p2p=65;


static void txcallback(dwDevice_t *dev)
{
  dwTime_t departure;
  dwGetTransmitTimestamp(dev, &departure);
  departure.full += (ANTENNA_DELAY / 2);

  switch (txPacket.payload[0]) {
    case LPS_P2P_POLL:
      //DEBUG_PRINT("sent LPS_P2P_POLL\n");
      poll_tx = departure;
      break;
    case LPS_P2P_FINAL:
      //DEBUG_PRINT("sent LPS_P2P_FINAL\n");
      final_tx = departure;
      break;
    case LPS_P2P_ANSWER:
      //DEBUG_PRINT("sent LPS_P2P_ANSWER to %02x at %04x\n", (unsigned int)txPacket.destAddress, (unsigned int)departure.low32);
      answer_tx = departure;
      break;
    case LPS_P2P_REPORT:
      //DEBUG_PRINT("sent LPS_P2P_REPORT\n");
      break;
  }
}

static uint32_t rxcallback(dwDevice_t *dev)
{
  dwTime_t arival = { .full=0 };
  dwGetReceiveTimestamp(dev, &arival);

  int dataLength = dwGetDataLength(dev);
  if (dataLength == 0) return 0;

  packet_t rxPacket;
  memset(&rxPacket, 0, MAC802154_HEADER_LENGTH);
  dwGetData(dev, (uint8_t*)&rxPacket, dataLength);

  txPacket.destAddress = rxPacket.sourceAddress;
  txPacket.sourceAddress = rxPacket.destAddress;

  DEBUG_PRINT("F %x\n", (unsigned int)txPacket.destAddress);

  switch(rxPacket.payload[LPS_P2P_TYPE]) {
    case LPS_P2P_POLL:

      curr_peer = rxPacket.sourceAddress;

      int payloadLength = 2;
      txPacket.payload[LPS_P2P_TYPE] = LPS_P2P_ANSWER;
      txPacket.payload[LPS_P2P_SEQ] = rxPacket.payload[LPS_P2P_SEQ];

      arival.full -= (ANTENNA_DELAY/2);
      poll_rx = arival;

      dwNewTransmit(dev);
      dwSetDefaults(dev);
      dwSetData(dev, (uint8_t*)&txPacket, MAC802154_HEADER_LENGTH+payloadLength);
      dwWaitForResponse(dev, true);
      dwStartTransmit(dev);
DEBUG_PRINT("POLL\n");
      break;

    // Tag received messages
    case LPS_P2P_ANSWER:
      if (rxPacket.payload[LPS_P2P_SEQ] != curr_seq) return 0;

      txPacket.payload[LPS_P2P_TYPE] = LPS_P2P_FINAL;
      txPacket.payload[LPS_P2P_SEQ] = rxPacket.payload[LPS_P2P_SEQ];

      arival.full -= (ANTENNA_DELAY / 2);
      answer_rx = arival;

      dwNewTransmit(dev);
      dwSetDefaults(dev);
      dwSetData(dev, (uint8_t*)&txPacket, MAC802154_HEADER_LENGTH+2);
      dwWaitForResponse(dev, true);
      dwStartTransmit(dev);
DEBUG_PRINT("ANS\n");
      break;
    case LPS_P2P_FINAL:
      if (curr_peer != rxPacket.sourceAddress) return 0;

      lpsp2pTagReportPayload_t *report = (lpsp2pTagReportPayload_t *)(txPacket.payload+2);

      arival.full -= (ANTENNA_DELAY/2);
      final_rx = arival;

      txPacket.payload[LPS_P2P_TYPE] = LPS_P2P_REPORT;
      txPacket.payload[LPS_P2P_SEQ] = rxPacket.payload[LPS_P2P_SEQ];
      memcpy(&report->pollRx, &poll_rx, 5);
      memcpy(&report->answerTx, &answer_tx, 5);
      memcpy(&report->finalRx, &final_rx, 5);
      report->pressure = pressure;
      report->temperature = temperature;
      report->asl = asl;
      report->pressure_ok = pressure_ok;

      dwNewTransmit(dev);
      dwSetDefaults(dev);
      dwSetData(dev, (uint8_t*)&txPacket, MAC802154_HEADER_LENGTH+2+sizeof(lpsp2pTagReportPayload_t));
      dwWaitForResponse(dev, true);
      dwStartTransmit(dev);
DEBUG_PRINT("FIN\n");
      cntout=15; //set a shorter delay to sent next poll
      break;
    case LPS_P2P_REPORT:
    {
      lpsp2pTagReportPayload_t *report = (lpsp2pTagReportPayload_t *)(rxPacket.payload+2);
      double tround1, treply1, treply2, tround2, tprop_ctn, tprop;

      if (rxPacket.payload[LPS_P2P_SEQ] != curr_seq) {
        return 0;
      }

      memcpy(&poll_rx, &report->pollRx, 5);
      memcpy(&answer_tx, &report->answerTx, 5);
      memcpy(&final_rx, &report->finalRx, 5);

      tround1 = answer_rx.low32 - poll_tx.low32;
      treply1 = answer_tx.low32 - poll_rx.low32;
      tround2 = final_rx.low32 - answer_tx.low32;
      treply2 = final_tx.low32 - answer_rx.low32;

      tprop_ctn = ((tround1*tround2) - (treply1*treply2)) / (tround1 + tround2 + treply1 + treply2);

      tprop = tprop_ctn / LOCODECK_TS_FREQ;
DEBUG_PRINT("d=%d\n",(int)(100*SPEED_OF_LIGHT * tprop));
      /*
      state.distance[current_anchor] = SPEED_OF_LIGHT * tprop;
      state.pressures[current_anchor] = report->asl;
      // Outliers rejection
      rangingStats[current_anchor].ptr = (rangingStats[current_anchor].ptr + 1) % RANGING_HISTORY_LENGTH;
      float32_t mean;
      float32_t stddev;

      arm_std_f32(rangingStats[current_anchor].history, RANGING_HISTORY_LENGTH, &stddev);
      arm_mean_f32(rangingStats[current_anchor].history, RANGING_HISTORY_LENGTH, &mean);
      float32_t diff = fabsf(mean - state.distance[current_anchor]);

      rangingStats[current_anchor].history[rangingStats[current_anchor].ptr] = state.distance[current_anchor];

      rangingOk = true;

      if ((options->combinedAnchorPositionOk || options->anchorPosition[current_anchor].timestamp) &&
          (diff < (OUTLIER_TH*stddev))) {
        distanceMeasurement_t dist;
        dist.distance = state.distance[current_anchor];
        dist.x = options->anchorPosition[current_anchor].x;
        dist.y = options->anchorPosition[current_anchor].y;
        dist.z = options->anchorPosition[current_anchor].z;
        dist.stdDev = 0.25;
        estimatorEnqueueDistance(&dist);
      }

      ranging_complete = true;
      */
DEBUG_PRINT("REPT\n"); 
      break;
    }
  }
  return timeout_p2p;
}

static void initiateRanging(dwDevice_t *dev)
{
  dwIdle(dev);

  memset(&poll_tx, 0, sizeof(poll_tx));
  memset(&poll_rx, 0, sizeof(poll_rx));
  memset(&answer_tx, 0, sizeof(answer_tx));
  memset(&answer_rx, 0, sizeof(answer_rx));
  memset(&final_tx, 0, sizeof(final_tx));
  memset(&final_rx, 0, sizeof(final_rx));

  curr_seq = 0;
  curr_peer = 0;

  pressure = temperature = asl = 0;
  pressure_ok = true;

  txPacket.sourceAddress = 0xbccf000000000000 | 13;
  txPacket.destAddress = 0xbccf000000000000 | 12;
  txPacket.payload[LPS_P2P_TYPE] = LPS_P2P_POLL;
  txPacket.payload[LPS_P2P_SEQ] = ++curr_seq;

  dwNewTransmit(dev);
  dwSetDefaults(dev);
  dwSetData(dev, (uint8_t*)&txPacket, MAC802154_HEADER_LENGTH+2);

  dwWaitForResponse(dev, true);
  dwStartTransmit(dev);

  //DEBUG_PRINT("T %x\n", (unsigned int)txPacket.destAddress);
}

static uint32_t p2pDistOnEvent(dwDevice_t *dev, uwbEvent_t event)
{
  switch(event) {
    case eventPacketReceived:
      cntout=20;
      rxcallback(dev);
      break;
    case eventPacketSent:
      txcallback(dev);
      break;
    case eventReceiveTimeout:
      dwNewReceive(dev);
      dwSetDefaults(dev);
      dwStartReceive(dev);
      if(++ertcnt>cntout)
        ertcnt=0;
      else 
        break;
    case eventTimeout:  // Comes back to timeout after each ranging attempt
      initiateRanging(dev);
      break;
    case eventReceiveFailed:
      return 0;
  }

  return timeout_p2p;
}

static void p2pDistInit(dwDevice_t *dev)
{
  return;
  // Initialize the packet in the TX buffer
  memset(&txPacket, 0, sizeof(txPacket));
  MAC80215_PACKET_INIT(txPacket, MAC802154_TYPE_DATA);
  txPacket.pan = 0xbccf;

  dwSetReceiveWaitTimeout(dev, 0xffff);
  dwCommitConfiguration(dev);
}

static bool isRangingOk()
{
  return true;
}

point_t anchorPosition[2];
static bool getAnchorPosition(const uint8_t anchorId, point_t* position) {
    *position = anchorPosition[anchorId];
    return true;
}

static uint8_t getAnchorIdList(uint8_t unorderedAnchorList[], const int maxListSize) {
  return 2;
}

static uint8_t getActiveAnchorIdList(uint8_t unorderedAnchorList[], const int maxListSize) {
  return 2;
}
uwbAlgorithm_t uwbP2PDistAlgorithm = {
  .init = p2pDistInit,
  .onEvent = p2pDistOnEvent,
  .isRangingOk = isRangingOk,
  .getAnchorPosition = getAnchorPosition,
  .getAnchorIdList = getAnchorIdList,
  .getActiveAnchorIdList = getActiveAnchorIdList,
};