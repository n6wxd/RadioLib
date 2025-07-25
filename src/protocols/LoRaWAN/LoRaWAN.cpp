#include "LoRaWAN.h"
#include <string.h>
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#endif

#if !RADIOLIB_EXCLUDE_LORAWAN

LoRaWANNode::LoRaWANNode(PhysicalLayer* phy, const LoRaWANBand_t* band, uint8_t subBand) {
  this->phyLayer = phy;
  this->band = band;
  this->channels[RADIOLIB_LORAWAN_RX2] = this->band->rx2;
  this->channels[RADIOLIB_LORAWAN_RX_BC] = this->band->rx2;
  this->txPowerMax = this->band->powerMax;
  this->subBand = subBand;
  memset(this->channelPlan, 0, sizeof(this->channelPlan));
  for(int i = 0; i < RADIOLIB_LORAWAN_NUM_SUPPORTED_PACKAGES; i++) {
    this->packages[i] = RADIOLIB_LORAWAN_PACKAGE_NONE;
  }
}

#if defined(RADIOLIB_BUILD_ARDUINO)
int16_t LoRaWANNode::sendReceive(const String& strUp, uint8_t fPort, String& strDown, bool isConfirmed, LoRaWANEvent_t* eventUp, LoRaWANEvent_t* eventDown) {
  int16_t state = RADIOLIB_ERR_UNKNOWN;
  
  const char* dataUp = strUp.c_str();

  // build a temporary buffer
  // LoRaWAN downlinks can have 250 bytes at most with 1 extra byte for NULL
  size_t lenDown = 0;
  uint8_t dataDown[RADIOLIB_LORAWAN_MAX_DOWNLINK_SIZE + 1];

  state = this->sendReceive(reinterpret_cast<const uint8_t*>(dataUp), strlen(dataUp), fPort, dataDown, &lenDown, isConfirmed, eventUp, eventDown);

  if(state > RADIOLIB_ERR_NONE) {
    // add null terminator
    dataDown[lenDown] = '\0';

    // initialize Arduino String class
    strDown = String(reinterpret_cast<char*>(dataDown));
  }

  return(state);
}
#endif

int16_t LoRaWANNode::sendReceive(const char* strUp, uint8_t fPort, bool isConfirmed, LoRaWANEvent_t* eventUp, LoRaWANEvent_t* eventDown) {
  // build a temporary buffer
  // LoRaWAN downlinks can have 250 bytes at most with 1 extra byte for NULL
  size_t lenDown = 0;
  uint8_t dataDown[RADIOLIB_LORAWAN_MAX_DOWNLINK_SIZE + 1];
  
  return(this->sendReceive(reinterpret_cast<uint8_t*>(const_cast<char*>(strUp)), strlen(strUp), fPort, dataDown, &lenDown, isConfirmed, eventUp, eventDown));
}

int16_t LoRaWANNode::sendReceive(const char* strUp, uint8_t fPort, uint8_t* dataDown, size_t* lenDown, bool isConfirmed, LoRaWANEvent_t* eventUp, LoRaWANEvent_t* eventDown) {
  return(this->sendReceive(reinterpret_cast<uint8_t*>(const_cast<char*>(strUp)), strlen(strUp), fPort, dataDown, lenDown, isConfirmed, eventUp, eventDown));
}

int16_t LoRaWANNode::sendReceive(const uint8_t* dataUp, size_t lenUp, uint8_t fPort, bool isConfirmed, LoRaWANEvent_t* eventUp, LoRaWANEvent_t* eventDown) {
  // build a temporary buffer
  // LoRaWAN downlinks can have 250 bytes at most with 1 extra byte for NULL
  size_t lenDown = 0;
  uint8_t dataDown[RADIOLIB_LORAWAN_MAX_DOWNLINK_SIZE + 1];

  return(this->sendReceive(dataUp, lenUp, fPort, dataDown, &lenDown, isConfirmed, eventUp, eventDown));
}

int16_t LoRaWANNode::sendReceive(const uint8_t* dataUp, size_t lenUp, uint8_t fPort, uint8_t* dataDown, size_t* lenDown, bool isConfirmed, LoRaWANEvent_t* eventUp, LoRaWANEvent_t* eventDown) {
  if((lenUp > 0 && !dataUp) || !dataDown || !lenDown) {
    return(RADIOLIB_ERR_NULL_POINTER);
  }
  int16_t state = RADIOLIB_ERR_UNKNOWN;
  
  // if after (at) ADR_ACK_LIMIT frames no RekeyConf was received, revert to Join state
  if(this->fCntUp == (1UL << this->adrLimitExp)) {
    state = this->getMacPayload(RADIOLIB_LORAWAN_MAC_REKEY, this->fOptsUp, this->fOptsUpLen, NULL, RADIOLIB_LORAWAN_UPLINK);
    if(state == RADIOLIB_ERR_NONE) {
      this->clearSession();
    }
  }

  // if not joined, don't do anything
  if(!this->isActivated()) {
    return(RADIOLIB_ERR_NETWORK_NOT_JOINED);
  }

  if(lenUp == 0 && fPort == 0) {
    this->isMACPayload = true;
  }

  // check if the requested payload + fPort are allowed, also given dutycycle
  state = this->isValidUplink(lenUp + this->fOptsUpLen, fPort);
  RADIOLIB_ASSERT(state);

  // the first 16 bytes are reserved for MIC calculation blocks
  size_t uplinkMsgLen = RADIOLIB_LORAWAN_FRAME_LEN(lenUp, this->fOptsUpLen);
  #if RADIOLIB_STATIC_ONLY
  uint8_t uplinkMsg[RADIOLIB_STATIC_ARRAY_SIZE];
  uint8_t frmPayload[RADIOLIB_STATIC_ARRAY_SIZE];
  #else
  uint8_t* uplinkMsg = new uint8_t[uplinkMsgLen];
  uint8_t* frmPayload = new uint8_t[lenUp + this->fOptsUpLen];
  #endif

  uint8_t frmLen = 0;

  // if the payload consists of piggybacked MAC only, move this to the FRMPayload
  if(this->isMACPayload && lenUp == 0) {
    memcpy(frmPayload, this->fOptsUp, this->fOptsUpLen);
    frmLen = this->fOptsUpLen;

    memset(this->fOptsUp, 0, RADIOLIB_LORAWAN_FHDR_FOPTS_MAX_LEN);
    this->fOptsUpLen = 0;

    this->isMACPayload = false; // reset for next uplink

  // if there is user payload, move this to the FRMPayload
  } else {
    memcpy(frmPayload, dataUp, lenUp);
    frmLen = lenUp;
  }
  
  // build the encrypted uplink message
  this->composeUplink(frmPayload, frmLen, uplinkMsg, fPort, isConfirmed);

  // reset Time-on-Air as we are starting new uplink sequence
  this->lastToA = 0;

  // repeat uplink+downlink up to 'nbTrans' times (ADR)
  uint8_t trans = 0;
  for(; trans < this->nbTrans; trans++) {

    // keep track of number of hopped channels
    uint8_t numHops = this->maxChanges;

    // number of additional CAD tries
    uint8_t numBackoff = 0;
    if(this->backoffMax) {
      numBackoff = this->phyLayer->random(1, this->backoffMax + 1);
    }

    do {
      // select a pair of Tx/Rx channels for uplink+downlink
      this->selectChannels();

      // generate and set uplink MIC (depends on selected channel)
      this->micUplink(uplinkMsg, uplinkMsgLen);

    // if CSMA is enabled, repeat channel selection & encryption up to numHops times
    } while(this->csmaEnabled && numHops-- > 0 && !this->csmaChannelClear(this->difsSlots, numBackoff));
    
    // send it (without the MIC calculation blocks)
    state = this->transmitUplink(&this->channels[RADIOLIB_LORAWAN_UPLINK],
                                &uplinkMsg[RADIOLIB_LORAWAN_FHDR_LEN_START_OFFS], 
                                (uint8_t)(uplinkMsgLen - RADIOLIB_LORAWAN_FHDR_LEN_START_OFFS),
                                trans > 0);
    if(state != RADIOLIB_ERR_NONE) {
      // sometimes, a spurious error can occur even though the uplink was transmitted
      // therefore, just to be safe, increase frame counter by one for the next uplink
      this->fCntUp += 1;

      #if !RADIOLIB_STATIC_ONLY
      delete[] uplinkMsg;
      delete[] frmPayload;
      #endif
      return(state);
    }

    // handle Rx1 and Rx2 windows - returns window > 0 if a downlink is received
    state = this->receiveDownlink();

    // RETRANSMIT_TIMEOUT is 2s +/- 1s (RP v1.0.4)
    // must be present after any confirmed frame, so we force this here
    if(isConfirmed) {
      this->sleepDelay(this->phyLayer->random(RADIOLIB_LORAWAN_RETRANSMIT_TIMEOUT_MIN_MS, 
                                              RADIOLIB_LORAWAN_RETRANSMIT_TIMEOUT_MAX_MS));
    }

    // if an error occured or a downlink was received, stop retransmission
    if(state != RADIOLIB_ERR_NONE) {
      break;
    }
    // if no downlink was received, go on

  } // end of transmission & reception

  // note: if an error occurred, it may still be the case that a transmission occurred
  // therefore, we act as if a transmission occurred before throwing the actual error
  // this feels to be the best way to comply to spec

  // increase frame counter by one for the next uplink
  this->fCntUp += 1;

  // the downlink confirmation was acknowledged, so clear the counter value
  this->confFCntDown = RADIOLIB_LORAWAN_FCNT_NONE;

  // pass the uplink info if requested
  if(eventUp) {
    eventUp->dir = RADIOLIB_LORAWAN_UPLINK;
    eventUp->confirmed = isConfirmed;
    eventUp->confirming = (this->confFCntDown != RADIOLIB_LORAWAN_FCNT_NONE);
    eventUp->datarate = this->channels[RADIOLIB_LORAWAN_UPLINK].dr;
    eventUp->freq = this->channels[RADIOLIB_LORAWAN_UPLINK].freq / 10000.0;
    eventUp->power = this->txPowerMax - this->txPowerSteps * 2;
    eventUp->fCnt = this->fCntUp;
    eventUp->fPort = fPort;
    eventUp->nbTrans = trans;
    eventUp->multicast = false;
  }

  #if !RADIOLIB_STATIC_ONLY
    delete[] uplinkMsg;
    delete[] frmPayload;
  #endif

  // if a hardware error occurred, return
  if(state < RADIOLIB_ERR_NONE) {
    return(state);
  }

  uint8_t rxWindow = state;

  // if no downlink was received, do an early exit
  if(rxWindow == 0) {
    // check if ADR backoff must occur
    if(this->adrEnabled) {
      this->adrBackoff();
    }
    // remove only non-persistent MAC commands, the other commands should be re-sent until downlink is received
    LoRaWANNode::clearMacCommands(this->fOptsUp, &this->fOptsUpLen, RADIOLIB_LORAWAN_UPLINK);
    return(rxWindow);
  }
  
  state = this->parseDownlink(dataDown, lenDown, rxWindow, eventDown);
  RADIOLIB_ASSERT(state);

  // if in Class C, open up RxC window
  if(this->lwClass == RADIOLIB_LORAWAN_CLASS_C) {
    this->receiveClassC();
  }
  
  // return Rx window (which is > 0)
  return(rxWindow);
}

void LoRaWANNode::clearNonces() {
  // clear & set all the device credentials
  memset(this->bufferNonces, 0, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
  this->keyCheckSum = 0;
  this->devNonce = 0;
  this->joinNonce = 0;
  this->isActive = false;
  this->rev = 0;
}

uint8_t* LoRaWANNode::getBufferNonces() {
  // set the device credentials
  LoRaWANNode::hton<uint16_t>(&this->bufferNonces[RADIOLIB_LORAWAN_NONCES_VERSION], RADIOLIB_LORAWAN_NONCES_VERSION_VAL);
  LoRaWANNode::hton<uint16_t>(&this->bufferNonces[RADIOLIB_LORAWAN_NONCES_MODE], this->lwMode);
  LoRaWANNode::hton<uint8_t>(&this->bufferNonces[RADIOLIB_LORAWAN_NONCES_PLAN], this->band->bandNum);
  LoRaWANNode::hton<uint16_t>(&this->bufferNonces[RADIOLIB_LORAWAN_NONCES_CHECKSUM], this->keyCheckSum);

  // generate the signature of the Nonces buffer, and store it in the last two bytes of the Nonces buffer
  uint16_t signature = LoRaWANNode::checkSum16(this->bufferNonces, RADIOLIB_LORAWAN_NONCES_BUF_SIZE - 2);
  LoRaWANNode::hton<uint16_t>(&this->bufferNonces[RADIOLIB_LORAWAN_NONCES_SIGNATURE], signature);

  return(this->bufferNonces);
}

int16_t LoRaWANNode::setBufferNonces(const uint8_t* persistentBuffer) {
  if(this->isActivated()) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Did not update buffer: session already active");
    return(RADIOLIB_ERR_NONE);
  }

  int16_t state = LoRaWANNode::checkBufferCommon(persistentBuffer, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
  RADIOLIB_ASSERT(state);

  bool isSameKeys = LoRaWANNode::ntoh<uint16_t>(&persistentBuffer[RADIOLIB_LORAWAN_NONCES_CHECKSUM]) == this->keyCheckSum;
  bool isSameMode = LoRaWANNode::ntoh<uint16_t>(&persistentBuffer[RADIOLIB_LORAWAN_NONCES_MODE]) == this->lwMode;
  bool isSamePlan  = LoRaWANNode::ntoh<uint8_t>(&persistentBuffer[RADIOLIB_LORAWAN_NONCES_PLAN]) == this->band->bandNum;

  // check if Nonces buffer matches the current configuration
  if(!isSameKeys || !isSameMode || !isSamePlan) {
    // if configuration did not match, discard whatever is currently in the buffers and start fresh
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Configuration mismatch (keys: %d, mode: %d, plan: %d)", isSameKeys, isSameMode, isSamePlan);
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Discarding the Nonces buffer:");
    RADIOLIB_DEBUG_PROTOCOL_HEXDUMP(persistentBuffer, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
    return(RADIOLIB_ERR_NONCES_DISCARDED);
  }

  // copy the whole buffer over
  memcpy(this->bufferNonces, persistentBuffer, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);

  this->devNonce  = LoRaWANNode::ntoh<uint16_t>(&this->bufferNonces[RADIOLIB_LORAWAN_NONCES_DEV_NONCE]);
  this->joinNonce = LoRaWANNode::ntoh<uint32_t>(&this->bufferNonces[RADIOLIB_LORAWAN_NONCES_JOIN_NONCE], 3);

  // revert to inactive as long as no session is restored
  this->bufferNonces[RADIOLIB_LORAWAN_NONCES_ACTIVE] = (uint8_t)false;
  this->isActive = false;

  return(state);
}

void LoRaWANNode::clearSession() {
  memset(this->bufferSession, 0, RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
  memset(this->fOptsUp, 0, RADIOLIB_LORAWAN_FHDR_FOPTS_MAX_LEN);
  memset(this->fOptsDown, 0, RADIOLIB_LORAWAN_FHDR_FOPTS_MAX_LEN);
  this->bufferNonces[RADIOLIB_LORAWAN_NONCES_ACTIVE] = (uint8_t)false;
  this->isActive = false;

  // reset all frame counters
  this->fCntUp = 0;
  this->aFCntDown = 0;
  this->nFCntDown = 0;
  this->confFCntUp = RADIOLIB_LORAWAN_FCNT_NONE;
  this->confFCntDown = RADIOLIB_LORAWAN_FCNT_NONE;
  this->adrFCnt = 0;

  // reset ADR state
  this->txPowerSteps = 0;
  this->nbTrans = 1;

  // clear CSMA settings
  this->csmaEnabled = false;
  this->maxChanges = 0;
  this->difsSlots = 0;
  this->backoffMax = 0;

  // revert to default Class A
  this->lwClass = RADIOLIB_LORAWAN_CLASS_A;
}

void LoRaWANNode::createSession(uint16_t lwMode, uint8_t initialDr) {
  this->clearSession();

  // setup the default channels
  this->addDefaultChannels();

  uint8_t drUp = RADIOLIB_LORAWAN_DATA_RATE_UNUSED;

  // on fixed bands, the first OTAA uplink (JoinRequest) is sent on fixed datarate
  if(this->band->bandType == RADIOLIB_LORAWAN_BAND_FIXED && lwMode == RADIOLIB_LORAWAN_MODE_OTAA) {
    // randomly select one of 8 or 9 channels and find corresponding datarate
    uint8_t numChannels = this->band->numTxSpans == 1 ? 8 : 9;
    uint8_t rand = this->phyLayer->random(numChannels) + 1;     // range 1-8 or 1-9
    if(rand <= 8) {
      drUp = this->band->txSpans[0].drJoinRequest;        // if one of the first 8 channels, select datarate of span 0
    } else {
      drUp = this->band->txSpans[1].drJoinRequest;        // if ninth channel, select datarate of span 1
    }
  } else {
    // on dynamic bands, the first OTAA uplink (JoinRequest) can be any available datarate
    // this is also true for ABP on both dynamic and fixed bands, as there is no JoinRequest
    if(initialDr != RADIOLIB_LORAWAN_DATA_RATE_UNUSED) {
      uint8_t i = 0; 
      for(; i < RADIOLIB_LORAWAN_NUM_AVAILABLE_CHANNELS; i++) {
        if(this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].enabled) {
          if(initialDr >= this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].drMin
            && initialDr <= this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].drMax) {
              drUp = initialDr;
              break;
          }
        }
      }
      // if there is no channel that allowed the user-specified datarate, revert to default datarate
      if(i == RADIOLIB_LORAWAN_NUM_AVAILABLE_CHANNELS) {
        RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Datarate %d is not valid - using default", initialDr);
        initialDr = RADIOLIB_LORAWAN_DATA_RATE_UNUSED;
      }
    }
  
    // if there is no (channel that allowed the) user-specified datarate, use a default datarate
    if(initialDr == RADIOLIB_LORAWAN_DATA_RATE_UNUSED) {
      // use the specified datarate from the first channel (this is always defined)
      drUp = this->channelPlan[RADIOLIB_LORAWAN_UPLINK][0].dr;
    }
  }

  uint8_t cOcts[5];                       // 5 = maximum downlink payload length
  uint8_t cid = RADIOLIB_LORAWAN_MAC_LINK_ADR;
  uint8_t cLen = 1;                       // only apply Dr/Tx field
  cOcts[0]  = (drUp << 4);                // set uplink datarate
  cOcts[0] |= 0;                          // default to max Tx Power
  (void)execMacCommand(cid, cOcts, cLen);

  cid = RADIOLIB_LORAWAN_MAC_DUTY_CYCLE;
  this->getMacLen(cid, &cLen, RADIOLIB_LORAWAN_DOWNLINK);
  uint8_t maxDCyclePower = 0;
  switch(this->band->dutyCycle) {
    case(3600):
      maxDCyclePower = 10;
      break;
    case(36000):
      maxDCyclePower = 7;
      break;
  }
  cOcts[0]  = maxDCyclePower;
  (void)execMacCommand(cid, cOcts, cLen);

  cid = RADIOLIB_LORAWAN_MAC_RX_PARAM_SETUP;
  (void)this->getMacLen(cid, &cLen, RADIOLIB_LORAWAN_DOWNLINK);
  cOcts[0]  = (RADIOLIB_LORAWAN_RX1_DR_OFFSET << 4);
  cOcts[0] |= this->channels[RADIOLIB_LORAWAN_RX2].dr; // may be set by user, otherwise band's default upon initialization
  LoRaWANNode::hton<uint32_t>(&cOcts[1], this->channels[RADIOLIB_LORAWAN_RX2].freq, 3);
  (void)execMacCommand(cid, cOcts, cLen);

  cid = RADIOLIB_LORAWAN_MAC_RX_TIMING_SETUP;
  (void)this->getMacLen(cid, &cLen, RADIOLIB_LORAWAN_DOWNLINK);
  cOcts[0]  = (RADIOLIB_LORAWAN_RECEIVE_DELAY_1_MS / 1000);
  (void)execMacCommand(cid, cOcts, cLen);

  cid = RADIOLIB_LORAWAN_MAC_TX_PARAM_SETUP;
  (void)this->getMacLen(cid, &cLen, RADIOLIB_LORAWAN_DOWNLINK);
  cOcts[0]  = (this->band->dwellTimeDn > 0 ? 1 : 0) << 5;
  cOcts[0] |= (this->band->dwellTimeUp > 0 ? 1 : 0) << 4;
  uint8_t maxEIRPRaw;
  switch(this->band->powerMax) {
    case(12):
      maxEIRPRaw = 2;
      break;
    case(14):
      maxEIRPRaw = 4;
      break;
    case(16):
      maxEIRPRaw = 5;
      break;
    case(19):   // this option does not exist for the TxParamSetupReq but will be caught during execution
      maxEIRPRaw = 7;
      break;
    case(30):
      maxEIRPRaw = 13;
      break;
    default:
      maxEIRPRaw = 2;
      break;
  }
  cOcts[0] |= maxEIRPRaw;
  (void)execMacCommand(cid, cOcts, cLen);

  cid = RADIOLIB_LORAWAN_MAC_ADR_PARAM_SETUP;
  (void)this->getMacLen(cid, &cLen, RADIOLIB_LORAWAN_DOWNLINK);
  cOcts[0]  = (RADIOLIB_LORAWAN_ADR_ACK_LIMIT_EXP << 4);
  cOcts[0] |= RADIOLIB_LORAWAN_ADR_ACK_DELAY_EXP;
  (void)execMacCommand(cid, cOcts, cLen);

  cid = RADIOLIB_LORAWAN_MAC_REJOIN_PARAM_SETUP;
  (void)this->getMacLen(cid, &cLen, RADIOLIB_LORAWAN_DOWNLINK);
  cOcts[0]  = (RADIOLIB_LORAWAN_REJOIN_MAX_TIME_N << 4);
  cOcts[0] |= RADIOLIB_LORAWAN_REJOIN_MAX_COUNT_N;
  (void)execMacCommand(cid, cOcts, cLen);
}

uint8_t* LoRaWANNode::getBufferSession() {
  // store all frame counters
  LoRaWANNode::hton<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_A_FCNT_DOWN], this->aFCntDown);
  LoRaWANNode::hton<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_N_FCNT_DOWN], this->nFCntDown);
  LoRaWANNode::hton<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_CONF_FCNT_UP], this->confFCntUp);
  LoRaWANNode::hton<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_CONF_FCNT_DOWN], this->confFCntDown);
  LoRaWANNode::hton<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_ADR_FCNT], this->adrFCnt);
  LoRaWANNode::hton<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_FCNT_UP], this->fCntUp);
  LoRaWANNode::hton<uint8_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_CLASS], this->lwClass);

  // store the enabled channels
  uint64_t chMaskGrp0123 = 0;
  uint32_t chMaskGrp45 = 0;
  this->getChannelPlanMask(&chMaskGrp0123, &chMaskGrp45);
  LoRaWANNode::hton<uint64_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_LINK_ADR] + 1, chMaskGrp0123);
  LoRaWANNode::hton<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_LINK_ADR] + 9, chMaskGrp45);

  // store the available/unused channels
  uint16_t chMask = 0x0000;
  (void)this->getAvailableChannels(&chMask);
  LoRaWANNode::hton<uint16_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_AVAILABLE_CHANNELS], chMask);

  // store the current uplink MAC command queue
  memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_MAC_QUEUE], this->fOptsUp, RADIOLIB_LORAWAN_FHDR_FOPTS_MAX_LEN);
  memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_MAC_QUEUE_LEN], &this->fOptsUpLen, 1);

  // generate the signature of the Session buffer, and store it in the last two bytes of the Session buffer
  uint16_t signature = LoRaWANNode::checkSum16(this->bufferSession, RADIOLIB_LORAWAN_SESSION_BUF_SIZE - 2);
  LoRaWANNode::hton<uint16_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_SIGNATURE], signature);
  
  return(this->bufferSession);
}

int16_t LoRaWANNode::setBufferSession(const uint8_t* persistentBuffer) {
  if(this->isActivated()) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Did not update buffer: session already active");
    return(RADIOLIB_ERR_NONE);
  }

  int16_t state = LoRaWANNode::checkBufferCommon(persistentBuffer, RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
  RADIOLIB_ASSERT(state);

  // the Nonces buffer holds a checksum signature - compare this to the signature that is in the session buffer
  uint16_t signatureNonces = LoRaWANNode::ntoh<uint16_t>(&this->bufferNonces[RADIOLIB_LORAWAN_NONCES_SIGNATURE]);
  uint16_t signatureInSession = LoRaWANNode::ntoh<uint16_t>(&persistentBuffer[RADIOLIB_LORAWAN_SESSION_NONCES_SIGNATURE]);
  if(signatureNonces != signatureInSession) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("The Session buffer (%04x) does not match the Nonces buffer (%04x)",
                                    signatureInSession, signatureNonces);
    return(RADIOLIB_ERR_SESSION_DISCARDED);
  }

  // copy the whole buffer over
  memcpy(this->bufferSession, persistentBuffer, RADIOLIB_LORAWAN_SESSION_BUF_SIZE);

  //// this code can be used in case breaking chances must be caught:
  // uint8_t nvm_table_version = this->bufferNonces[RADIOLIB_LORAWAN_NONCES_VERSION];
  // if (RADIOLIB_LORAWAN_NONCES_VERSION_VAL > nvm_table_version) {
  //  // set default values for variables that are new or something
  // }

  // pull all authentication keys from persistent storage
  this->devAddr = LoRaWANNode::ntoh<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_DEV_ADDR]);
  memcpy(this->appSKey,     &this->bufferSession[RADIOLIB_LORAWAN_SESSION_APP_SKEY],      RADIOLIB_AES128_BLOCK_SIZE);
  memcpy(this->nwkSEncKey,  &this->bufferSession[RADIOLIB_LORAWAN_SESSION_NWK_SENC_KEY],  RADIOLIB_AES128_BLOCK_SIZE);
  memcpy(this->fNwkSIntKey, &this->bufferSession[RADIOLIB_LORAWAN_SESSION_FNWK_SINT_KEY], RADIOLIB_AES128_BLOCK_SIZE);
  memcpy(this->sNwkSIntKey, &this->bufferSession[RADIOLIB_LORAWAN_SESSION_SNWK_SINT_KEY], RADIOLIB_AES128_BLOCK_SIZE);

  // restore session parameters
  this->rev          = LoRaWANNode::ntoh<uint8_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_VERSION]);
  this->lwClass      = LoRaWANNode::ntoh<uint8_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_CLASS]);
  this->homeNetId    = LoRaWANNode::ntoh<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_HOMENET_ID]);
  this->aFCntDown    = LoRaWANNode::ntoh<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_A_FCNT_DOWN]);
  this->nFCntDown    = LoRaWANNode::ntoh<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_N_FCNT_DOWN]);
  this->confFCntUp   = LoRaWANNode::ntoh<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_CONF_FCNT_UP]);
  this->confFCntDown = LoRaWANNode::ntoh<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_CONF_FCNT_DOWN]);
  this->adrFCnt      = LoRaWANNode::ntoh<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_ADR_FCNT]);
  this->fCntUp       = LoRaWANNode::ntoh<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_FCNT_UP]);
  
  // restore the complete MAC state

  uint8_t cOcts[14] = { 0 }; // see Wiki dev notes for this odd size
  uint8_t cid;
  uint8_t cLen = 0;

  // setup the default channels
  this->addDefaultChannels();

  // for dynamic bands, the additional channels must be restored per-channel
  if(this->band->bandType == RADIOLIB_LORAWAN_BAND_DYNAMIC) {
    // all-zero buffer used for checking if MAC commands are set
    const uint8_t bufferZeroes[RADIOLIB_LORAWAN_MAX_MAC_COMMAND_LEN_DOWN] = { 0 };

    // restore the session channels
    const uint8_t *startChannelsUp = &this->bufferSession[RADIOLIB_LORAWAN_SESSION_UL_CHANNELS];

    cid = RADIOLIB_LORAWAN_MAC_NEW_CHANNEL;
    (void)this->getMacLen(cid, &cLen, RADIOLIB_LORAWAN_DOWNLINK);
    for(int i = 0; i < RADIOLIB_LORAWAN_NUM_AVAILABLE_CHANNELS; i++) {
      memcpy(cOcts, startChannelsUp + (i * cLen), cLen);
      if(memcmp(cOcts, bufferZeroes, cLen) != 0) { // only execute if it is not all zeroes
        (void)execMacCommand(cid, cOcts, cLen);
      }
    }

    const uint8_t *startChannelsDown = &this->bufferSession[RADIOLIB_LORAWAN_SESSION_DL_CHANNELS];

    cid = RADIOLIB_LORAWAN_MAC_DL_CHANNEL;
    (void)this->getMacLen(cid, &cLen, RADIOLIB_LORAWAN_DOWNLINK);
    for(int i = 0; i < RADIOLIB_LORAWAN_NUM_AVAILABLE_CHANNELS; i++) {
      memcpy(cOcts, startChannelsDown + (i * cLen), cLen);
      if(memcmp(cOcts, bufferZeroes, cLen) != 0) { // only execute if it is not all zeroes
        (void)execMacCommand(cid, cOcts, cLen);
      }
    }
  }

  // restore the MAC state - ADR needs special care, the rest is straight default
  cid = RADIOLIB_LORAWAN_MAC_LINK_ADR;
  cLen = 14;                            // special internal ADR command
  memcpy(cOcts, &this->bufferSession[RADIOLIB_LORAWAN_SESSION_LINK_ADR], cLen);
  (void)execMacCommand(cid, cOcts, cLen);

  const uint8_t cids[6] = {
    RADIOLIB_LORAWAN_MAC_DUTY_CYCLE,          RADIOLIB_LORAWAN_MAC_RX_PARAM_SETUP, 
    RADIOLIB_LORAWAN_MAC_RX_TIMING_SETUP,     RADIOLIB_LORAWAN_MAC_TX_PARAM_SETUP,
    RADIOLIB_LORAWAN_MAC_ADR_PARAM_SETUP,     RADIOLIB_LORAWAN_MAC_REJOIN_PARAM_SETUP
  };

  const uint16_t locs[6] = {
    RADIOLIB_LORAWAN_SESSION_DUTY_CYCLE,      RADIOLIB_LORAWAN_SESSION_RX_PARAM_SETUP,
    RADIOLIB_LORAWAN_SESSION_RX_TIMING_SETUP, RADIOLIB_LORAWAN_SESSION_TX_PARAM_SETUP,
    RADIOLIB_LORAWAN_SESSION_ADR_PARAM_SETUP, RADIOLIB_LORAWAN_SESSION_REJOIN_PARAM_SETUP
  };

  for(uint8_t i = 0; i < 6; i++) {
    (void)this->getMacLen(cids[i], &cLen, RADIOLIB_LORAWAN_DOWNLINK);
    memcpy(cOcts, &this->bufferSession[locs[i]], cLen);
    (void)execMacCommand(cids[i], cOcts, cLen);
  }

  // set the available channels
  uint16_t chMask = LoRaWANNode::ntoh<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_AVAILABLE_CHANNELS]);
  this->setAvailableChannels(chMask);

  // copy uplink MAC command queue back in place
  memcpy(this->fOptsUp, &this->bufferSession[RADIOLIB_LORAWAN_SESSION_MAC_QUEUE], RADIOLIB_LORAWAN_FHDR_FOPTS_MAX_LEN);
  memcpy(&this->fOptsUpLen, &this->bufferSession[RADIOLIB_LORAWAN_SESSION_MAC_QUEUE_LEN], 1);

  // as both the Nonces and session are restored, revert to active session
  this->bufferNonces[RADIOLIB_LORAWAN_NONCES_ACTIVE] = (uint8_t)true;

  return(state);
}

int16_t LoRaWANNode::beginOTAA(uint64_t joinEUI, uint64_t devEUI, const uint8_t* nwkKey, const uint8_t* appKey) {
  if(!appKey) {
    return(RADIOLIB_ERR_NULL_POINTER);
  }
  // clear all the device credentials in case there were any
  this->clearNonces();

  this->joinEUI = joinEUI;
  this->devEUI = devEUI;
  memcpy(this->appKey, appKey, RADIOLIB_AES128_KEY_SIZE);
  if(nwkKey) {
    this->rev = 1;
    memcpy(this->nwkKey, nwkKey, RADIOLIB_AES128_KEY_SIZE);
  }

  // generate activation key checksum
  this->keyCheckSum ^= LoRaWANNode::checkSum16(reinterpret_cast<uint8_t*>(&joinEUI), sizeof(uint64_t));
  this->keyCheckSum ^= LoRaWANNode::checkSum16(reinterpret_cast<uint8_t*>(&devEUI), sizeof(uint64_t));
  this->keyCheckSum ^= LoRaWANNode::checkSum16(appKey, RADIOLIB_AES128_KEY_SIZE);
  if(nwkKey) {
    this->keyCheckSum ^= LoRaWANNode::checkSum16(nwkKey, RADIOLIB_AES128_KEY_SIZE);
  }

  this->lwMode = RADIOLIB_LORAWAN_MODE_OTAA;

  return(RADIOLIB_ERR_NONE);
}

int16_t LoRaWANNode::beginABP(uint32_t addr, const uint8_t* fNwkSIntKey, const uint8_t* sNwkSIntKey, const uint8_t* nwkSEncKey, const uint8_t* appSKey) {
  if(!nwkSEncKey || !appSKey) {
    return(RADIOLIB_ERR_NULL_POINTER);
  }
  // clear all the device credentials in case there were any
  this->clearNonces();

  this->devAddr = addr;
  memcpy(this->appSKey, appSKey, RADIOLIB_AES128_KEY_SIZE);
  memcpy(this->nwkSEncKey, nwkSEncKey, RADIOLIB_AES128_KEY_SIZE);
  if(fNwkSIntKey && sNwkSIntKey) {
    this->rev = 1;
    memcpy(this->fNwkSIntKey, fNwkSIntKey, RADIOLIB_AES128_KEY_SIZE);
    memcpy(this->sNwkSIntKey, sNwkSIntKey, RADIOLIB_AES128_KEY_SIZE);
  } else {
    memcpy(this->fNwkSIntKey, nwkSEncKey, RADIOLIB_AES128_KEY_SIZE);
    memcpy(this->sNwkSIntKey, nwkSEncKey, RADIOLIB_AES128_KEY_SIZE);
  }

  // generate activation key checksum
  this->keyCheckSum ^= LoRaWANNode::checkSum16(reinterpret_cast<uint8_t*>(&addr), sizeof(uint32_t));
  this->keyCheckSum ^= LoRaWANNode::checkSum16(nwkSEncKey, RADIOLIB_AES128_KEY_SIZE);
  this->keyCheckSum ^= LoRaWANNode::checkSum16(appSKey, RADIOLIB_AES128_KEY_SIZE);
  if(fNwkSIntKey) { this->keyCheckSum ^= LoRaWANNode::checkSum16(fNwkSIntKey, RADIOLIB_AES128_KEY_SIZE); }
  if(sNwkSIntKey) { this->keyCheckSum ^= LoRaWANNode::checkSum16(sNwkSIntKey, RADIOLIB_AES128_KEY_SIZE); }

  this->lwMode = RADIOLIB_LORAWAN_MODE_ABP;

  if(this->rev == 1) {
    LoRaWANNode::pushMacCommand(RADIOLIB_LORAWAN_MAC_RESET, &this->rev, this->fOptsUp, &this->fOptsUpLen, RADIOLIB_LORAWAN_UPLINK);
  }

  return(RADIOLIB_ERR_NONE);
}

void LoRaWANNode::composeJoinRequest(uint8_t* out) {
  // copy devNonce currently in use
  uint16_t devNonceUsed = this->devNonce;
  
  // set the packet fields
  out[0] = RADIOLIB_LORAWAN_MHDR_MTYPE_JOIN_REQUEST | RADIOLIB_LORAWAN_MHDR_MAJOR_R1;
  LoRaWANNode::hton<uint64_t>(&out[RADIOLIB_LORAWAN_JOIN_REQUEST_JOIN_EUI_POS], this->joinEUI);
  LoRaWANNode::hton<uint64_t>(&out[RADIOLIB_LORAWAN_JOIN_REQUEST_DEV_EUI_POS], this->devEUI);
  LoRaWANNode::hton<uint16_t>(&out[RADIOLIB_LORAWAN_JOIN_REQUEST_DEV_NONCE_POS], devNonceUsed);

  // add the authentication code
  uint32_t mic = 0;
  if(this->rev == 1) {
    mic =this->generateMIC(out, RADIOLIB_LORAWAN_JOIN_REQUEST_LEN - sizeof(uint32_t), this->nwkKey);
  } else {
    mic =this->generateMIC(out, RADIOLIB_LORAWAN_JOIN_REQUEST_LEN - sizeof(uint32_t), this->appKey);
  }
  LoRaWANNode::hton<uint32_t>(&out[RADIOLIB_LORAWAN_JOIN_REQUEST_LEN - sizeof(uint32_t)], mic);
}

int16_t LoRaWANNode::processJoinAccept(LoRaWANJoinEvent_t *joinEvent) {
  int16_t state = RADIOLIB_ERR_UNKNOWN;

  // build the buffer for the reply data
  uint8_t joinAcceptMsgEnc[RADIOLIB_LORAWAN_JOIN_ACCEPT_MAX_LEN];

  // check received length
  size_t lenRx = this->phyLayer->getPacketLength(true);
  if((lenRx != RADIOLIB_LORAWAN_JOIN_ACCEPT_MAX_LEN) && (lenRx != RADIOLIB_LORAWAN_JOIN_ACCEPT_MAX_LEN - RADIOLIB_LORAWAN_JOIN_ACCEPT_CFLIST_LEN)) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("JoinAccept reply length mismatch, expected %dB got %luB", RADIOLIB_LORAWAN_JOIN_ACCEPT_MAX_LEN, (unsigned long)lenRx);
    return(RADIOLIB_ERR_DOWNLINK_MALFORMED);
  }

  // read the packet
  state = this->phyLayer->readData(joinAcceptMsgEnc, lenRx);
  // downlink frames are sent without CRC, which will raise error on SX127x
  // we can ignore that error
  if(state != RADIOLIB_ERR_LORA_HEADER_DAMAGED) {
    RADIOLIB_ASSERT(state);
  } else {
    state = RADIOLIB_ERR_NONE;
  }

  // check reply message type
  if((joinAcceptMsgEnc[0] & RADIOLIB_LORAWAN_MHDR_MTYPE_MASK) != RADIOLIB_LORAWAN_MHDR_MTYPE_JOIN_ACCEPT) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("JoinAccept reply message type invalid, expected 0x%02x got 0x%02x", RADIOLIB_LORAWAN_MHDR_MTYPE_JOIN_ACCEPT, joinAcceptMsgEnc[0]);
    return(RADIOLIB_ERR_DOWNLINK_MALFORMED);
  }

  // decrypt the join accept message
  // this is done by encrypting again in ECB mode
  // the first byte is the MAC header which is not encrypted
  uint8_t joinAcceptMsg[RADIOLIB_LORAWAN_JOIN_ACCEPT_MAX_LEN];
  joinAcceptMsg[0] = joinAcceptMsgEnc[0];
  if(this->rev == 1) {
    RadioLibAES128Instance.init(this->nwkKey);
  } else {
    RadioLibAES128Instance.init(this->appKey);
  }
  RadioLibAES128Instance.encryptECB(&joinAcceptMsgEnc[1], RADIOLIB_LORAWAN_JOIN_ACCEPT_MAX_LEN - 1, &joinAcceptMsg[1]);

  // get current joinNonce from downlink
  uint32_t joinNonceNew = LoRaWANNode::ntoh<uint32_t>(&joinAcceptMsg[RADIOLIB_LORAWAN_JOIN_ACCEPT_JOIN_NONCE_POS], 3);
  
  RADIOLIB_DEBUG_PROTOCOL_PRINTLN("JoinAccept (JoinNonce = %lu, previously %lu):", (unsigned long)joinNonceNew, (unsigned long)this->joinNonce);
  RADIOLIB_DEBUG_PROTOCOL_HEXDUMP(joinAcceptMsg, lenRx);

  if(this->rev == 1) {
    // for v1.1, the JoinNonce received must be greater than the last joinNonce heard, else error
    if((this->joinNonce > 0) && (joinNonceNew <= this->joinNonce)) {
      return(RADIOLIB_ERR_JOIN_NONCE_INVALID);
    }
  } else {
    // for v1.0.4, the JoinNonce is simply a non-repeating value (we only check the last value)
    if((this->joinNonce > 0) && (joinNonceNew == this->joinNonce)) {
      return(RADIOLIB_ERR_JOIN_NONCE_INVALID);
    }
  }
  this->joinNonce = joinNonceNew;

  this->homeNetId = LoRaWANNode::ntoh<uint32_t>(&joinAcceptMsg[RADIOLIB_LORAWAN_JOIN_ACCEPT_HOME_NET_ID_POS], 3);
  this->devAddr = LoRaWANNode::ntoh<uint32_t>(&joinAcceptMsg[RADIOLIB_LORAWAN_JOIN_ACCEPT_DEV_ADDR_POS]);

  // check LoRaWAN revision (the MIC verification depends on this)
  uint8_t dlSettings = joinAcceptMsg[RADIOLIB_LORAWAN_JOIN_ACCEPT_DL_SETTINGS_POS];
  this->rev = (dlSettings & RADIOLIB_LORAWAN_JOIN_ACCEPT_R_1_1) >> 7;
  RADIOLIB_DEBUG_PROTOCOL_PRINTLN("LoRaWAN revision: 1.%d", this->rev);

  // verify MIC
  if(this->rev == 1) {
    // 1.1 version, first we need to derive the join accept integrity key
    uint8_t keyDerivationBuff[RADIOLIB_AES128_BLOCK_SIZE] = { 0 };
    keyDerivationBuff[0] = RADIOLIB_LORAWAN_JOIN_ACCEPT_JS_INT_KEY;
    LoRaWANNode::hton<uint64_t>(&keyDerivationBuff[1], this->devEUI);
    RadioLibAES128Instance.init(this->nwkKey);
    RadioLibAES128Instance.encryptECB(keyDerivationBuff, RADIOLIB_AES128_BLOCK_SIZE, this->jSIntKey);

    // prepare the buffer for MIC calculation
    uint8_t micBuff[3*RADIOLIB_AES128_BLOCK_SIZE] = { 0 };
    micBuff[0] = RADIOLIB_LORAWAN_JOIN_REQUEST_TYPE;
    LoRaWANNode::hton<uint64_t>(&micBuff[1], this->joinEUI);
    LoRaWANNode::hton<uint16_t>(&micBuff[9], this->devNonce - 1);
    memcpy(&micBuff[11], joinAcceptMsg, lenRx);
    
    if(!verifyMIC(micBuff, lenRx + 11, this->jSIntKey)) {
      return(RADIOLIB_ERR_MIC_MISMATCH);
    }
  
  } else {
    // 1.0 version
    if(!verifyMIC(joinAcceptMsg, lenRx, this->appKey)) {
      return(RADIOLIB_ERR_MIC_MISMATCH);
    }

  }

  uint8_t cOcts[5];
  uint8_t cid = RADIOLIB_LORAWAN_MAC_RX_PARAM_SETUP;
  uint8_t cLen = 0;
  (void)this->getMacLen(cid, &cLen, RADIOLIB_LORAWAN_DOWNLINK);
  cOcts[0] = dlSettings & 0x7F;
  LoRaWANNode::hton<uint32_t>(&cOcts[1], this->channels[RADIOLIB_LORAWAN_RX2].freq, 3);
  (void)execMacCommand(cid, cOcts, cLen);

  cid = RADIOLIB_LORAWAN_MAC_RX_TIMING_SETUP;
  (void)this->getMacLen(cid, &cLen, RADIOLIB_LORAWAN_DOWNLINK);
  cOcts[0] = joinAcceptMsg[RADIOLIB_LORAWAN_JOIN_ACCEPT_RX_DELAY_POS];
  (void)execMacCommand(cid, cOcts, cLen);

  // process CFlist if present (and if CFListType matches used band type)
  if(lenRx == RADIOLIB_LORAWAN_JOIN_ACCEPT_MAX_LEN && joinAcceptMsg[RADIOLIB_LORAWAN_JOIN_ACCEPT_CFLIST_TYPE_POS] == this->band->bandType) {
    this->processCFList(&joinAcceptMsg[RADIOLIB_LORAWAN_JOIN_ACCEPT_CFLIST_POS]);
  } 
  // if no (valid) CFList was received, default or subband are already setup so don't need to do anything else

  uint8_t keyDerivationBuff[RADIOLIB_AES128_BLOCK_SIZE] = { 0 };
  LoRaWANNode::hton<uint32_t>(&keyDerivationBuff[RADIOLIB_LORAWAN_JOIN_ACCEPT_AES_JOIN_NONCE_POS], this->joinNonce, 3);

  // check protocol version (1.0 vs 1.1)
  if(this->rev == 1) {
    // 1.1 version, derive the keys
    LoRaWANNode::hton<uint64_t>(&keyDerivationBuff[RADIOLIB_LORAWAN_JOIN_ACCEPT_AES_JOIN_EUI_POS], this->joinEUI);
    LoRaWANNode::hton<uint16_t>(&keyDerivationBuff[RADIOLIB_LORAWAN_JOIN_ACCEPT_AES_DEV_NONCE_POS], this->devNonce - 1);
    keyDerivationBuff[0] = RADIOLIB_LORAWAN_JOIN_ACCEPT_APP_S_KEY;

    RadioLibAES128Instance.init(this->appKey);
    RadioLibAES128Instance.encryptECB(keyDerivationBuff, RADIOLIB_AES128_BLOCK_SIZE, this->appSKey);

    keyDerivationBuff[0] = RADIOLIB_LORAWAN_JOIN_ACCEPT_F_NWK_S_INT_KEY;
    RadioLibAES128Instance.init(this->nwkKey);
    RadioLibAES128Instance.encryptECB(keyDerivationBuff, RADIOLIB_AES128_BLOCK_SIZE, this->fNwkSIntKey);

    keyDerivationBuff[0] = RADIOLIB_LORAWAN_JOIN_ACCEPT_S_NWK_S_INT_KEY;
    RadioLibAES128Instance.init(this->nwkKey);
    RadioLibAES128Instance.encryptECB(keyDerivationBuff, RADIOLIB_AES128_BLOCK_SIZE, this->sNwkSIntKey);

    keyDerivationBuff[0] = RADIOLIB_LORAWAN_JOIN_ACCEPT_NWK_S_ENC_KEY;
    RadioLibAES128Instance.init(this->nwkKey);
    RadioLibAES128Instance.encryptECB(keyDerivationBuff, RADIOLIB_AES128_BLOCK_SIZE, this->nwkSEncKey);

  } else {
    // 1.0 version, just derive the keys
    LoRaWANNode::hton<uint32_t>(&keyDerivationBuff[RADIOLIB_LORAWAN_JOIN_ACCEPT_HOME_NET_ID_POS], this->homeNetId, 3);
    LoRaWANNode::hton<uint16_t>(&keyDerivationBuff[RADIOLIB_LORAWAN_JOIN_ACCEPT_DEV_ADDR_POS], this->devNonce - 1);
    keyDerivationBuff[0] = RADIOLIB_LORAWAN_JOIN_ACCEPT_APP_S_KEY;
    RadioLibAES128Instance.init(this->appKey);
    RadioLibAES128Instance.encryptECB(keyDerivationBuff, RADIOLIB_AES128_BLOCK_SIZE, this->appSKey);

    keyDerivationBuff[0] = RADIOLIB_LORAWAN_JOIN_ACCEPT_F_NWK_S_INT_KEY;
    RadioLibAES128Instance.init(this->appKey);
    RadioLibAES128Instance.encryptECB(keyDerivationBuff, RADIOLIB_AES128_BLOCK_SIZE, this->fNwkSIntKey);

    memcpy(this->sNwkSIntKey, this->fNwkSIntKey, RADIOLIB_AES128_KEY_SIZE);
    memcpy(this->nwkSEncKey, this->fNwkSIntKey, RADIOLIB_AES128_KEY_SIZE);
  
  }

  // for LW v1.1, send the RekeyInd MAC command
  if(this->rev == 1) {
    // enqueue the RekeyInd MAC command to be sent in the next uplink
    cid = RADIOLIB_LORAWAN_MAC_REKEY;
    this->getMacLen(cid, &cLen, RADIOLIB_LORAWAN_UPLINK);
    cOcts[0] = this->rev;
    state = LoRaWANNode::pushMacCommand(cid, cOcts, this->fOptsUp, &this->fOptsUpLen, RADIOLIB_LORAWAN_UPLINK);
    RADIOLIB_ASSERT(state);
  }

  LoRaWANNode::hton<uint32_t>(&this->bufferNonces[RADIOLIB_LORAWAN_NONCES_JOIN_NONCE], this->joinNonce, 3);

  this->bufferNonces[RADIOLIB_LORAWAN_NONCES_ACTIVE] = (uint8_t)true;

  // generate the signature of the Nonces buffer, and store it in the last two bytes of the Nonces buffer
  // also store this signature in the Session buffer to make sure these buffers match
  uint16_t signature = LoRaWANNode::checkSum16(this->bufferNonces, RADIOLIB_LORAWAN_NONCES_BUF_SIZE - 2);
  LoRaWANNode::hton<uint16_t>(&this->bufferNonces[RADIOLIB_LORAWAN_NONCES_SIGNATURE], signature);
  LoRaWANNode::hton<uint16_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_NONCES_SIGNATURE], signature);

  // store DevAddr and all keys
  LoRaWANNode::hton<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_DEV_ADDR], this->devAddr);
  memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_APP_SKEY], this->appSKey, RADIOLIB_AES128_KEY_SIZE);
  memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_NWK_SENC_KEY], this->nwkSEncKey, RADIOLIB_AES128_KEY_SIZE);
  memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_FNWK_SINT_KEY], this->fNwkSIntKey, RADIOLIB_AES128_KEY_SIZE);
  memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_SNWK_SINT_KEY], this->sNwkSIntKey, RADIOLIB_AES128_KEY_SIZE);

  // store network parameters
  LoRaWANNode::hton<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_HOMENET_ID], this->homeNetId);
  LoRaWANNode::hton<uint8_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_VERSION], this->rev);

  this->isActive = true;

  // received JoinAccept, so update JoinNonce value in event
  if(joinEvent) {
    joinEvent->joinNonce = this->joinNonce;
  }

  return(state);
}

int16_t LoRaWANNode::activateOTAA(uint8_t joinDr, LoRaWANJoinEvent_t *joinEvent) {
  // check if there is an active session
  if(this->isActivated()) {
    // already activated, don't do anything
    return(RADIOLIB_ERR_NONE);
  }
  if(this->bufferNonces[RADIOLIB_LORAWAN_NONCES_ACTIVE]) {
    // session restored but not yet activated - do so now
    this->isActive = true;
    return(RADIOLIB_LORAWAN_SESSION_RESTORED);
  }

  // starting a new session, so make sure to update event fields already
  if(joinEvent) {
    joinEvent->newSession = true;
    joinEvent->devNonce = this->devNonce;
    joinEvent->joinNonce = this->joinNonce;
  }

  // setup all MAC properties to default values
  this->createSession(RADIOLIB_LORAWAN_MODE_OTAA, joinDr);

  // build the JoinRequest message
  uint8_t joinRequestMsg[RADIOLIB_LORAWAN_JOIN_REQUEST_LEN];
  this->composeJoinRequest(joinRequestMsg);

  // select a random pair of Tx/Rx channels
  int16_t state = this->selectChannels();
  RADIOLIB_ASSERT(state);
  
  RADIOLIB_DEBUG_PROTOCOL_PRINTLN("JoinRequest (DevNonce = %d):", this->devNonce);
  RADIOLIB_DEBUG_PROTOCOL_HEXDUMP(joinRequestMsg, RADIOLIB_LORAWAN_JOIN_REQUEST_LEN);

  state = this->transmitUplink(&this->channels[RADIOLIB_LORAWAN_UPLINK], 
                                joinRequestMsg, 
                                RADIOLIB_LORAWAN_JOIN_REQUEST_LEN);
  RADIOLIB_ASSERT(state);

  // JoinRequest successfully sent, so increase & save devNonce
  this->devNonce += 1;
  LoRaWANNode::hton<uint16_t>(&this->bufferNonces[RADIOLIB_LORAWAN_NONCES_DEV_NONCE], this->devNonce);

  // configure Rx1 and Rx2 delay for JoinAccept message - these are re-configured once a valid JoinAccept is received
  this->rxDelays[1] = RADIOLIB_LORAWAN_JOIN_ACCEPT_DELAY_1_MS;
  this->rxDelays[2] = RADIOLIB_LORAWAN_JOIN_ACCEPT_DELAY_2_MS;

  // handle Rx1 and Rx2 windows - returns window > 0 if a downlink is received
  state = this->receiveDownlink();
  if(state < RADIOLIB_ERR_NONE) {
    return(state);
  } else if (state == RADIOLIB_ERR_NONE) {
    return(RADIOLIB_ERR_NO_JOIN_ACCEPT);
  }

  // process JoinAccept message
  state = this->processJoinAccept(joinEvent);
  RADIOLIB_ASSERT(state);

  return(RADIOLIB_LORAWAN_NEW_SESSION);
}

int16_t LoRaWANNode::activateABP(uint8_t initialDr) {
  // check if there is an active session
  if(this->isActivated()) {
    // already activated, don't do anything
    return(RADIOLIB_ERR_NONE);
  }
  if(this->bufferNonces[RADIOLIB_LORAWAN_NONCES_ACTIVE]) {
    // session restored but not yet activated - do so now
    this->isActive = true;
    return(RADIOLIB_LORAWAN_SESSION_RESTORED);
  }

  // setup all MAC properties to default values
  this->createSession(RADIOLIB_LORAWAN_MODE_ABP, initialDr);

  // new session all good, so set active-bit to true
  this->bufferNonces[RADIOLIB_LORAWAN_NONCES_ACTIVE] = (uint8_t)true;

  // generate the signature of the Nonces buffer, and store it in the last two bytes of the Nonces buffer
  // also store this signature in the Session buffer to make sure these buffers match
  uint16_t signature = LoRaWANNode::checkSum16(this->bufferNonces, RADIOLIB_LORAWAN_NONCES_BUF_SIZE - 2);
  LoRaWANNode::hton<uint16_t>(&this->bufferNonces[RADIOLIB_LORAWAN_NONCES_SIGNATURE], signature);
  LoRaWANNode::hton<uint16_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_NONCES_SIGNATURE], signature);

  // store DevAddr and all keys
  LoRaWANNode::hton<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_DEV_ADDR], this->devAddr);
  memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_APP_SKEY], this->appSKey, RADIOLIB_AES128_BLOCK_SIZE);
  memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_NWK_SENC_KEY], this->nwkSEncKey, RADIOLIB_AES128_BLOCK_SIZE);
  memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_FNWK_SINT_KEY], this->fNwkSIntKey, RADIOLIB_AES128_BLOCK_SIZE);
  memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_SNWK_SINT_KEY], this->sNwkSIntKey, RADIOLIB_AES128_BLOCK_SIZE);
  
  // store network parameters
  LoRaWANNode::hton<uint32_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_HOMENET_ID], this->homeNetId);
  LoRaWANNode::hton<uint8_t>(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_VERSION], this->rev);

  this->isActive = true;

  return(RADIOLIB_LORAWAN_NEW_SESSION);
}

void LoRaWANNode::processCFList(const uint8_t* cfList) {
  RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Processing CFList");
  
  uint8_t cOcts[14] = { 0 }; // see Wiki dev notes for this odd size
  uint8_t cid;
  uint8_t cLen = 0;

  if(this->band->bandType == RADIOLIB_LORAWAN_BAND_DYNAMIC) {
    // retrieve number of existing (default) channels
    size_t num = 0;
    for(int i = 0; i < RADIOLIB_LORAWAN_NUM_AVAILABLE_CHANNELS; i++) {
      if(!this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].enabled) {
        break;
      }
      num++;
    }

    cid = RADIOLIB_LORAWAN_MAC_NEW_CHANNEL;
    (void)this->getMacLen(cid, &cLen, RADIOLIB_LORAWAN_DOWNLINK);

    const uint8_t freqZero[3] = { 0 };

    // datarate range for all new channels is equal to the default channels
    cOcts[4] = (this->band->txFreqs[0].drMax << 4) | this->band->txFreqs[0].drMin;
    for(uint8_t i = 0; i < 5; i++, num++) {
      // if the frequency fields are all zero, there are no more channels in the CFList
      if(memcmp(&cfList[i*3], freqZero, 3) == 0) {
        break;
      }
      cOcts[0] = num;
      memcpy(&cOcts[1], &cfList[i*3], 3);
      (void)execMacCommand(cid, cOcts, cLen);
    }
  } else {                // RADIOLIB_LORAWAN_BAND_FIXED
    // complete channel mask received, so clear all existing channels
    for(int i = 0; i < RADIOLIB_LORAWAN_NUM_AVAILABLE_CHANNELS; i++) {
      this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i] = RADIOLIB_LORAWAN_CHANNEL_NONE;
    }

    // copy channel mask straight over to LinkAdr MAC command
    cid = RADIOLIB_LORAWAN_MAC_LINK_ADR;
    cLen = 14;                                      // special internal ADR length
    cOcts[0] = 0xFF;                                // same datarate and cOcts
    memcpy(&cOcts[1], cfList, 12);                  // copy mask
    cOcts[13] = 0;                                  // set NbTrans = 0 -> keep the same
    (void)execMacCommand(cid, cOcts, cLen);
  }

}

bool LoRaWANNode::isActivated() {
  return(this->isActive);
}

int16_t LoRaWANNode::setClass(uint8_t cls) {
  // only allow switching class once activated
  if(!this->isActivated()) {
    return(RADIOLIB_ERR_NETWORK_NOT_JOINED);
  }

  // only Class A/B/C exist
  if(cls > RADIOLIB_LORAWAN_CLASS_C) {
    return(RADIOLIB_ERR_UNSUPPORTED);
  }

  // Class B is not implemented
  if(cls == RADIOLIB_LORAWAN_CLASS_B) {
    return(RADIOLIB_ERR_UNSUPPORTED);
  }

  // for LoRaWAN v1.0.4, simply switch class
  if(this->rev == 0) {
    this->lwClass = cls;
    return(RADIOLIB_ERR_NONE);
  }

  // for LoRaWAN v1.1, queue the DeviceModeInd MAC command
  // it will only switch once DeviceModeConf is received
  uint8_t cOct = cls;
  int16_t state = LoRaWANNode::pushMacCommand(RADIOLIB_LORAWAN_MAC_DEVICE_MODE, &cOct, this->fOptsUp, &this->fOptsUpLen, RADIOLIB_LORAWAN_UPLINK);
  return(state);
}

int16_t LoRaWANNode::startMulticastSession(uint8_t cls, uint32_t mcAddr, const uint8_t* mcAppSKey, const uint8_t* mcNwkSKey, uint32_t mcFCntMin, uint32_t mcFCntMax, uint32_t mcFreq, uint8_t mcDr) {
  this->multicast = false;

  if(!this->isActivated()) {
    return(RADIOLIB_ERR_NETWORK_NOT_JOINED);
  }

  // currently only possible for Class C
  if(cls == RADIOLIB_LORAWAN_CLASS_B) {
    return(RADIOLIB_ERR_UNSUPPORTED);
  }

  if(mcAppSKey == nullptr || mcNwkSKey == nullptr) {
    return(RADIOLIB_ERR_NULL_POINTER);
  }

  // check if frequency is within band
  if(mcFreq == 0) {
    mcFreq = this->channels[RADIOLIB_LORAWAN_RX2].freq * 100;
  }
  if(mcFreq / 100 < this->band->freqMin || mcFreq / 100 > this->band->freqMax) {
    return(RADIOLIB_ERR_INVALID_FREQUENCY);
  }

  // check if datarate is defined
  if(mcDr == RADIOLIB_LORAWAN_DATA_RATE_UNUSED) {
    mcDr = this->channels[RADIOLIB_LORAWAN_RX2].dr;
  }
  if(this->band->dataRates[mcDr] == RADIOLIB_LORAWAN_DATA_RATE_UNUSED) {
    return(RADIOLIB_ERR_INVALID_DATA_RATE);
  }

  // check the frame counter range
  if(mcFCntMin >= mcFCntMax) {
    return(RADIOLIB_ERR_MULTICAST_FCNT_INVALID);
  }

  // all checks passed, so apply configuration
  this->multicast = cls;
  this->channels[RADIOLIB_LORAWAN_RX_BC].freq = mcFreq / 100;
  this->channels[RADIOLIB_LORAWAN_RX_BC].dr = mcDr;
  this->channels[RADIOLIB_LORAWAN_RX_BC].drMin = mcDr;
  this->channels[RADIOLIB_LORAWAN_RX_BC].drMax = mcDr;
  this->mcAddr = mcAddr;
  memcpy(this->mcAppSKey, mcAppSKey, RADIOLIB_AES128_KEY_SIZE);
  memcpy(this->mcNwkSKey, mcNwkSKey, RADIOLIB_AES128_KEY_SIZE);
  this->mcAFCnt = mcFCntMin;
  this->mcAFCntMax = mcFCntMax;

  return(RADIOLIB_ERR_NONE);
}

void LoRaWANNode::stopMulticastSession() {
  this->multicast = false;
}

int16_t LoRaWANNode::isValidUplink(size_t len, uint8_t fPort) {
  // check destination fPort
  bool ok = false;
  if(fPort == RADIOLIB_LORAWAN_FPORT_MAC_COMMAND && this->isMACPayload) {
    ok = true;
  }
  if(fPort >= RADIOLIB_LORAWAN_FPORT_PAYLOAD_MIN && fPort <= RADIOLIB_LORAWAN_FPORT_PAYLOAD_MAX) {
    ok = true;
  }
  if(fPort >= RADIOLIB_LORAWAN_FPORT_RESERVED) {
    for(int id = 0; id < RADIOLIB_LORAWAN_NUM_SUPPORTED_PACKAGES; id++) {
      if(this->packages[id].enabled && fPort == this->packages[id].packFPort) {
        ok = true;
        break;
      }
    }
  }

  if(!ok) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Requested uplink at FPort %d - rejected! This FPort is reserved.", fPort);
    return(RADIOLIB_ERR_INVALID_PORT);
  }

  // check maximum payload len as defined in band
  uint8_t maxPayLen = this->band->payloadLenMax[this->channels[RADIOLIB_LORAWAN_UPLINK].dr];
  if(this->packages[RADIOLIB_LORAWAN_PACKAGE_TS011].enabled) {
    maxPayLen = RADIOLIB_MIN(maxPayLen, 222); // payload length is limited to 222 if under repeater
  }

  // throw an error if the packet is too long
  if(len > maxPayLen) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("%d bytes payload exceeding limit of %d bytes", len, maxPayLen);
    return(RADIOLIB_ERR_PACKET_TOO_LONG);
  }

  return(RADIOLIB_ERR_NONE);
}

void LoRaWANNode::adrBackoff() {
  // check if we need to do ADR stuff
  uint32_t adrLimit = 0x01 << this->adrLimitExp;
  uint32_t adrDelay = 0x01 << this->adrDelayExp;

  // check if we already tried everything (adrFCnt == FCNT_NONE)
  if(this->adrFCnt == RADIOLIB_LORAWAN_FCNT_NONE) {
    return;
  }

  // no need to do any backoff for first Limit+Delay uplinks
  if((this->fCntUp - this->adrFCnt) < (adrLimit + adrDelay)) {
    return;
  }

  // only perform backoff every Delay uplinks
  if((this->fCntUp - this->adrFCnt - adrLimit) % adrDelay != 0) {
    return;
  }

  // if we hit the Limit + Delay, try one of three, in order: 
  // set TxPower to max, set DR to min, enable all default channels

  // if the TxPower field has some offset, remove it and switch to maximum power
  if(this->txPowerSteps > 0) {
    // set the maximum power supported by both the module and the band
    if(this->setTxPower(this->txPowerMax) == RADIOLIB_ERR_NONE) {
      return;
    }
  }

  // if datarate can be decreased, try it
  if(this->channels[RADIOLIB_LORAWAN_UPLINK].dr > 0) {
    uint8_t oldDr = this->channels[RADIOLIB_LORAWAN_UPLINK].dr;

    if(this->setDatarate(oldDr - 1) == RADIOLIB_ERR_NONE) {
      // if there is no dwell time limit, a lower datarate is OK
      if(!this->dwellTimeUp) {
        return;
      }
      // if there is a dwell time limit, check if this datarate allows an empty uplink
      if(this->phyLayer->getTimeOnAir(13) / 1000 <= this->dwellTimeUp) { 
        return;
      }
      // if the Time on Air of an empty uplink exceeded the dwell time, revert
      this->setDatarate(oldDr);
    }
  }

  // last resort: enable all (default) channels
  this->addDefaultChannels();

  // re-enabling default channels may have enabled channels that do support
  // the next required datarate; if datarate can be decreased, try it
  if(this->channels[RADIOLIB_LORAWAN_UPLINK].dr > 0) {
    uint8_t oldDr = this->channels[RADIOLIB_LORAWAN_UPLINK].dr;

    if(this->setDatarate(oldDr - 1) == RADIOLIB_ERR_NONE) {
      // if there is no dwell time limit, a lower datarate is OK
      if(!this->dwellTimeUp) {
        return;
      }
      // if there is a dwell time limit, check if this datarate allows an empty uplink
      if(this->phyLayer->getTimeOnAir(13) / 1000 <= this->dwellTimeUp) { 
        return;
      }
      // if the Time on Air of an empty uplink exceeded the dwell time, revert
      this->setDatarate(oldDr);
    }
  }

  this->nbTrans = 1;

  // as there is nothing more to do, set ADR counter to maximum value to indicate that we've tried everything
  this->adrFCnt = RADIOLIB_LORAWAN_FCNT_NONE;

  return;
}

void LoRaWANNode::composeUplink(const uint8_t* in, uint8_t lenIn, uint8_t* out, uint8_t fPort, bool isConfirmed) {
  // set the packet fields
  if(isConfirmed) {
    out[RADIOLIB_LORAWAN_FHDR_LEN_START_OFFS] = RADIOLIB_LORAWAN_MHDR_MTYPE_CONF_DATA_UP;
    this->confFCntUp = this->fCntUp;
  } else {
    out[RADIOLIB_LORAWAN_FHDR_LEN_START_OFFS] = RADIOLIB_LORAWAN_MHDR_MTYPE_UNCONF_DATA_UP;
  }
  out[RADIOLIB_LORAWAN_FHDR_LEN_START_OFFS] |= RADIOLIB_LORAWAN_MHDR_MAJOR_R1;
  LoRaWANNode::hton<uint32_t>(&out[RADIOLIB_LORAWAN_FHDR_DEV_ADDR_POS], this->devAddr);

  out[RADIOLIB_LORAWAN_FHDR_FCTRL_POS] = 0x00;
  if(this->adrEnabled) {
    out[RADIOLIB_LORAWAN_FHDR_FCTRL_POS] |= RADIOLIB_LORAWAN_FCTRL_ADR_ENABLED;
    
    // AdrAckReq is set if no downlink has been received for >=Limit uplinks
    uint32_t adrLimit = 0x01 << this->adrLimitExp;
    if(this->rev == 1) {
      // AdrAckReq is unset once backoff has been completed 
      // (which is internally denoted by adrFCnt == FCNT_NONE)
      if(this->adrFCnt != RADIOLIB_LORAWAN_FCNT_NONE && (this->fCntUp - this->adrFCnt) >= adrLimit) {
        out[RADIOLIB_LORAWAN_FHDR_FCTRL_POS] |= RADIOLIB_LORAWAN_FCTRL_ADR_ACK_REQ;
      }
    } else {  // rev == 0
      // AdrAckReq is always set, also when backoff has been completed
      if(this->adrFCnt == RADIOLIB_LORAWAN_FCNT_NONE || (this->fCntUp - this->adrFCnt) >= adrLimit) {
        out[RADIOLIB_LORAWAN_FHDR_FCTRL_POS] |= RADIOLIB_LORAWAN_FCTRL_ADR_ACK_REQ;
      }
    }
  }
  
  // check if we have some MAC commands to append
  out[RADIOLIB_LORAWAN_FHDR_FCTRL_POS] |= this->fOptsUpLen;

  // if the saved confirm-fCnt is set, set the ACK bit
  if(this->confFCntDown != RADIOLIB_LORAWAN_FCNT_NONE) {
    out[RADIOLIB_LORAWAN_FHDR_FCTRL_POS] |= RADIOLIB_LORAWAN_FCTRL_ACK;
  }

  LoRaWANNode::hton<uint16_t>(&out[RADIOLIB_LORAWAN_FHDR_FCNT_POS], (uint16_t)this->fCntUp);

  if(this->fOptsUpLen > 0) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Uplink MAC payload:");
    RADIOLIB_DEBUG_PROTOCOL_HEXDUMP(this->fOptsUp, this->fOptsUpLen);

    if(this->rev == 1) {
      // in LoRaWAN v1.1, the FOpts are encrypted using the NwkSEncKey
      processAES(this->fOptsUp, this->fOptsUpLen, this->nwkSEncKey, &out[RADIOLIB_LORAWAN_FHDR_FOPTS_POS], this->devAddr, this->fCntUp, RADIOLIB_LORAWAN_UPLINK, 0x01, true);
    } else {
      // in LoRaWAN v1.0.x, the FOpts are unencrypted
      memcpy(&out[RADIOLIB_LORAWAN_FHDR_FOPTS_POS], this->fOptsUp, this->fOptsUpLen);
    }
    
  }

  // set the fPort
  out[RADIOLIB_LORAWAN_FHDR_FPORT_POS(this->fOptsUpLen)] = fPort;

  // select encryption key based on the target fPort
  uint8_t* encKey = this->appSKey;
  if(fPort == RADIOLIB_LORAWAN_FPORT_MAC_COMMAND) {
    encKey = this->nwkSEncKey;
  }
  // check if any of the packages uses this FPort
  for(int id = 0; id < RADIOLIB_LORAWAN_NUM_SUPPORTED_PACKAGES; id++) {
    if(this->packages[id].enabled && fPort == this->packages[id].packFPort) {
      encKey = this->packages[id].isAppPack ? this->appSKey : this->nwkSEncKey;
      break;
    }
  }

  // encrypt the frame payload
  processAES(in, lenIn, encKey, &out[RADIOLIB_LORAWAN_FRAME_PAYLOAD_POS(this->fOptsUpLen)], this->devAddr, this->fCntUp, RADIOLIB_LORAWAN_UPLINK, 0x00, true);
}

void LoRaWANNode::micUplink(uint8_t* inOut, size_t lenInOut) {
  // create blocks for MIC calculation
  uint8_t block0[RADIOLIB_AES128_BLOCK_SIZE] = { 0 };
  block0[RADIOLIB_LORAWAN_BLOCK_MAGIC_POS] = RADIOLIB_LORAWAN_MIC_BLOCK_MAGIC;
  block0[RADIOLIB_LORAWAN_BLOCK_DIR_POS] = RADIOLIB_LORAWAN_UPLINK;
  LoRaWANNode::hton<uint32_t>(&block0[RADIOLIB_LORAWAN_BLOCK_DEV_ADDR_POS], this->devAddr);
  LoRaWANNode::hton<uint32_t>(&block0[RADIOLIB_LORAWAN_BLOCK_FCNT_POS], this->fCntUp);
  block0[RADIOLIB_LORAWAN_MIC_BLOCK_LEN_POS] = lenInOut - RADIOLIB_AES128_BLOCK_SIZE - sizeof(uint32_t);

  uint8_t block1[RADIOLIB_AES128_BLOCK_SIZE] = { 0 };
  memcpy(block1, block0, RADIOLIB_AES128_BLOCK_SIZE);
  if(this->confFCntDown != RADIOLIB_LORAWAN_FCNT_NONE) {
    LoRaWANNode::hton<uint16_t>(&block1[RADIOLIB_LORAWAN_BLOCK_CONF_FCNT_POS], (uint16_t)this->confFCntDown);
  }
  block1[RADIOLIB_LORAWAN_MIC_DATA_RATE_POS] = this->channels[RADIOLIB_LORAWAN_UPLINK].dr;
  block1[RADIOLIB_LORAWAN_MIC_CH_INDEX_POS] = this->channels[RADIOLIB_LORAWAN_UPLINK].idx;
  
  RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Uplink (FCntUp = %lu) decoded:", (unsigned long)this->fCntUp);
  RADIOLIB_DEBUG_PROTOCOL_HEXDUMP(inOut, lenInOut);

  // calculate authentication codes
  memcpy(inOut, block1, RADIOLIB_AES128_BLOCK_SIZE);
  uint32_t micS = this->generateMIC(inOut, lenInOut - sizeof(uint32_t), this->sNwkSIntKey);
  memcpy(inOut, block0, RADIOLIB_AES128_BLOCK_SIZE);
  uint32_t micF = this->generateMIC(inOut, lenInOut - sizeof(uint32_t), this->fNwkSIntKey);

  // check LoRaWAN revision
  if(this->rev == 1) {
    uint32_t mic = ((uint32_t)(micF & 0x0000FF00) << 16) | ((uint32_t)(micF & 0x0000000FF) << 16) | ((uint32_t)(micS & 0x0000FF00) >> 0) | ((uint32_t)(micS & 0x0000000FF) >> 0);
    LoRaWANNode::hton<uint32_t>(&inOut[lenInOut - sizeof(uint32_t)], mic);
  } else {
    LoRaWANNode::hton<uint32_t>(&inOut[lenInOut - sizeof(uint32_t)], micF);
  }
}

int16_t LoRaWANNode::transmitUplink(const LoRaWANChannel_t* chnl, uint8_t* in, uint8_t len, bool retrans) {
  int16_t state = RADIOLIB_ERR_UNKNOWN;
  Module* mod = this->phyLayer->getMod();

  RadioLibTime_t tNow = mod->hal->millis();
  // if scheduled uplink time is in the past, reschedule to now
  if(this->tUplink < tNow) {
    this->tUplink = tNow;
  }

  // if dutycycle is enabled and the time since last uplink + interval has not elapsed, return an error
  // but: don't check this for retransmissions
  if(!retrans && this->dutyCycleEnabled) {
    if(this->rxDelayStart + (RadioLibTime_t)dutyCycleInterval(this->dutyCycle, this->lastToA) > this->tUplink) {
      return(RADIOLIB_ERR_UPLINK_UNAVAILABLE);
    }
  }

  // set the physical layer configuration for uplink
  state = this->setPhyProperties(chnl,
                                 RADIOLIB_LORAWAN_UPLINK, 
                                 this->txPowerMax - 2*this->txPowerSteps);
  RADIOLIB_ASSERT(state);

  // check whether dwell time limitation is exceeded
  RadioLibTime_t toa = this->phyLayer->getTimeOnAir(len) / 1000;

  if(this->dwellTimeUp) {
    if(toa > this->dwellTimeUp) {
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Dwell time exceeded: ToA = %lu, max = %d", (unsigned long)toa, this->dwellTimeUp);
      return(RADIOLIB_ERR_DWELL_TIME_EXCEEDED);
    }
  }

  RadioModeConfig_t modeCfg;
  modeCfg.transmit.data = in;
  modeCfg.transmit.len = len;
  modeCfg.transmit.addr = 0;
  state = this->phyLayer->stageMode(RADIOLIB_RADIO_MODE_TX, &modeCfg);
  RADIOLIB_ASSERT(state);
  
  // if requested, wait until transmitting uplink
  tNow = mod->hal->millis();
  if(this->tUplink > tNow + this->launchDuration) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Delaying transmission by %lu ms", (unsigned long)(this->tUplink - tNow - this->launchDuration));
    tNow = mod->hal->millis();
    if(this->tUplink > tNow + this->launchDuration) {
      this->sleepDelay(this->tUplink - tNow - this->launchDuration);
    }
  }

  // start transmission, and time the duration of launchMode() to offset window timing
  RadioLibTime_t spiStart = mod->hal->millis();
  state = this->phyLayer->launchMode();
  RadioLibTime_t spiEnd = mod->hal->millis();
  this->launchDuration = spiEnd - spiStart;
  RADIOLIB_ASSERT(state);

  // sleep for the duration of the transmission
  this->sleepDelay(toa);
  RadioLibTime_t txEnd = mod->hal->millis();

  // wait for an additional transmission duration as Tx timeout period
  while(!mod->hal->digitalRead(mod->getIrq())) {
    // yield for multi-threaded platforms
    mod->hal->yield();

    if(mod->hal->millis() > txEnd + this->scanGuard) {
      return(RADIOLIB_ERR_TX_TIMEOUT);
    }
  }
  state = this->phyLayer->finishTransmit();

  // set the timestamp so that we can measure when to start receiving
  this->rxDelayStart = mod->hal->millis();
  RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Uplink sent <-- Rx Delay start");

  // increase Time on Air of the uplink sequence
  this->lastToA += toa;

  return(state);
}

// flag to indicate whether there was some action during Rx mode (timeout or downlink)
static volatile bool downlinkAction = false;

// interrupt service routine to handle downlinks automatically
#if defined(ESP8266) || defined(ESP32)
  IRAM_ATTR
#endif
static void LoRaWANNodeOnDownlinkAction(void) {
  downlinkAction = true;
}

int16_t LoRaWANNode::receiveClassA(uint8_t dir, const LoRaWANChannel_t* dlChannel, uint8_t window, const RadioLibTime_t dlDelay, RadioLibTime_t tReference) {
  Module* mod = this->phyLayer->getMod();

  int16_t state = RADIOLIB_ERR_UNKNOWN;

  // either both must be set or none
  if((dlDelay == 0 && tReference > 0) || (dlDelay > 0 && tReference == 0)) {
    return(RADIOLIB_ERR_NO_RX_WINDOW);
  }

  // set the physical layer configuration for downlink
  state = this->setPhyProperties(dlChannel, dir, this->txPowerMax - 2*this->txPowerSteps);
  RADIOLIB_ASSERT(state);

  // calculate the timeout of an empty packet plus scanGuard
  RadioLibTime_t timeoutHost = this->phyLayer->getTimeOnAir(0) + this->scanGuard*1000;

  // get the maximum allowed Time-on-Air of a packet given the current datarate
  uint8_t maxPayLen = this->band->payloadLenMax[dlChannel->dr];
  if(this->packages[RADIOLIB_LORAWAN_PACKAGE_TS011].enabled) {
    maxPayLen = RADIOLIB_MIN(maxPayLen, 222); // payload length is limited to 222 if under repeater
  }
  RadioLibTime_t tMax = this->phyLayer->getTimeOnAir(maxPayLen + 13) / 1000; // mandatory FHDR is 12/13 bytes

  // set the radio Rx parameters
  RadioModeConfig_t modeCfg;
  modeCfg.receive.irqFlags = RADIOLIB_IRQ_RX_DEFAULT_FLAGS;
  modeCfg.receive.irqMask = RADIOLIB_IRQ_RX_DEFAULT_MASK;
  modeCfg.receive.len = 0;
  modeCfg.receive.timeout = this->phyLayer->calculateRxTimeout(timeoutHost);

  state = this->phyLayer->stageMode(RADIOLIB_RADIO_MODE_RX, &modeCfg);
  RADIOLIB_ASSERT(state);

  // setup interrupt
  this->phyLayer->setPacketReceivedAction(LoRaWANNodeOnDownlinkAction);
  downlinkAction = false;

  // if the Rx window must be awaited, do so
  RadioLibTime_t tNow = mod->hal->millis();
  if(dlDelay > 0 && tReference > 0) {
    // calculate time at which the window should open
    // - the launch of Rx window takes a few milliseconds, so shorten the waitLen a bit (launchDuration)
    // - the Rx window is padded using scanGuard, so shorten the waitLen a bit (scanGuard / 2)
    RadioLibTime_t tWindow = tReference + dlDelay - this->launchDuration - this->scanGuard / 2;
    if(tNow > tWindow) {
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Window too late by %d ms", tNow - tWindow);
      return(RADIOLIB_ERR_NO_RX_WINDOW);
    }
    this->sleepDelay(tWindow - tNow);
  }

  // open Rx window by starting receive with specified timeout
  state = this->phyLayer->launchMode();
  RadioLibTime_t tOpen = mod->hal->millis();
  RADIOLIB_ASSERT(state);
  RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Opened Rx%d window (%d ms timeout)... <-- Rx Delay end ", window, (int)(timeoutHost / 1000 + 2));
  
  // sleep for the duration of the padded Rx window
  this->sleepDelay(timeoutHost / 1000);
  
  // wait for the DIO interrupt to fire (RxDone or RxTimeout)
  // use a small additional delay in case the RxTimeout interrupt is slow to fire
  RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Closing Rx%d window", window);
  while(!downlinkAction && mod->hal->millis() - tOpen <= timeoutHost / 1000 + this->scanGuard) {
    mod->hal->yield();
  }

  // check IRQ bit for RxTimeout
  int16_t timedOut = this->phyLayer->checkIrq(RADIOLIB_IRQ_TIMEOUT);
  if(timedOut == RADIOLIB_ERR_UNSUPPORTED) {
    return(timedOut);
  }

  // if the IRQ bit for RxTimeout is set, put chip in standby and return
  if(timedOut) {
    this->phyLayer->clearPacketReceivedAction();
    this->phyLayer->standby();
    return(0);  // no downlink
  }
  
  // if the IRQ bit for RxTimeout is not set, something is being received, 
  // so keep listening for maximum ToA waiting for the DIO to fire
  while(!downlinkAction && mod->hal->millis() - tOpen < tMax + this->scanGuard) {
    mod->hal->yield();
  }

  // update time of downlink reception
  if(downlinkAction) {
    this->tDownlink = mod->hal->millis();
  }

  // we have a message, clear actions, go to standby
  this->phyLayer->clearPacketReceivedAction();
  this->phyLayer->standby();

  // if all windows passed without receiving anything, return 0 for no window
  if(!downlinkAction) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Downlink missing!");
    return(0);
  }
  downlinkAction = false;

  // Any frame received by an end-device containing a MACPayload greater than 
  // the specified maximum length M over the data rate used to receive the frame 
  // SHALL be silently discarded.
  if(this->phyLayer->getPacketLength() > (size_t)(maxPayLen + 13)) {  // mandatory FHDR is 12/13 bytes
    return(0);  // act as if no downlink was received
  }

  // return downlink window number (1/2)
  return(window);
}

int16_t LoRaWANNode::receiveClassC(RadioLibTime_t timeout) {
  // only open RxC if the device is Unicast-C or Multicast-C, otherwise ignore without error
  if(this->lwClass != RADIOLIB_LORAWAN_CLASS_C && this->multicast != RADIOLIB_LORAWAN_CLASS_C) {
    return(RADIOLIB_ERR_NONE);
  }
  Module* mod = this->phyLayer->getMod();
  
  RadioLibTime_t tStart = mod->hal->millis();

  // set the physical layer configuration for Class C window
  int16_t state = this->setPhyProperties(&this->channels[RADIOLIB_LORAWAN_RX_BC], RADIOLIB_LORAWAN_DOWNLINK, this->txPowerMax - 2*this->txPowerSteps);
  RADIOLIB_ASSERT(state);

  // setup interrupt
  this->phyLayer->setPacketReceivedAction(LoRaWANNodeOnDownlinkAction);
  downlinkAction = false;
  
  // configure radio
  RadioModeConfig_t modeCfg;
  if(timeout) {
    timeout -= (mod->hal->millis() - tStart);
    timeout -= this->launchDuration;
    modeCfg.receive.timeout = this->phyLayer->calculateRxTimeout(timeout * 1000);
  } else {
    modeCfg.receive.timeout = 0xFFFFFFFF; // max(uint32_t) is used for RxContinuous
  }
  modeCfg.receive.irqFlags = RADIOLIB_IRQ_RX_DEFAULT_FLAGS;
  modeCfg.receive.irqMask = RADIOLIB_IRQ_RX_DEFAULT_MASK;
  modeCfg.receive.len = 0;
  state = this->phyLayer->stageMode(RADIOLIB_RADIO_MODE_RX, &modeCfg);
  RADIOLIB_ASSERT(state);

  // open RxC window by starting receive with specified timeout
  state = this->phyLayer->launchMode();
  RadioLibTime_t tOpen = mod->hal->millis();
  RADIOLIB_ASSERT(state);
  RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Opened RxC window");

  if(timeout) {
    // wait for the DIO interrupt to fire (RxDone or RxTimeout)
    // use a small additional delay in case the RxTimeout interrupt is slow to fire
    while(!downlinkAction && mod->hal->millis() - tOpen <= timeout + this->scanGuard) {
      mod->hal->yield();
    }
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Closed RxC window");

    // check IRQ bit for RxTimeout
    int16_t timedOut = this->phyLayer->checkIrq(RADIOLIB_IRQ_TIMEOUT);
    if(timedOut == RADIOLIB_ERR_UNSUPPORTED) {
      return(timedOut);
    }

    // if the IRQ bit for RxTimeout is set, put chip in standby and return
    if(timedOut) {
      this->phyLayer->clearPacketReceivedAction();
      this->phyLayer->standby();
      return(0);  // no downlink
    }

    // update time of downlink reception
    if(downlinkAction) {
      this->tDownlink = mod->hal->millis();
    }

    // we have a message, clear actions, go to standby
    this->phyLayer->clearPacketReceivedAction();
    this->phyLayer->standby();

    // if all windows passed without receiving anything, return 0 for no window
    if(!downlinkAction) {
      return(0);
    }
    downlinkAction = false;

    // Any frame received by an end-device containing a MACPayload greater than 
    // the specified maximum length M over the data rate used to receive the frame 
    // SHALL be silently discarded.
    uint8_t maxPayLen = this->band->payloadLenMax[this->channels[RADIOLIB_LORAWAN_RX_BC].dr];
    if(this->packages[RADIOLIB_LORAWAN_PACKAGE_TS011].enabled) {
      maxPayLen = RADIOLIB_MIN(maxPayLen, 222); // payload length is limited to 222 if under repeater
    }
    if(this->phyLayer->getPacketLength() > (size_t)(maxPayLen + 13)) {  // mandatory FHDR is 12/13 bytes
      return(0);  // act as if no downlink was received
    }

    // return downlink window number (3 = RxC)
    return(RADIOLIB_LORAWAN_RX_BC);
  }

  return(state);
}

int16_t LoRaWANNode::receiveDownlink() {
  Module* mod = this->phyLayer->getMod();

  // if applicable, open Class C between uplink and Rx1
  RadioLibTime_t timeoutClassC = this->rxDelayStart + this->rxDelays[RADIOLIB_LORAWAN_RX1] - \
                                  mod->hal->millis() - 5*this->scanGuard;
  int16_t state = this->receiveClassC(timeoutClassC);
  RADIOLIB_ASSERT(state);

  // open Rx1 window
  state = this->receiveClassA(RADIOLIB_LORAWAN_DOWNLINK, 
                              &this->channels[RADIOLIB_LORAWAN_RX1], 
                              RADIOLIB_LORAWAN_RX1, 
                              this->rxDelays[RADIOLIB_LORAWAN_RX1], 
                              this->rxDelayStart);
  RADIOLIB_ASSERT(state);
  
  // for LoRaWAN v1.1 Class C, there is no Rx2 window: it keeps RxC open uninterrupted
  if(this->lwClass == RADIOLIB_LORAWAN_CLASS_C && this->rev == 1) {
    state = this->receiveClassC();
    return(state);
  }

  // for LoRaWAN v1.0.4 Class C, there is an RxC window between Rx1 and Rx2
  timeoutClassC = this->rxDelayStart + this->rxDelays[RADIOLIB_LORAWAN_RX2] - \
                                  mod->hal->millis() - 5*this->scanGuard;
  state = this->receiveClassC(timeoutClassC);
  RADIOLIB_ASSERT(state);

  // open Rx2 window
  state = this->receiveClassA(RADIOLIB_LORAWAN_DOWNLINK, 
                              &this->channels[RADIOLIB_LORAWAN_RX2], 
                              RADIOLIB_LORAWAN_RX2, 
                              this->rxDelays[RADIOLIB_LORAWAN_RX2], 
                              this->rxDelayStart);
  RADIOLIB_ASSERT(state);

  state = this->receiveClassC();
  return(state);
}

int16_t LoRaWANNode::parseDownlink(uint8_t* data, size_t* len, uint8_t window, LoRaWANEvent_t* event) {
  int16_t state = RADIOLIB_ERR_UNKNOWN;
  
  // set user-data length to 0 to prevent undefined behaviour in case of bad use
  // if there is user-data, this will be handled at the appropriate place
  *len = 0;

  // get the packet length
  size_t downlinkMsgLen = this->phyLayer->getPacketLength();

  // check the minimum required frame length
  // an extra byte is subtracted because downlink frames may not have a fPort
  if(downlinkMsgLen < RADIOLIB_LORAWAN_FRAME_LEN(0, 0) - 1 - RADIOLIB_AES128_BLOCK_SIZE) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Downlink message too short (%lu bytes)", (unsigned long)downlinkMsgLen);
    return(RADIOLIB_ERR_DOWNLINK_MALFORMED);
  }

  // build the buffer for the downlink message
  // the first 16 bytes are reserved for MIC calculation block
  #if !RADIOLIB_STATIC_ONLY
    uint8_t* downlinkMsg = new uint8_t[RADIOLIB_AES128_BLOCK_SIZE + downlinkMsgLen];
  #else
    uint8_t downlinkMsg[RADIOLIB_STATIC_ARRAY_SIZE];
  #endif

  // read the data
  state = this->phyLayer->readData(&downlinkMsg[RADIOLIB_AES128_BLOCK_SIZE], downlinkMsgLen);
  // downlink frames are sent without CRC, which will raise error on SX127x
  // we can ignore that error
  if(state == RADIOLIB_ERR_LORA_HEADER_DAMAGED) {
    state = RADIOLIB_ERR_NONE;
  }
  
  if(state != RADIOLIB_ERR_NONE) {
    #if !RADIOLIB_STATIC_ONLY
      delete[] downlinkMsg;
    #endif
    return(state);
  }

  // check the address
  uint32_t addr = LoRaWANNode::ntoh<uint32_t>(&downlinkMsg[RADIOLIB_LORAWAN_FHDR_DEV_ADDR_POS]);
  uint32_t expectedAddr = this->devAddr;
  if(this->multicast && window == RADIOLIB_LORAWAN_RX_BC) {
    expectedAddr = this->mcAddr;
  }
  if(addr != expectedAddr) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Device address mismatch, expected 0x%08lX, got 0x%08lX", 
                                    (unsigned long)expectedAddr, (unsigned long)addr);
    #if !RADIOLIB_STATIC_ONLY
      delete[] downlinkMsg;
    #endif
    return(RADIOLIB_ERR_DOWNLINK_MALFORMED);
  }

  // calculate length of (piggy-backed) FOpts
  uint8_t fOptsLen = downlinkMsg[RADIOLIB_LORAWAN_FHDR_FCTRL_POS] & RADIOLIB_LORAWAN_FHDR_FOPTS_LEN_MASK;

  // MHDR(1) - DevAddr(4) - FCtrl(1) - FCnt(2) - FOpts - Payload - MIC(4)
  // potentially also an FPort, will find out next
  uint8_t payLen = downlinkMsgLen - 1 - 4 - 1 - 2 - fOptsLen - 4;

  // in LoRaWAN v1.1, a frame is a Network frame if there is no Application payload
  // i.e.: either no payload at all (empty frame or FOpts only), or MAC only payload
  uint8_t fPort = RADIOLIB_LORAWAN_FPORT_MAC_COMMAND;
  bool isAppDownlink = false;
  if(this->rev == 0) {
    isAppDownlink = true;
  }
  if(payLen > 0) {
    payLen -= 1;  // subtract one as fPort is set
    fPort = downlinkMsg[RADIOLIB_LORAWAN_FHDR_FPORT_POS(fOptsLen)];

    // check destination fPort
    bool ok = false;

    // LoRaWAN v1.0.4 only: A Class B/C downlink SHALL NOT transport any MAC command. 
    // (...) it SHALL silently discard the entire frame.
    // However, we also enforce this for LoRaWAN v1.1 (TTS does not allow this anyway).
    if(fPort == RADIOLIB_LORAWAN_FPORT_MAC_COMMAND && window < RADIOLIB_LORAWAN_RX_BC) {
      // payload consists of all MAC commands (or is empty)
      ok = true;
    }
    if(fPort >= RADIOLIB_LORAWAN_FPORT_PAYLOAD_MIN && fPort <= RADIOLIB_LORAWAN_FPORT_PAYLOAD_MAX) {
      ok = true;
      isAppDownlink = true;
    }
    // check if any of the packages uses this FPort
    if(fPort >= RADIOLIB_LORAWAN_FPORT_RESERVED) {
      for(int id = 0; id < RADIOLIB_LORAWAN_NUM_SUPPORTED_PACKAGES; id++) {
        if(this->packages[id].enabled && fPort == this->packages[id].packFPort) {
          ok = true;
          isAppDownlink = this->packages[id].isAppPack;
          break;
        }
      }
    }

    if(!ok) {
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Received downlink at FPort %d - rejected! This FPort is reserved.", fPort);
      #if !RADIOLIB_STATIC_ONLY
        delete[] downlinkMsg;
      #endif
      return(RADIOLIB_ERR_INVALID_PORT);
    }
  }

  // handle FOpts in uplink with FPort = 0 (or absent which means 0)
  if(fPort == RADIOLIB_LORAWAN_FPORT_MAC_COMMAND) {
    if(fOptsLen > 0 && payLen > 0) {
      // MAC commands SHALL NOT be present in the payload field and the frame options field simultaneously. 
      // Should this occur, the end-device SHALL silently discard the frame.
      #if !RADIOLIB_STATIC_ONLY
        delete[] downlinkMsg;
      #endif
      return(RADIOLIB_ERR_DOWNLINK_MALFORMED);
    }
    // if FOpts are in the payload, use this instead
    if(payLen > 0) {
      fOptsLen = payLen;
    }
  }

  // LoRaWAN v1.0.4 only: A Class B/C downlink SHALL NOT transport any MAC command. 
  // (...) it SHALL silently discard the entire frame.
  // However, we also enforce this for LoRaWAN v1.1 (TTS does not allow this anyway).
  if(fOptsLen > 0 && window == RADIOLIB_LORAWAN_RX_BC) {
    #if !RADIOLIB_STATIC_ONLY
      delete[] downlinkMsg;
    #endif
    return(RADIOLIB_ERR_DOWNLINK_MALFORMED);
  }
  
  // get the frame counter
  uint16_t payFCnt16 = LoRaWANNode::ntoh<uint16_t>(&downlinkMsg[RADIOLIB_LORAWAN_FHDR_FCNT_POS]);

  // check the FCntDown value (Network or Application, or Multicast)
  uint32_t devFCnt32 = 0;
  if(this->multicast && window == RADIOLIB_LORAWAN_RX_BC) {
    // multicast: McApp downlink counter
    devFCnt32 = this->mcAFCnt;
  } else {
    // unicast: App or Nwk downlink
    if(isAppDownlink) {
      devFCnt32 = this->aFCntDown;
    } else {
      devFCnt32 = this->nFCntDown;
    }
  }

  // assume a rollover if the FCnt16 in the payload is equal to / smaller 
  // than the previous FCnt16 known by device
  // (MAX_FCNT_GAP is deprecated for 1.0.4 / 1.1, TTS and CS both apply a 16-bit rollover)
  if(devFCnt32 > 0 && payFCnt16 <= (uint16_t)devFCnt32) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("FCnt rollover: %d -> %d", (uint16_t)devFCnt32, payFCnt16);
    devFCnt32 += 0x10000;   // add 16-bit value
  }
  devFCnt32 &= ~0xFFFF;     // clear lower 16 bits known by device
  devFCnt32 |= payFCnt16;   // set lower 16 bits from payload

  // for multicast, a maximum FCnt value is defined in TS005
  if(this->multicast && window == RADIOLIB_LORAWAN_RX_BC) {
    if(devFCnt32 > this->mcAFCntMax) {
      #if !RADIOLIB_STATIC_ONLY
        delete[] downlinkMsg;
      #endif
      return(RADIOLIB_ERR_MULTICAST_FCNT_INVALID);
    }
  }

  // check if the ACK bit is set, indicating this frame acknowledges the previous uplink
  bool isConfirmingUp = false;
  if((downlinkMsg[RADIOLIB_LORAWAN_FHDR_FCTRL_POS] & RADIOLIB_LORAWAN_FCTRL_ACK)) {
    isConfirmingUp = true;
  }

  // set the MIC calculation blocks
  memset(downlinkMsg, 0x00, RADIOLIB_AES128_BLOCK_SIZE);
  downlinkMsg[RADIOLIB_LORAWAN_BLOCK_MAGIC_POS] = RADIOLIB_LORAWAN_MIC_BLOCK_MAGIC;
  // if this downlink is confirming an uplink, the MIC was generated with the least-significant 16 bits of that fCntUp
  // (LoRaWAN v1.1 only)
  if(isConfirmingUp && (this->rev == 1)) {
    LoRaWANNode::hton<uint16_t>(&downlinkMsg[RADIOLIB_LORAWAN_BLOCK_CONF_FCNT_POS], (uint16_t)this->confFCntUp);
  }
  downlinkMsg[RADIOLIB_LORAWAN_BLOCK_DIR_POS] = RADIOLIB_LORAWAN_DOWNLINK;
  LoRaWANNode::hton<uint32_t>(&downlinkMsg[RADIOLIB_LORAWAN_BLOCK_DEV_ADDR_POS], addr);
  LoRaWANNode::hton<uint32_t>(&downlinkMsg[RADIOLIB_LORAWAN_BLOCK_FCNT_POS], devFCnt32);
  downlinkMsg[RADIOLIB_LORAWAN_MIC_BLOCK_LEN_POS] = downlinkMsgLen - sizeof(uint32_t);

  // check the MIC
  // (if a rollover was more than 16-bit, this will always result in MIC mismatch)
  uint8_t* micKey = this->sNwkSIntKey;
  if(this->multicast && window == RADIOLIB_LORAWAN_RX_BC) {
    micKey = this->mcNwkSKey;
  }
  if(!verifyMIC(downlinkMsg, RADIOLIB_AES128_BLOCK_SIZE + downlinkMsgLen, micKey)) {
    #if !RADIOLIB_STATIC_ONLY
      delete[] downlinkMsg;
    #endif
    return(RADIOLIB_ERR_MIC_MISMATCH);
  }

  // all checks passed, so dump its contents and start processing
  RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Downlink (%sFCntDown = %lu) encoded:", 
                                  (this->multicast && window == RADIOLIB_LORAWAN_RX_BC) ? "M" :
                                                                  (isAppDownlink ? "A" : "N"), 
                                  (unsigned long)devFCnt32);
  RADIOLIB_DEBUG_PROTOCOL_HEXDUMP(downlinkMsg, RADIOLIB_AES128_BLOCK_SIZE + downlinkMsgLen);

  // save new FCnt to respective frame counter
  if(this->multicast && window == RADIOLIB_LORAWAN_RX_BC) {
    // multicast: McApp downlink
    this->mcAFCnt = devFCnt32;
  } else {
    // unicast: App or Nwk downlink
    if(isAppDownlink) {
      this->aFCntDown = devFCnt32;
    } else {
      this->nFCntDown = devFCnt32;
    }
  }

  bool isConfirmedDown = false;

  // do some housekeeping for normal Class A downlinks (not allowed for Class B/C)
  if(window < RADIOLIB_LORAWAN_RX_BC) {
    // if this is a confirmed frame, save the downlink number (only app frames can be confirmed)
    if((downlinkMsg[RADIOLIB_LORAWAN_FHDR_LEN_START_OFFS] & 0xFE) == RADIOLIB_LORAWAN_MHDR_MTYPE_CONF_DATA_DOWN) {
      this->confFCntDown = this->aFCntDown;
      isConfirmedDown = true;
    }

    // a Class A downlink was received, so restart the ADR counter with the next uplink
    this->adrFCnt = this->getFCntUp() + 1;

    // a Class A downlink was received, so we can clear the MAC uplink and downlink buffers
    memset(this->fOptsUp, 0, RADIOLIB_LORAWAN_FHDR_FOPTS_MAX_LEN);
    this->fOptsUpLen = 0;
    memset(this->fOptsDown, 0, RADIOLIB_LORAWAN_FHDR_FOPTS_MAX_LEN);
    this->fOptsDownLen = 0;
  }

  #if !RADIOLIB_STATIC_ONLY
    uint8_t* fOpts = new uint8_t[fOptsLen];
  #else
    uint8_t fOpts[RADIOLIB_STATIC_ARRAY_SIZE];
  #endif

  // decrypt any FOpts on FPort = 0, in which case FOptsLen is the length of the payload
  if(fPort == RADIOLIB_LORAWAN_FPORT_MAC_COMMAND && payLen > 0) {
    payLen = 0;
    processAES(&downlinkMsg[RADIOLIB_LORAWAN_FRAME_PAYLOAD_POS(0)], fOptsLen, this->nwkSEncKey, fOpts, addr, devFCnt32, RADIOLIB_LORAWAN_DOWNLINK, 0x00, true);
  
  // decrypt any piggy-backed FOpts
  } else if(fOptsLen > 0) {
    // the decryption depends on the LoRaWAN version
    if(this->rev == 1) {
      // in LoRaWAN v1.1, the piggy-backed FOpts are encrypted using the NwkSEncKey
      uint8_t ctrId = 0x01 + isAppDownlink; // see LoRaWAN v1.1 errata
      processAES(&downlinkMsg[RADIOLIB_LORAWAN_FHDR_FOPTS_POS], (size_t)fOptsLen, this->nwkSEncKey, fOpts, this->devAddr, devFCnt32, RADIOLIB_LORAWAN_DOWNLINK, ctrId, true);
    } else {
      // in LoRaWAN v1.0.x, the piggy-backed FOpts are unencrypted
      memcpy(fOpts, &downlinkMsg[RADIOLIB_LORAWAN_FHDR_FOPTS_POS], (size_t)fOptsLen);
    }
  }

  // process any FOpts
  if(fOptsLen > 0) {
    uint8_t* mPtr = fOpts;
    uint8_t procLen = 0;
    uint8_t fOptsRe[RADIOLIB_LORAWAN_MAX_DOWNLINK_SIZE] = { 0 };
    uint8_t fOptsReLen = 0;

    // indication whether LinkAdr MAC command has been processed
    bool mAdr = false;

    while(procLen < fOptsLen) {
      uint8_t cid = *mPtr;      // MAC id is the first byte

      // fetch length of MAC downlink command and uplink response
      uint8_t fLen = 1;
      uint8_t fLenRe = 1;
      state = this->getMacLen(cid, &fLen, RADIOLIB_LORAWAN_DOWNLINK, true, mPtr + 1);
      if(state != RADIOLIB_ERR_NONE) {
        RADIOLIB_DEBUG_PROTOCOL_PRINTLN("WARNING: Unknown MAC CID %02x", cid);
        RADIOLIB_DEBUG_PROTOCOL_PRINTLN("WARNING: Skipping remaining MAC payload");
        fOptsLen = procLen; // truncate to last processed MAC command
        break;
      }
      (void)this->getMacLen(cid, &fLenRe, RADIOLIB_LORAWAN_UPLINK, true);

      // check whether the complete payload is present
      if(procLen + fLen > fOptsLen) {
        RADIOLIB_DEBUG_PROTOCOL_PRINTLN("WARNING: Incomplete MAC command %02x (%d bytes, expected %d)", cid, fOptsLen - procLen, fLen);
        RADIOLIB_DEBUG_PROTOCOL_PRINTLN("WARNING: Skipping remaining MAC payload");
        fOptsLen = procLen; // truncate to last processed MAC command
        break;
      }

      bool reply = false;

      // if this is a LinkAdr MAC command, pre-process contiguous commands into one atomic block
      if(cid == RADIOLIB_LORAWAN_MAC_LINK_ADR) {
        // if there was any LinkAdr command before, set NACK and continue without processing
        if(mAdr) {
          reply = true;
          fOptsRe[fOptsReLen + 1] = 0x00;

        // if this is the first LinkAdr command, do some special treatment:
        } else {
          mAdr = true;
          uint8_t fAdrLen = 5;
          uint8_t mAdrOpt[14] = { 0 };

          // retrieve all contiguous LinkAdr commands
          while(procLen + fLen + fAdrLen < fOptsLen + 1 && *(mPtr + fLen) == RADIOLIB_LORAWAN_MAC_LINK_ADR) {
            fLen += 5;    // ADR command is 5 bytes
            fLenRe += 2;  // ADR response is 2 bytes
          }

          // pre-process them into a single complete channel mask (stored in mAdrOpt)
          LoRaWANNode::preprocessMacLinkAdr(mPtr, fLen, mAdrOpt);

          // execute like a normal MAC command (but pointing to mAdrOpt instead)
          reply = this->execMacCommand(cid, mAdrOpt, 14, &fOptsRe[fOptsReLen + 1]);

          // in LoRaWAN v1.0.x, all ACK bytes should have equal status - fix in post-processing
          if(this->rev == 0) {
            LoRaWANNode::postprocessMacLinkAdr(&fOptsRe[fOptsReLen], fLen);

          // in LoRaWAN v1.1, just provide one ACK, so no post-processing but cut off reply length 
          } else {
            fLenRe = 2;
          }
        }

      // MAC command other than LinkAdr, just process the payload
      } else {
        reply = this->execMacCommand(cid, mPtr + 1, fLen - 1, &fOptsRe[fOptsReLen + 1]);
      }

      // if there is a reply, add it to the response payload
      if(reply) {
        fOptsRe[fOptsReLen] = cid;
        fOptsReLen += fLenRe;
      }

      procLen += fLen;
      mPtr += fLen;
    }

    // remove all MAC commands except those whose payload can be requested by the user
    // (which are LinkCheck and DeviceTime)  
    this->fOptsDownLen = fOptsLen;
    LoRaWANNode::clearMacCommands(fOpts, &this->fOptsDownLen, RADIOLIB_LORAWAN_DOWNLINK);
    memcpy(this->fOptsDown, fOpts, this->fOptsDownLen);

    // if fOptsLen for the next uplink is larger than can be piggybacked onto an uplink, send separate uplink
    if(fOptsReLen > RADIOLIB_LORAWAN_FHDR_FOPTS_MAX_LEN) {
      this->isMACPayload = true;
    }

    // get the maximum uplink payload size
    uint8_t maxReLen = this->getMaxPayloadLen();

    // truncate uplink payload size if necessary, and send separate uplink
    if(fOptsReLen > maxReLen) {
      this->isMACPayload = true;

      fOptsReLen = 0;
      uint8_t fLenRe = 0;

      // move back to the start of the uplink buffer
      mPtr = fOpts;
      // and add as many MAC commands as space is available
      while(fOptsReLen + fLenRe <= maxReLen) {
        fOptsReLen += fLenRe;

        // fetch length of MAC uplink response
        (void)this->getMacLen(*mPtr, &fLenRe, RADIOLIB_LORAWAN_UPLINK, true, mPtr + 1);
        mPtr += fLenRe;
      }
    }

    // if the limit on number of FOpts is reached, send a MAC-only uplink
    // otherwise, the user might get stuck as the app payload won't fit
    if(this->isMACPayload) {
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("! Sending MAC-only uplink (%d bytes):", fOptsReLen);
      RADIOLIB_DEBUG_PROTOCOL_HEXDUMP(fOptsRe, fOptsReLen);

      // temporarily lift dutyCycle restrictions to allow immediate MAC response
      bool prevDC = this->dutyCycleEnabled;
      this->dutyCycleEnabled = false;
      this->sendReceive(fOptsRe, fOptsReLen, RADIOLIB_LORAWAN_FPORT_MAC_COMMAND);
      this->dutyCycleEnabled = prevDC;

    } else { // fOptsReLen <= 15
      memcpy(this->fOptsUp, fOptsRe, fOptsReLen);
      this->fOptsUpLen = fOptsReLen;
    }
  }

  // figure out which key to use to decrypt the payload
  uint8_t* encKey = this->appSKey;
  if(this->multicast && window == RADIOLIB_LORAWAN_RX_BC) {
    encKey = this->mcAppSKey;
  }
  for(int id = 0; id < RADIOLIB_LORAWAN_NUM_SUPPORTED_PACKAGES; id++) {
    if(this->packages[id].enabled && fPort == this->packages[id].packFPort) {
      encKey = this->packages[id].isAppPack ? this->appSKey : this->nwkSEncKey;
      break;
    }
  }

  // decrypt the frame payload
  // by default, the data and length are user-accessible
  processAES(&downlinkMsg[RADIOLIB_LORAWAN_FRAME_PAYLOAD_POS(fOptsLen)], payLen, encKey, data, addr, devFCnt32, RADIOLIB_LORAWAN_DOWNLINK, 0x00, true);
  *len = payLen;

  // however, if this frame belongs to a package, redirect instead and 'hide' contents from the user
  // just to be sure that it doesn't get re-interpreted...
  for(int id = 0; id < RADIOLIB_LORAWAN_NUM_SUPPORTED_PACKAGES; id++) {
    if(this->packages[id].enabled && fPort == this->packages[id].packFPort) {
      this->packages[id].callback(data, *len);
      memset(data, 0, *len);
      *len = 0;
    }
  }

  // pass the event info if requested
  if(event) {
    event->dir = RADIOLIB_LORAWAN_DOWNLINK;
    event->confirmed = isConfirmedDown;
    event->confirming = isConfirmingUp;
    event->frmPending = (downlinkMsg[RADIOLIB_LORAWAN_FHDR_FCTRL_POS] & RADIOLIB_LORAWAN_FCTRL_FRAME_PENDING) != 0;
    event->datarate = this->channels[window].dr;
    event->freq = this->channels[window].freq / 10000.0;
    event->power = this->txPowerMax - this->txPowerSteps * 2;
    event->fCnt = devFCnt32;
    event->fPort = fPort;
    event->multicast = (bool)this->multicast;
  }

  #if !RADIOLIB_STATIC_ONLY
    delete[] fOpts;
    delete[] downlinkMsg;
  #endif

  return(RADIOLIB_ERR_NONE);
}

int16_t LoRaWANNode::getDownlinkClassC(uint8_t* dataDown, size_t* lenDown, LoRaWANEvent_t* eventDown) {
  // only allow if the device is Unicast-C or Multicast-C, otherwise ignore without error
  if(this->lwClass != RADIOLIB_LORAWAN_CLASS_C && this->multicast != RADIOLIB_LORAWAN_CLASS_C) {
    return(RADIOLIB_ERR_NONE);
  }

  int16_t state = RADIOLIB_ERR_NONE;

  if(downlinkAction) {
    state = this->parseDownlink(dataDown, lenDown, RADIOLIB_LORAWAN_RX_BC, eventDown);
    downlinkAction = false;

    // if downlink parsed successfully, set state to RxC window
    if(state == RADIOLIB_ERR_NONE) {
      state = RADIOLIB_LORAWAN_RX_BC;

    // otherwise, if device is acting as Multicast on top of the same Unicast class,
    // try decrypting it as a Unicast downlink by temporarily disabling Multicast
    } else if(this->multicast == this->lwClass) {
      this->multicast = false;
      state = this->parseDownlink(dataDown, lenDown, RADIOLIB_LORAWAN_RX_BC, eventDown);
      this->multicast = this->lwClass;
      // if downlink parsed succesfully, set state to RxC window
      if(state == RADIOLIB_ERR_NONE) {
        state = RADIOLIB_LORAWAN_RX_BC;
      }
    }
  }

  return(state);
}

bool LoRaWANNode::execMacCommand(uint8_t cid, uint8_t* optIn, uint8_t lenIn) {
  uint8_t buff[RADIOLIB_LORAWAN_MAX_MAC_COMMAND_LEN_DOWN];
  return(this->execMacCommand(cid, optIn, lenIn, buff));
}

bool LoRaWANNode::execMacCommand(uint8_t cid, uint8_t* optIn, uint8_t lenIn, uint8_t* optOut) {
  RADIOLIB_DEBUG_PROTOCOL_PRINTLN("[MAC] 0x%02x", cid);
  RADIOLIB_DEBUG_PROTOCOL_HEXDUMP(optIn, lenIn);

  if(cid >= RADIOLIB_LORAWAN_MAC_PROPRIETARY) {
    // TODO call user-provided callback for proprietary MAC commands?
    return(false);
  }

  switch(cid) {
    case(RADIOLIB_LORAWAN_MAC_RESET): {
      // get the server version
      uint8_t srvVersion = optIn[0];
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("ResetConf: server version 1.%d", srvVersion);
      if(srvVersion != this->rev) {
        // invalid server version, resend the ResetInd MAC command
        RADIOLIB_DEBUG_PROTOCOL_PRINTLN("ERROR! Please disable your device and consult supported LoRaWAN versions");
        (void)LoRaWANNode::pushMacCommand(cid, &this->rev, this->fOptsUp, &this->fOptsUpLen, RADIOLIB_LORAWAN_UPLINK);
      }
      return(false);
    } break;

    case(RADIOLIB_LORAWAN_MAC_LINK_CHECK): {
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("LinkCheckAns: [user]");

      return(false);
    } break;

    case(RADIOLIB_LORAWAN_MAC_LINK_ADR): {
      // get the ADR configuration
      uint8_t macDrUp = (optIn[0] & 0xF0) >> 4;
      uint8_t macTxSteps = optIn[0] & 0x0F;
      
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("LinkAdrReq: dataRate = %d, txSteps = %d, nbTrans = %d", macDrUp, macTxSteps, lenIn > 1 ? optIn[13] : 0);

      uint8_t chMaskAck = 0;
      uint8_t drAck = 0;
      uint8_t pwrAck = 0;

      // first, get current configuration
      uint64_t chMaskGrp0123 = 0;
      uint32_t chMaskGrp45 = 0;
      this->getChannelPlanMask(&chMaskGrp0123, &chMaskGrp45);
      uint16_t chMaskActive = 0;
      (void)this->getAvailableChannels(&chMaskActive);
      uint8_t currentDr = this->channels[RADIOLIB_LORAWAN_UPLINK].dr;

      // only apply channel mask if present (internal Dr/Tx commands do not set channel mask)
      if(lenIn > 1) {
        uint64_t macChMaskGrp0123 = LoRaWANNode::ntoh<uint64_t>(&optIn[1]);
        uint32_t macChMaskGrp45 = LoRaWANNode::ntoh<uint32_t>(&optIn[9]);
        // apply requested channel mask and enable all of them for testing datarate
        chMaskAck = this->applyChannelMask(macChMaskGrp0123, macChMaskGrp45);
      } else {
        chMaskAck = true;
      }
      
      this->setAvailableChannels(0xFFFF);

      int16_t state;

      // try to apply the datarate configuration
      // if value is set to 'keep current values', retrieve current value
      if(macDrUp == 0x0F) {
        macDrUp = currentDr;
      }

      if (this->band->dataRates[macDrUp] != RADIOLIB_LORAWAN_DATA_RATE_UNUSED) {
        // check if the module supports this data rate
        DataRate_t dr;
        state = this->findDataRate(macDrUp, &dr);

        // if datarate in hardware all good, set datarate for now
        // and check if there are any available Tx channels for this datarate
        if(state == RADIOLIB_ERR_NONE) {
          this->channels[RADIOLIB_LORAWAN_UPLINK].dr = macDrUp;

          // only if we have available Tx channels, we set an Ack
          if(this->getAvailableChannels(NULL) > 0) {
            drAck = 1;
          } else {
            RADIOLIB_DEBUG_PROTOCOL_PRINTLN("ADR: no channels available for datarate %d", macDrUp);
          }
        } else {
          RADIOLIB_DEBUG_PROTOCOL_PRINTLN("ADR: hardware failure configurating datarate %d, code %d", macDrUp, state);
        }
      
      }

      // try to apply the power configuration
      // if value is set to 'keep current values', retrieve current value
      if(macTxSteps == 0x0F) {
        macTxSteps = this->txPowerSteps;
      }

      // only allow TxPower if less than / equal to the maximum number of defined steps
      if(macTxSteps <= this->band->powerNumSteps) {
        int8_t power = this->txPowerMax - 2*macTxSteps;
        int8_t powerActual = 0;
        state = this->phyLayer->checkOutputPower(power, &powerActual);
        // only acknowledge if the radio is able to operate at or below the requested power level
        if(state == RADIOLIB_ERR_NONE || (state == RADIOLIB_ERR_INVALID_OUTPUT_POWER && powerActual < power)) {
          pwrAck = 1;
        } else {
          RADIOLIB_DEBUG_PROTOCOL_PRINTLN("ADR failed to configure Tx power %d, code %d!", power, state);
        }
      }

      // set ACK bits
      optOut[0] = (pwrAck << 2) | (drAck << 1) | (chMaskAck << 0);

      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("LinkAdrAns: %02x", optOut[0]);

      // if ACK not completely successful, revert and stop
      if(optOut[0] != 0x07) {
        // according to paragraph 4.3.1.1, if ADR is disabled, 
        // the ADR channel mask must be accepted even if drAck/pwrAck fails.
        // therefore, only revert the channel mask if ADR is enabled.
        if(this->adrEnabled) {
          this->applyChannelMask(chMaskGrp0123, chMaskGrp45);
          this->setAvailableChannels(chMaskActive);
        }
        // revert datarate
        this->channels[RADIOLIB_LORAWAN_UPLINK].dr = currentDr;
        // Tx power was not actually modified

        return(true);
      }

      // ACK successful, so apply and save
      this->txPowerSteps = macTxSteps;
      if(lenIn > 1) {
        uint8_t macNbTrans = optIn[13] & 0x0F;

        // if there is a value for NbTrans > 0, apply it
        if(macNbTrans) {
          this->nbTrans = macNbTrans;
        } else {
          // for LoRaWAN v1.0.4, if NbTrans == 0, the end-device SHALL use the default value (being 1)
          if(this->rev == 0) {
            this->nbTrans = 1;
          }
          // for LoRaWAN v1.1, if NbTrans == 0, the end-device SHALL keep the current NbTrans value unchanged
          // so, don't do anything
        }
        
      }

      // restore original active channels
      this->setAvailableChannels(chMaskActive);

      // save to the ADR MAC location
      // but first re-set the Dr/Tx/NbTrans field to make sure they're not set to 0xF
      optIn[0]  = (this->channels[RADIOLIB_LORAWAN_UPLINK].dr) << 4;
      optIn[0] |= this->txPowerSteps;
      if(lenIn > 1) {
        optIn[13] = this->nbTrans;
      }
      memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_LINK_ADR], optIn, lenIn);

      return(true);
    } break;

    case(RADIOLIB_LORAWAN_MAC_DUTY_CYCLE): {
      uint8_t maxDutyCycle = optIn[0] & 0x0F;
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("DutyCycleReq: max duty cycle = 1/2^%d", maxDutyCycle);
      if(maxDutyCycle == 0) {
        this->dutyCycle = this->band->dutyCycle;
      } else {
        this->dutyCycle = (RadioLibTime_t)60 * (RadioLibTime_t)60 * (RadioLibTime_t)1000 / (RadioLibTime_t)(1UL << maxDutyCycle);
      }

      memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_DUTY_CYCLE], optIn, lenIn);

      return(true);
    } break;

    case(RADIOLIB_LORAWAN_MAC_RX_PARAM_SETUP): {
      // get the configuration
      uint8_t macRx1DrOffset = (optIn[0] & 0x70) >> 4;
      uint8_t macRx2Dr = optIn[0] & 0x0F;
      uint32_t macRx2Freq = LoRaWANNode::ntoh<uint32_t>(&optIn[1], 3);
      
      uint8_t rx1DrOsAck = 0;
      uint8_t rx2DrAck = 0;
      uint8_t rx2FreqAck = 0;

      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("RXParamSetupReq: Rx1DrOffset = %d, rx2DataRate = %d, freq = %7.3f", 
                                      macRx1DrOffset, macRx2Dr, macRx2Freq / 10000.0);
      
      // check the requested configuration
      uint8_t uplinkDr = this->channels[RADIOLIB_LORAWAN_UPLINK].dr;
      DataRate_t dr;
      if(this->band->rx1DrTable[uplinkDr][macRx1DrOffset] != RADIOLIB_LORAWAN_DATA_RATE_UNUSED) {
        if(this->findDataRate(this->band->rx1DrTable[uplinkDr][macRx1DrOffset], &dr) == RADIOLIB_ERR_NONE) {
          rx1DrOsAck = 1;
        }
      }
      if(macRx2Dr >= this->band->rx2.drMin && macRx2Dr <= this->band->rx2.drMax) {
        if(this->band->dataRates[macRx2Dr] != RADIOLIB_LORAWAN_DATA_RATE_UNUSED) {
          if(this->findDataRate(macRx2Dr, &dr) == RADIOLIB_ERR_NONE) {
            rx2DrAck = 1;
          }
        }
      }
      if(macRx2Freq >= this->band->freqMin && macRx2Freq <= this->band->freqMax) {
        if(this->phyLayer->setFrequency(macRx2Freq / 10000.0) == RADIOLIB_ERR_NONE) {
          rx2FreqAck = 1;
        }
      }
      optOut[0] = (rx1DrOsAck << 2) | (rx2DrAck << 1) | (rx2FreqAck << 0);

      // if not fully acknowledged, return now without applying the requested configuration
      if(optOut[0] != 0x07) {
        return(true);
      }

      // passed ACK, so apply configuration
      this->rx1DrOffset = macRx1DrOffset;
      this->channels[RADIOLIB_LORAWAN_RX2].dr = macRx2Dr;
      this->channels[RADIOLIB_LORAWAN_RX2].freq = macRx2Freq;
      this->channels[RADIOLIB_LORAWAN_RX_BC].dr = macRx2Dr;
      this->channels[RADIOLIB_LORAWAN_RX_BC].freq = macRx2Freq;
      memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_RX_PARAM_SETUP], optIn, lenIn);

      return(true);
    } break;

    case(RADIOLIB_LORAWAN_MAC_DEV_STATUS): {
      // set the uplink reply
      optOut[0] = this->battLevel;
      int8_t snr = this->phyLayer->getSNR();
      optOut[1] = snr & 0x3F;

      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("DevStatusAns: status = 0x%02x%02x", optOut[0], optOut[1]);
      return(true);
    } break;

    case(RADIOLIB_LORAWAN_MAC_NEW_CHANNEL): {
      // only implemented on dynamic bands
      if(this->band->bandType == RADIOLIB_LORAWAN_BAND_FIXED) {
        return(false);
      }

      // get the configuration
      uint8_t macChIndex = optIn[0];
      uint32_t macFreq = LoRaWANNode::ntoh<uint32_t>(&optIn[1], 3);
      uint8_t macDrMax = (optIn[4] & 0xF0) >> 4;
      uint8_t macDrMin = optIn[4] & 0x0F;
      
      uint8_t drAck = 0;
      uint8_t freqAck = 0;

      // the default channels shall not be modified, so check if this is a default channel
      // if the channel index is set, this channel is defined, so return a NACK
      if(macChIndex < 3 && this->band->txFreqs[macChIndex].idx != RADIOLIB_LORAWAN_CHANNEL_INDEX_NONE) {
        optOut[0] = 0;
        return(true);
      }

      // check if the outermost datarates are defined and if the device supports them
      DataRate_t dr;
      if(this->band->dataRates[macDrMin] != RADIOLIB_LORAWAN_DATA_RATE_UNUSED && this->findDataRate(macDrMin, &dr) == RADIOLIB_ERR_NONE) {
        if(this->band->dataRates[macDrMax] != RADIOLIB_LORAWAN_DATA_RATE_UNUSED && this->findDataRate(macDrMax, &dr) == RADIOLIB_ERR_NONE) {
          drAck = 1;
        }
      }

      // check if the frequency is allowed and possible
      if(macFreq >= this->band->freqMin && macFreq <= this->band->freqMax) {
        if(this->phyLayer->setFrequency((float)macFreq / 10000.0f) == RADIOLIB_ERR_NONE) {
          freqAck = 1;
        }
      // otherwise, if frequency is 0, disable the channel which is also a valid option
      } else if(macFreq == 0) {
        freqAck = 1;
      }

      // set ACK bits
      optOut[0] = (drAck << 1) | (freqAck << 0);

      // if not fully acknowledged, return now without applying the requested configuration
      if(optOut[0] != 0x03) {
        return(true);
      }

      // ACK successful, so apply and save
      if(macFreq > 0) {
        this->channelPlan[RADIOLIB_LORAWAN_UPLINK][macChIndex].enabled   = true;
        this->channelPlan[RADIOLIB_LORAWAN_UPLINK][macChIndex].idx       = macChIndex;
        this->channelPlan[RADIOLIB_LORAWAN_UPLINK][macChIndex].freq      = macFreq;
        this->channelPlan[RADIOLIB_LORAWAN_UPLINK][macChIndex].drMin     = macDrMin;
        this->channelPlan[RADIOLIB_LORAWAN_UPLINK][macChIndex].drMax     = macDrMax;
        this->channelPlan[RADIOLIB_LORAWAN_UPLINK][macChIndex].available = true;
        // downlink channel is identical to uplink channel
        this->channelPlan[RADIOLIB_LORAWAN_DOWNLINK][macChIndex] = this->channelPlan[RADIOLIB_LORAWAN_UPLINK][macChIndex];
      } else {
        this->channelPlan[RADIOLIB_LORAWAN_UPLINK][macChIndex] = RADIOLIB_LORAWAN_CHANNEL_NONE;
        this->channelPlan[RADIOLIB_LORAWAN_DOWNLINK][macChIndex] = RADIOLIB_LORAWAN_CHANNEL_NONE;

      }

      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("UL: %3d %d %7.3f (%d - %d) | DL: %3d %d %7.3f (%d - %d)", 
                              this->channelPlan[RADIOLIB_LORAWAN_UPLINK][macChIndex].idx,
                              this->channelPlan[RADIOLIB_LORAWAN_UPLINK][macChIndex].enabled,
                              this->channelPlan[RADIOLIB_LORAWAN_UPLINK][macChIndex].freq / 10000.0,
                              this->channelPlan[RADIOLIB_LORAWAN_UPLINK][macChIndex].drMin,
                              this->channelPlan[RADIOLIB_LORAWAN_UPLINK][macChIndex].drMax,

                              this->channelPlan[RADIOLIB_LORAWAN_DOWNLINK][macChIndex].idx,
                              this->channelPlan[RADIOLIB_LORAWAN_DOWNLINK][macChIndex].enabled,
                              this->channelPlan[RADIOLIB_LORAWAN_DOWNLINK][macChIndex].freq / 10000.0,
                              this->channelPlan[RADIOLIB_LORAWAN_DOWNLINK][macChIndex].drMin,
                              this->channelPlan[RADIOLIB_LORAWAN_DOWNLINK][macChIndex].drMax
                            );

      memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_UL_CHANNELS] + macChIndex * lenIn, optIn, lenIn);

      return(true);
    } break;

    case(RADIOLIB_LORAWAN_MAC_DL_CHANNEL): {
      // only implemented on dynamic bands
      if(this->band->bandType == RADIOLIB_LORAWAN_BAND_FIXED) {
        return(false);
      }

      // get the configuration
      uint8_t macChIndex = optIn[0];
      uint32_t macFreq = LoRaWANNode::ntoh<uint32_t>(&optIn[1], 3);
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("DlChannelReq: index = %d, freq = %7.3f MHz", macChIndex, macFreq / 10000.0);
      uint8_t freqDlAck = 0;
      uint8_t freqUlAck = 0;
      
      // check if the frequency is allowed possible
      if(macFreq >= this->band->freqMin && macFreq <= this->band->freqMax) { 
        if(this->phyLayer->setFrequency(macFreq / 10000.0) == RADIOLIB_ERR_NONE) {
          freqDlAck = 1;
        }
      }
      
      // check if the corresponding uplink frequency is actually set
      if(this->channelPlan[RADIOLIB_LORAWAN_UPLINK][macChIndex].freq > 0) {
        freqUlAck = 1;
      }

      // set ACK bits
      optOut[0] = (freqUlAck << 1) | (freqDlAck << 0);

      // if not fully acknowledged, return now without applying the requested configuration
      if(optOut[0] != 0x03) {
        return(true);
      }

      // ACK successful, so apply and save
      this->channelPlan[RADIOLIB_LORAWAN_DOWNLINK][macChIndex].freq = macFreq;

      memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_DL_CHANNELS] + macChIndex * lenIn, optIn, lenIn);

      return(true);
    } break;

    case(RADIOLIB_LORAWAN_MAC_RX_TIMING_SETUP): {
      // get the configuration
      uint8_t delay = optIn[0] & 0x0F;
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("RXTimingSetupReq: delay = %d sec", delay);
      
      // apply the configuration
      if(delay == 0) {
        delay = 1;
      }
      this->rxDelays[1] = (RadioLibTime_t)delay * (RadioLibTime_t)1000; // Rx1 delay
      this->rxDelays[2] = this->rxDelays[1] + 1000;   // Rx2 delay

      memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_RX_TIMING_SETUP], optIn, lenIn);

      return(true);
    } break;

    case(RADIOLIB_LORAWAN_MAC_TX_PARAM_SETUP): {
      // TxParamSetupReq is only supported on a subset of bands
      // in other bands, silently ignore without response
      if(!this->band->txParamSupported) {
        return(false);
      }
      uint8_t dlDwell = (optIn[0] & 0x20) >> 5;
      uint8_t ulDwell = (optIn[0] & 0x10) >> 4;
      uint8_t maxEirpRaw = optIn[0] & 0x0F;

      // who the f came up with this ...
      const uint8_t eirpEncoding[] = { 8, 10, 12, 13, 14, 16, 18, 20, 21, 24, 26, 27, 29, 30, 33, 36 };
      this->txPowerMax = eirpEncoding[maxEirpRaw];
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("TxParamSetupReq: dlDwell = %d, ulDwell = %d, maxEirp = %d dBm", dlDwell, ulDwell, eirpEncoding[maxEirpRaw]);

      this->dwellTimeUp = ulDwell ? RADIOLIB_LORAWAN_DWELL_TIME : 0;
      this->dwellTimeDn = dlDwell ? RADIOLIB_LORAWAN_DWELL_TIME : 0;

      memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_TX_PARAM_SETUP], optIn, lenIn);

      return(true);
    } break;

    case(RADIOLIB_LORAWAN_MAC_REKEY): {
      // get the server version
      uint8_t srvVersion = optIn[0];
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("RekeyConf: server version = 1.%d", srvVersion);

      // If the server's version is invalid the device SHALL discard the RekeyConf command and retransmit the RekeyInd in the next uplink frame
      if(srvVersion != this->rev) {
        RADIOLIB_DEBUG_PROTOCOL_PRINTLN("ERROR! Please disable your device and consult supported LoRaWAN versions");
        (void)LoRaWANNode::pushMacCommand(cid, &this->rev, this->fOptsUp, &this->fOptsUpLen, RADIOLIB_LORAWAN_UPLINK);
      }
      
      return(false);
    } break;

    case(RADIOLIB_LORAWAN_MAC_ADR_PARAM_SETUP): {
      this->adrLimitExp = (optIn[0] & 0xF0) >> 4;
      this->adrDelayExp = optIn[0] & 0x0F;
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("ADRParamSetupReq: limitExp = %d, delayExp = %d", this->adrLimitExp, this->adrDelayExp);

      memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_ADR_PARAM_SETUP], optIn, lenIn);

      return(true);
    } break;

    case(RADIOLIB_LORAWAN_MAC_DEVICE_TIME): {
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("DeviceTimeAns: [user]");

      return(false);
    } break;

    case(RADIOLIB_LORAWAN_MAC_FORCE_REJOIN): {
      // TODO implement this
      uint16_t rejoinReq = LoRaWANNode::ntoh<uint16_t>(optIn);
      uint8_t period = (rejoinReq & 0x3800) >> 11;
      uint8_t maxRetries = (rejoinReq & 0x0700) >> 8;
      uint8_t rejoinType = (rejoinReq & 0x0070) >> 4;
      uint8_t dr = rejoinReq & 0x000F;
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("ForceRejoinReq: period = %d, maxRetries = %d, rejoinType = %d, dr = %d", period, maxRetries, rejoinType, dr);
      (void)period;
      (void)maxRetries;
      (void)rejoinType;
      (void)dr;
      return(false);
    } break;

    case(RADIOLIB_LORAWAN_MAC_REJOIN_PARAM_SETUP): {
      // TODO implement this
      uint8_t maxTime = (optIn[0] & 0xF0) >> 4;
      uint8_t maxCount = optIn[0] & 0x0F;
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("RejoinParamSetupReq: maxTime = %d, maxCount = %d", maxTime, maxCount);

      memcpy(&this->bufferSession[RADIOLIB_LORAWAN_SESSION_REJOIN_PARAM_SETUP], optIn, lenIn);

      lenIn = 0;
      optIn[0] = (1 << 1) | 1;

      (void)maxTime;
      (void)maxCount;
      return(true);
    } break;

    case(RADIOLIB_LORAWAN_MAC_DEVICE_MODE): {
      // only implemented on LoRaWAN v1.1
      if(this->rev == 0) {
        return(false);
      }

      if(optIn[0] > RADIOLIB_LORAWAN_CLASS_C) {
        return(false);
      }

      // retrieve pending class from MAC uplink queue
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("DeviceMode: Switching to LoRaWAN Class %s", 
                                        optIn[0] == RADIOLIB_LORAWAN_CLASS_A ? "A" :
                                        optIn[0] == RADIOLIB_LORAWAN_CLASS_B ? "B" : "C");

      this->lwClass = optIn[0];

      return(false);
    } break;

    default: {
      // derived classes may implement additional MAC commands
      return(derivedMacHandler(cid, optIn, lenIn, optOut));
    }
  }

  return(false);
}

bool LoRaWANNode::derivedMacHandler(uint8_t cid, uint8_t* optIn, uint8_t lenIn, uint8_t* optOut) {
  (void)cid;
  (void)optIn;
  (void)lenIn;
  (void)optOut;
  return(false);
}

void LoRaWANNode::preprocessMacLinkAdr(uint8_t* mPtr, uint8_t cLen, uint8_t* mAdrOpt) {
  uint8_t fLen = 5;   // single ADR command is 5 bytes
  uint8_t numOpts = cLen / fLen;

  // get the current channel plan mask
  uint64_t chMaskGrp0123 = 0;
  uint32_t chMaskGrp45 = 0;
  this->getChannelPlanMask(&chMaskGrp0123, &chMaskGrp45);

  // set Dr/Tx field from last MAC command
  mAdrOpt[0] = mPtr[cLen - fLen + 1];

  // set NbTrans partial field from last MAC command
  mAdrOpt[13] = mPtr[cLen - fLen + 4] & 0x0F;

  for(uint8_t opt = 0; opt < numOpts; opt++) {
    uint8_t chMaskCntl = (mPtr[opt * fLen + 4] & 0x70) >> 4;
    uint16_t chMask = LoRaWANNode::ntoh<uint16_t>(&mPtr[opt * fLen + 2]);
    switch(chMaskCntl) {
      case 0:
      case 1:
      case 2:
      case 3:
        // clear the target 16-bit block
        chMaskGrp0123 &= ~((uint64_t)0xFFFF << (16 * chMaskCntl));
        // set the new 16-bit value in that block
        chMaskGrp0123 |= (uint64_t)chMask << (16 * chMaskCntl);
        break;
      case 4:
        // clear the target 16-bit block
        chMaskGrp45 &= ~((uint32_t)0xFFFF);
        // set the new 16-bit value in that block
        chMaskGrp45 |= (uint32_t)chMask;
        break;
      case 5:
        // for CN470, this is just a normal channel mask
        // for all other bands, the first 10 bits enable banks of 8 125kHz channels
        if(this->band->bandNum == BandCN470) {
          chMaskGrp45 &= ~((uint32_t)0xFFFF << 16);
          chMaskGrp45 |= (uint32_t)chMask << 16;
        } else {
          int bank = 0;
          for(; bank < 8; bank++) {
            chMaskGrp0123 &= ~((uint64_t)0xFF << (8 * bank));
            if(chMask & ((uint16_t)1 << bank)) {
              chMaskGrp0123 |= ((uint64_t)0xFF << (8 * bank));
            }
          }
          for(; bank < 10; bank++) {
            chMaskGrp45 &= ~((uint32_t)0xFF << (8 * (bank - 8)));
            if(chMask & ((uint16_t)1 << bank)) {
              chMaskGrp45 |= ((uint32_t)0xFF << (8 * (bank - 8)));
            }
          }
        }
        break;
      case 6:
        // for dynamic bands: all channels ON (that are currently defined)
        // for fixed bands:   all default 125kHz channels ON, channel mask similar to ChMask = 4
        // except for CN470:  all default 125kHz channels ON

        if(this->band->bandType == RADIOLIB_LORAWAN_BAND_FIXED && this->band->bandNum != BandCN470) {
          chMaskGrp45 &= ~((uint32_t)0xFFFF);
          chMaskGrp45 |= (uint32_t)chMask;
        }
        break;
      case 7:
        // for fixed bands:   all 125kHz channels ON, channel mask similar to ChMask = 4
        // except for CN470:  RFU
        if(this->band->bandType == RADIOLIB_LORAWAN_BAND_FIXED && this->band->bandNum != BandCN470) {
          chMaskGrp0123 = 0;
          chMaskGrp45 |= (uint32_t)chMask;
        }
        break;
    }
  }
  LoRaWANNode::hton<uint64_t>(&mAdrOpt[1], chMaskGrp0123);
  LoRaWANNode::hton<uint32_t>(&mAdrOpt[9], chMaskGrp45);
}

void LoRaWANNode::postprocessMacLinkAdr(uint8_t* ack, uint8_t cLen) {
  uint8_t fLen = 5;   // single ADR command is 5 bytes
  uint8_t numOpts = cLen / fLen;
  
  // duplicate the ACK bits of the atomic block response 'numOpts' times
  // skip one, as the first response is already there
  for(int opt = 1; opt < numOpts; opt++) {
    ack[opt*2 + 0] = RADIOLIB_LORAWAN_MAC_LINK_ADR;
    ack[opt*2 + 1] = ack[1];
  }
}

int16_t LoRaWANNode::getMacCommand(uint8_t cid, LoRaWANMacCommand_t* cmd) {
  for(size_t i = 0; i < RADIOLIB_LORAWAN_NUM_MAC_COMMANDS; i++) {
    if(MacTable[i].cid == cid) {
      memcpy(reinterpret_cast<void*>(cmd), reinterpret_cast<void*>(const_cast<LoRaWANMacCommand_t*>(&MacTable[i])), sizeof(LoRaWANMacCommand_t));
      return(RADIOLIB_ERR_NONE);
    }
  }
  // didn't find this CID, check if derived class can help (if any)
  int16_t state = this->derivedMacFinder(cid, cmd);
  return(state);
}

int16_t LoRaWANNode::derivedMacFinder(uint8_t cid, LoRaWANMacCommand_t* cmd) {
  (void)cid;
  (void)cmd;
  return(RADIOLIB_ERR_INVALID_CID);
}

int16_t LoRaWANNode::sendMacCommandReq(uint8_t cid) {
  LoRaWANMacCommand_t cmd = RADIOLIB_LORAWAN_MAC_COMMAND_NONE;
  int16_t state = this->getMacCommand(cid, &cmd);
  RADIOLIB_ASSERT(state);
  if(!cmd.user) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("You are not allowed to request this MAC command");
    return(RADIOLIB_ERR_INVALID_CID);
  }

  // if there are already 15 MAC bytes in the uplink queue, we can't add a new one
  if(fOptsUpLen >= RADIOLIB_LORAWAN_FHDR_FOPTS_MAX_LEN) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("The maximum size of FOpts payload was reached");
    return(RADIOLIB_ERR_COMMAND_QUEUE_FULL);
  }

  // if this MAC command is already in the queue, silently stop
  if(this->getMacPayload(cid, this->fOptsUp, this->fOptsUpLen, NULL, RADIOLIB_LORAWAN_UPLINK) == RADIOLIB_ERR_NONE) {
    return(RADIOLIB_ERR_NONE);
  }

  state = LoRaWANNode::pushMacCommand(cid, NULL, this->fOptsUp, &this->fOptsUpLen, RADIOLIB_LORAWAN_UPLINK);
  return(state);
}

int16_t LoRaWANNode::getMacLinkCheckAns(uint8_t* margin, uint8_t* gwCnt) {
  uint8_t payload[2] = { 0 };
  int16_t state = this->getMacPayload(RADIOLIB_LORAWAN_MAC_LINK_CHECK, this->fOptsDown, this->fOptsDownLen, payload, RADIOLIB_LORAWAN_DOWNLINK);
  RADIOLIB_ASSERT(state);

  if(margin) { *margin = payload[0]; }
  if(gwCnt)  { *gwCnt  = payload[1]; }

  return(RADIOLIB_ERR_NONE);
}

int16_t LoRaWANNode::getMacDeviceTimeAns(uint32_t* gpsEpoch, uint8_t* fraction, bool returnUnix) {
  uint8_t payload[5] = { 0 };
  int16_t state = this->getMacPayload(RADIOLIB_LORAWAN_MAC_DEVICE_TIME, this->fOptsDown, this->fOptsDownLen, payload, RADIOLIB_LORAWAN_DOWNLINK);
  RADIOLIB_ASSERT(state);

  if(gpsEpoch) { 
    *gpsEpoch = LoRaWANNode::ntoh<uint32_t>(&payload[0]); 
    if(returnUnix) {
      uint32_t unixOffset = 315964800UL - 18UL; // 18 leap seconds since GPS epoch (Jan. 6th 1980)
      *gpsEpoch += unixOffset;
    }
  }
  if(fraction) { *fraction = payload[4]; }

  return(RADIOLIB_ERR_NONE);
}

int16_t LoRaWANNode::getMacLen(uint8_t cid, uint8_t* len, uint8_t dir, bool inclusive, uint8_t* payload) {
  (void)payload;

  *len = 0;
  if(inclusive) {
    *len += 1;    // add one byte for CID
  }
  
  LoRaWANMacCommand_t cmd = RADIOLIB_LORAWAN_MAC_COMMAND_NONE;
  int16_t state = this->getMacCommand(cid, &cmd);
  RADIOLIB_ASSERT(state);
  if(dir == RADIOLIB_LORAWAN_UPLINK) {
    *len += cmd.lenUp;
  } else {
    *len += cmd.lenDn;
  }
  return(RADIOLIB_ERR_NONE);
}

bool LoRaWANNode::isPersistentMacCommand(uint8_t cid, uint8_t dir) {
  // if this MAC command doesn't exist, it wouldn't even get into the queue, so don't care about outcome
  LoRaWANMacCommand_t cmd = RADIOLIB_LORAWAN_MAC_COMMAND_NONE;
  (void)this->getMacCommand(cid, &cmd);
  
  // in the uplink direction, MAC payload should persist per spec
  if(dir == RADIOLIB_LORAWAN_UPLINK) {
    return(cmd.persist);

  // in the downlink direction, MAC payload should persist if it is user-accessible
  // which is the case for LinkCheck and DeviceTime
  } else {
    return(cmd.user);
  }
  return(false);
}

int16_t LoRaWANNode::pushMacCommand(uint8_t cid, const uint8_t* cOcts, uint8_t* out, uint8_t* lenOut, uint8_t dir) {
  uint8_t fLen = 0;
  int16_t state = this->getMacLen(cid, &fLen, dir, true);
  RADIOLIB_ASSERT(state);

  // check if we can even append the MAC command into the buffer
  if(*lenOut + fLen > RADIOLIB_LORAWAN_FHDR_FOPTS_MAX_LEN) {
    return(RADIOLIB_ERR_COMMAND_QUEUE_FULL);
  }

  out[*lenOut] = cid;                           // add MAC id
  memcpy(&out[*lenOut + 1], cOcts, fLen - 1);   // copy payload into buffer
  *lenOut += fLen;                              // payload + command ID

  return(RADIOLIB_ERR_NONE);
}

int16_t LoRaWANNode::getMacPayload(uint8_t cid, const uint8_t* in, uint8_t lenIn, uint8_t* out, uint8_t dir) {
  size_t i = 0;

  while(i < lenIn) {
    uint8_t id = in[i];
    uint8_t fLen = 0;
    int16_t state = this->getMacLen(id, &fLen, dir, true);
    RADIOLIB_ASSERT(state);
    if(lenIn < i + fLen) {
      return(RADIOLIB_ERR_INVALID_CID);
    }

    // if this is the requested MAC id, copy the payload over
    if(id == cid) {
      // only copy payload if destination is supplied
      if(out) {
        memcpy(out, &in[i + 1], fLen - 1);
      }
      return(RADIOLIB_ERR_NONE);
    }

    // move on to next MAC command
    i += fLen;
  }

  return(RADIOLIB_ERR_COMMAND_QUEUE_ITEM_NOT_FOUND);
}

int16_t LoRaWANNode::deleteMacCommand(uint8_t cid, uint8_t* inOut, uint8_t* lenInOut, uint8_t dir) {
  size_t i = 0;
  while(i < *lenInOut) {
    uint8_t id = inOut[i];
    uint8_t fLen = 0;
    int16_t state = this->getMacLen(id, &fLen, dir);
    RADIOLIB_ASSERT(state);
    if(*lenInOut < i + fLen) {
      return(RADIOLIB_ERR_INVALID_CID);
    }

    // if this is the requested MAC id, 
    if(id == cid) {
      // remove it by moving the rest of the payload forward
      memmove(&inOut[i], &inOut[i + fLen], *lenInOut - i - fLen);

      // set the remainder of the queue to 0
      memset(&inOut[i + fLen], 0, *lenInOut - i - fLen);

      *lenInOut -= fLen;
      return(RADIOLIB_ERR_NONE);
    }

    // move on to next MAC command
    i += fLen;
  }

  return(RADIOLIB_ERR_COMMAND_QUEUE_ITEM_NOT_FOUND);
}

void LoRaWANNode::clearMacCommands(uint8_t* inOut, uint8_t* lenInOut, uint8_t dir) {
  size_t i = 0;
  uint8_t numDeleted = 0;
  while(i < *lenInOut) {
    uint8_t id = inOut[i];
    uint8_t fLen = 0;
    // include CID byte, so if command fails, we still move one byte forward
    (void)this->getMacLen(id, &fLen, dir, true);

    // only clear MAC command if it should not persist until a downlink is received
    if(!this->isPersistentMacCommand(id, dir)) {
      // remove it by moving the rest of the payload forward
      memmove(&inOut[i], &inOut[i + fLen], *lenInOut - i - fLen);

      // set the remainder of the queue to 0
      memset(&inOut[i + fLen], 0, *lenInOut - i - fLen);

      numDeleted += fLen;
    }

    // move on to next MAC command
    i += fLen;
  }
  *lenInOut -= numDeleted;
}

int16_t LoRaWANNode::setDatarate(uint8_t drUp) {
  // scan through all enabled channels and check if the requested datarate is available
  bool isValidDR = false;
  for(size_t i = 0; i < RADIOLIB_LORAWAN_NUM_AVAILABLE_CHANNELS; i++) {
    const LoRaWANChannel_t *chnl = &(this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i]);
    if(chnl->enabled) {
      if(drUp >= chnl->drMin && drUp <= chnl->drMax) {
        isValidDR = true;
        break;
      }
    }
  }
  if(!isValidDR) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("No defined channel allows datarate %d", drUp);
    return(RADIOLIB_ERR_INVALID_DATA_RATE);
  }

  uint8_t cOcts[1];
  uint8_t cAck[1];
  uint8_t cid = RADIOLIB_LORAWAN_MAC_LINK_ADR;
  uint8_t cLen = 1;                       // only apply Dr/Tx field
  cOcts[0]  = (drUp << 4);                // set requested datarate
  cOcts[0] |= 0x0F;                       // keep Tx Power the same
  (void)execMacCommand(cid, cOcts, cLen, cAck);

  // check if ACK is set for Datarate
  if(!(cAck[0] & 0x02)) {
    return(RADIOLIB_ERR_INVALID_DATA_RATE);
  }
  
  return(RADIOLIB_ERR_NONE);
}

int16_t LoRaWANNode::setTxPower(int8_t txPower) {
  // only allow values within the band's (or MAC state) maximum
  if(txPower > this->txPowerMax) {
    return(RADIOLIB_ERR_INVALID_OUTPUT_POWER);
  }
  // Tx Power is set in steps of two
  // the selected value is rounded down to nearest multiple of two away from txPowerMax
  // e.g. on EU868, max is 16; if 13 is selected then we set to 12
  uint8_t numSteps = (this->txPowerMax - txPower + 1) / (-RADIOLIB_LORAWAN_POWER_STEP_SIZE_DBM);

  uint8_t cOcts[1];
  uint8_t cAck[1];
  uint8_t cid = RADIOLIB_LORAWAN_MAC_LINK_ADR;
  uint8_t cLen = 1;                       // only apply Dr/Tx field
  cOcts[0]  = 0xF0;                       // keep datarate the same
  cOcts[0] |= numSteps;                   // set requested Tx Power
  (void)execMacCommand(cid, cOcts, cLen, cAck);

  // check if ACK is set for Tx Power
  if(!(cAck[0] & 0x04)) {
    return(RADIOLIB_ERR_INVALID_OUTPUT_POWER);
  }
  
  return(RADIOLIB_ERR_NONE);
}

int16_t LoRaWANNode::setRx2Dr(uint8_t dr) {
  // this can only be configured in ABP mode
  if(this->lwMode != RADIOLIB_LORAWAN_MODE_ABP) {
    return(RADIOLIB_ERR_INVALID_MODE);
  }

  // can only configure different datarate for dynamic bands
  if(this->band->bandType == RADIOLIB_LORAWAN_BAND_FIXED) {
    return(RADIOLIB_ERR_NO_CHANNEL_AVAILABLE);
  }

  // check if datarate is available in the selected band
  if(this->band->dataRates[dr] == RADIOLIB_LORAWAN_DATA_RATE_UNUSED) {
    return(RADIOLIB_ERR_INVALID_DATA_RATE);
  }
  
  // find and check if the datarate is available for this radio module
  DataRate_t dataRate;
  int16_t state = findDataRate(dr, &dataRate);
  RADIOLIB_ASSERT(state);

  // passed all checks, so configure the datarate
  this->channels[RADIOLIB_LORAWAN_RX2].dr = dr;
  this->channels[RADIOLIB_LORAWAN_RX_BC].dr = dr;

  return(state);
}

void LoRaWANNode::setADR(bool enable) {
  this->adrEnabled = enable;
}

void LoRaWANNode::setDutyCycle(bool enable, RadioLibTime_t msPerHour) {
  this->dutyCycleEnabled = enable;
  if(!enable) {
    this->dutyCycle = 0;
    return;
  }
  if(msPerHour == 0) {
    this->dutyCycle = this->band->dutyCycle;
  } else {
    this->dutyCycle = msPerHour;
  }
}

void LoRaWANNode::setDwellTime(bool enable, RadioLibTime_t msPerUplink) {
  if(!enable) {
    this->dwellTimeUp = 0;
    
  } else if(msPerUplink > 0) {
    this->dwellTimeUp = msPerUplink;
  } else {  //msPerUplink == 0
    this->dwellTimeUp = this->band->dwellTimeUp;
  }
}

// A user may enable CSMA to provide frames an additional layer of protection from interference.
// https://resources.lora-alliance.org/technical-recommendations/tr013-1-0-0-csma
void LoRaWANNode::setCSMA(bool csmaEnabled, uint8_t maxChanges, uint8_t backoffMax, uint8_t difsSlots) {
  this->csmaEnabled = csmaEnabled;
  if(csmaEnabled) {
    this->maxChanges = maxChanges;
    this->difsSlots = difsSlots;
    this->backoffMax = backoffMax;
  } else {
    // disable all values
    this->maxChanges = 0;
    this->difsSlots = 0;
    this->backoffMax = 0;
  }
}

void LoRaWANNode::setDeviceStatus(uint8_t battLevel) {
  this->battLevel = battLevel;
}

void LoRaWANNode::scheduleTransmission(RadioLibTime_t tUplink) {
  this->tUplink = tUplink;
}

// return fCnt of last uplink; also return 0 if no uplink occured yet
uint32_t LoRaWANNode::getFCntUp() {
  if(this->fCntUp == 0) {
    return(0);
  }
  return(this->fCntUp - 1);
}

uint32_t LoRaWANNode::getNFCntDown() {
  return(this->nFCntDown);
}

uint32_t LoRaWANNode::getAFCntDown() {
  return(this->aFCntDown);
}

uint32_t LoRaWANNode::getDevAddr() {
  return(this->devAddr);
}

RadioLibTime_t LoRaWANNode::getLastToA() {
  return(this->lastToA);
}

int16_t LoRaWANNode::setPhyProperties(const LoRaWANChannel_t* chnl, uint8_t dir, int8_t pwr, size_t pre) {
  int16_t state = RADIOLIB_ERR_NONE;

  // get the currently configured modem from the radio
  ModemType_t modem;
  state = this->phyLayer->getModem(&modem);
  RADIOLIB_ASSERT(state);

  // set modem-dependent functions
  switch(this->band->dataRates[chnl->dr] & RADIOLIB_LORAWAN_DATA_RATE_MODEM) {
    case(RADIOLIB_LORAWAN_DATA_RATE_LORA):
      if(modem != ModemType_t::RADIOLIB_MODEM_LORA) {
        state = this->phyLayer->setModem(ModemType_t::RADIOLIB_MODEM_LORA);
        RADIOLIB_ASSERT(state);
      }
      modem = ModemType_t::RADIOLIB_MODEM_LORA;
      // downlink messages are sent with inverted IQ
      if(dir == RADIOLIB_LORAWAN_DOWNLINK) {
        state = this->phyLayer->invertIQ(true);
      } else {
        state = this->phyLayer->invertIQ(false);
      }
      RADIOLIB_ASSERT(state);
      break;
    
    case(RADIOLIB_LORAWAN_DATA_RATE_FSK):
      if(modem != ModemType_t::RADIOLIB_MODEM_FSK) {
        state = this->phyLayer->setModem(ModemType_t::RADIOLIB_MODEM_FSK);
        RADIOLIB_ASSERT(state);
      }
      modem = ModemType_t::RADIOLIB_MODEM_FSK;
      state = this->phyLayer->setDataShaping(RADIOLIB_SHAPING_1_0);
      RADIOLIB_ASSERT(state);
      state = this->phyLayer->setEncoding(RADIOLIB_ENCODING_WHITENING);
      RADIOLIB_ASSERT(state);
      break;
    
    case(RADIOLIB_LORAWAN_DATA_RATE_LR_FHSS):
      if(modem != ModemType_t::RADIOLIB_MODEM_LRFHSS) {
        state = this->phyLayer->setModem(ModemType_t::RADIOLIB_MODEM_LRFHSS);
        RADIOLIB_ASSERT(state);
      }
      modem = ModemType_t::RADIOLIB_MODEM_LRFHSS;
      break;
    
    default:
      return(RADIOLIB_ERR_UNSUPPORTED);
  }

  RADIOLIB_DEBUG_PROTOCOL_PRINTLN("");
  RADIOLIB_DEBUG_PROTOCOL_PRINTLN("PHY:  Frequency = %7.3f MHz, TX = %d dBm", chnl->freq / 10000.0, pwr);
  state = this->phyLayer->setFrequency(chnl->freq / 10000.0);
  RADIOLIB_ASSERT(state);
  
  // at this point, assume that Tx power value is already checked, so ignore the return value
  // this call is only used to clip a value that is higher than the module supports
  (void)this->phyLayer->checkOutputPower(pwr, &pwr);
  state = this->phyLayer->setOutputPower(pwr);
  RADIOLIB_ASSERT(state);

  DataRate_t dr;
  state = findDataRate(chnl->dr, &dr);
  RADIOLIB_ASSERT(state);
  state = this->phyLayer->setDataRate(dr);
  RADIOLIB_ASSERT(state);

  // this only needs to be done once-ish
  uint8_t syncWord[4] = { 0 };
  uint8_t syncWordLen = 0;
  size_t preLen = 0;
  switch(modem) {
    case(ModemType_t::RADIOLIB_MODEM_FSK): {
      preLen = 8*RADIOLIB_LORAWAN_GFSK_PREAMBLE_LEN;
      syncWord[0] = (uint8_t)(RADIOLIB_LORAWAN_GFSK_SYNC_WORD >> 16);
      syncWord[1] = (uint8_t)(RADIOLIB_LORAWAN_GFSK_SYNC_WORD >> 8);
      syncWord[2] = (uint8_t)RADIOLIB_LORAWAN_GFSK_SYNC_WORD;
      syncWordLen = 3;
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("FSK:  BR = %4.1f, FD = %4.1f kHz", 
                                      (double)dr.fsk.bitRate, (double)dr.fsk.freqDev);
    } break;

    case(ModemType_t::RADIOLIB_MODEM_LORA): {
      preLen = RADIOLIB_LORAWAN_LORA_PREAMBLE_LEN;
      syncWord[0] = RADIOLIB_LORAWAN_LORA_SYNC_WORD;
      syncWordLen = 1;
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("LoRa: SF = %d, BW = %5.1f kHz, CR = 4/%d, IQ: %c", 
                                    dr.lora.spreadingFactor, (double)dr.lora.bandwidth, dr.lora.codingRate, dir ? 'D' : 'U');
    } break;

    case(ModemType_t::RADIOLIB_MODEM_LRFHSS): {
      syncWord[0] = (uint8_t)(RADIOLIB_LORAWAN_LR_FHSS_SYNC_WORD >> 24);
      syncWord[1] = (uint8_t)(RADIOLIB_LORAWAN_LR_FHSS_SYNC_WORD >> 16);
      syncWord[2] = (uint8_t)(RADIOLIB_LORAWAN_LR_FHSS_SYNC_WORD >> 8);
      syncWord[3] = (uint8_t)RADIOLIB_LORAWAN_LR_FHSS_SYNC_WORD;
      syncWordLen = 4;
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("LR-FHSS: BW = 0x%02x, CR = 0x%02x kHz, grid = %c", 
                                    dr.lrFhss.bw, dr.lrFhss.cr, dr.lrFhss.narrowGrid ? 'N' : 'W');
    } break;

    default:
      return(RADIOLIB_ERR_WRONG_MODEM);
  }

  state = this->phyLayer->setSyncWord(syncWord, syncWordLen);
  RADIOLIB_ASSERT(state);

  // if a preamble length is supplied, overrule the 'calculated' preamble length
  if(pre) {
    preLen = pre;
  }
  if(modem != ModemType_t::RADIOLIB_MODEM_LRFHSS) {
    state = this->phyLayer->setPreambleLength(preLen);
  }
  return(state);
}

// The following function implements LMAC, a CSMA scheme for LoRa as specified 
// in the LoRa Alliance Technical Recommendation #13.
bool LoRaWANNode::csmaChannelClear(uint8_t difs, uint8_t numBackoff) {
  // DIFS phase: perform #DIFS CAD operations
  uint16_t numCads = 0;
  for (; numCads < difs; numCads++) {
    if (!this->cadChannelClear()) {
      return(false);
    }
  }

  // BO phase: perform #numBackoff additional CAD operations
  for (; numCads < difs + numBackoff; numCads++) {
    if (!this->cadChannelClear()) {
      return(false);
    }
  }

  // none of the CADs showed activity, so all clear
  return(true);
}

bool LoRaWANNode::cadChannelClear() {
  int16_t state = this->phyLayer->scanChannel();
  // if activity was detected, channel is not clear
  if ((state == RADIOLIB_PREAMBLE_DETECTED) || (state == RADIOLIB_LORA_DETECTED)) {
    return(false);
  }
  return(true);
}

void LoRaWANNode::addDefaultChannels() {
  // get current channel masks
  uint64_t chMaskGrp0123 = 0;
  uint32_t chMaskGrp45 = 0;
  this->getChannelPlanMask(&chMaskGrp0123, &chMaskGrp45);

  // there are at most three default channels for dynamic bands
  if(this->band->bandType == RADIOLIB_LORAWAN_BAND_DYNAMIC) {
    for(int num = 0; num < 3; num++) {
      if(this->band->txFreqs[num].enabled) {
        this->channelPlan[RADIOLIB_LORAWAN_UPLINK][num] = this->band->txFreqs[num];
        this->channelPlan[RADIOLIB_LORAWAN_DOWNLINK][num] = this->band->txFreqs[num];
        chMaskGrp0123 |= ((uint64_t)1 << num);
      }
    }

  } else {    // bandType == RADIOLIB_LORAWAN_BAND_FIXED
    // if a subband is set, we can set the channel indices straight from subband
    if(this->subBand > 0 && this->subBand <= 8) {
      // for sub band 1-8, set bank of 8 125kHz + single 500kHz channel
      chMaskGrp0123 |= (uint64_t)0xFF << ((this->subBand - 1) * 8);
      chMaskGrp45 |= (uint32_t)0x01 << (this->subBand - 1);
    } else if(this->subBand > 8 && this->subBand <= 12) {
      // CN470 only: for sub band 9-12, set bank of 8 125kHz channels
      chMaskGrp45 |= (uint32_t)0xFF << ((this->subBand - 9) * 8);
    } else {
      // if subband is set to 0, all 125kHz channels are enabled.
      // however, we can 'only' store 16 channels, so we don't use all channels at once.
      // instead, we select a random channel from each bank of 8 channels + 1 from second plan.
      chMaskGrp0123 = 0;
      chMaskGrp45 = 0;
      uint8_t num125kHz = this->band->txSpans[0].numChannels;
      uint8_t numBanks = num125kHz / 8;
      uint8_t bankIdx = this->phyLayer->random(8);
      for(uint8_t bank = 0; bank < numBanks; bank++) {
        uint8_t idx = bank * 8 + bankIdx;
        if(idx < 64) {
          chMaskGrp0123 |= ((uint64_t)1 << idx);
        } else {
          chMaskGrp45 |= ((uint32_t)1 << (idx - 64));
        }
      }
      // the 500 kHz channels are in the usual channel plan however
      // these are the channel indices 64-71 for bands other than CN470
      if(this->band->bandNum != BandCN470) {
        chMaskGrp45 |= ((uint32_t)1 << bankIdx);
      }
    }
  }

  // apply channel mask
  this->applyChannelMask(chMaskGrp0123, chMaskGrp45);

  // make sure the Rx2 settings are back to this band's default
  this->channels[RADIOLIB_LORAWAN_RX2] = this->band->rx2;
  this->channels[RADIOLIB_LORAWAN_RX_BC] = this->band->rx2;
  
  // make all enabled channels available for uplink selection
  this->setAvailableChannels(0xFFFF);
}

void LoRaWANNode::getChannelPlanMask(uint64_t* chMaskGrp0123, uint32_t* chMaskGrp45) {
  // clear masks in case anything was set
  *chMaskGrp0123 = 0;
  *chMaskGrp45 = 0;

  // if there are any channels selected, create the mask from those channels
  if(this->isActivated()) {
    for(int i = 0; i < RADIOLIB_LORAWAN_NUM_AVAILABLE_CHANNELS; i++) {
      uint8_t idx = this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].idx;
      if(idx != RADIOLIB_LORAWAN_CHANNEL_INDEX_NONE) {
        if(idx < 64) {
          *chMaskGrp0123 |= ((uint64_t)1 << idx);
        } else {
          *chMaskGrp45 |= ((uint32_t)1 << (idx - 64));
        }
      }
    }
  }
}

uint8_t LoRaWANNode::getAvailableChannels(uint16_t* chMask) {
  uint8_t num = 0;
  uint16_t mask = 0;
  uint8_t currentDr = this->channels[RADIOLIB_LORAWAN_UPLINK].dr;
  for(uint8_t i = 0; i < RADIOLIB_LORAWAN_NUM_AVAILABLE_CHANNELS; i++) {
    // if channel is available and usable for current datarate, set corresponding bit
    if(this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].available) {
      if(currentDr >= this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].drMin &&
         currentDr <= this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].drMax) {
        num++;
        mask |= (0x0001 << i);
      }
    }
  }
  if(chMask) {
    *chMask = mask;
  }
  return(num);
}

void LoRaWANNode::setAvailableChannels(uint16_t mask) {
  for(uint8_t i = 0; i < RADIOLIB_LORAWAN_NUM_AVAILABLE_CHANNELS; i++) {
    // if channel is enabled, set to available
    if(mask & (0x0001 << i) && this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].enabled) {
      this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].available = true;
    } else {
      this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].available = false;
    }
  }
}

int16_t LoRaWANNode::selectChannels() {
  uint16_t chMask = 0x0000;
  uint8_t numChannels = this->getAvailableChannels(&chMask);

  // if there are no available channels, try resetting them all to available
  if(numChannels == 0) {
    this->setAvailableChannels(0xFFFF);
    numChannels = this->getAvailableChannels(&chMask);

    // if there are still no channels available, give up
    if(numChannels == 0) {
      return(RADIOLIB_ERR_NO_CHANNEL_AVAILABLE);
    }
  }

  // select a random value within the number of possible channels
  int chRand = this->phyLayer->random(numChannels);

  // retrieve the index of this channel by looping through the channel mask
  int chIdx = -1;
  while(chRand >= 0) {
    chIdx++;
    if(chMask & 0x0001) {
      chRand--;
    }
    chMask >>= 1;
  }

  // as we are now going to use this channel, mark unavailable for next uplink
  this->channelPlan[RADIOLIB_LORAWAN_UPLINK][chIdx].available = false;

  uint8_t currentDr = this->channels[RADIOLIB_LORAWAN_UPLINK].dr;
  this->channels[RADIOLIB_LORAWAN_UPLINK] = this->channelPlan[RADIOLIB_LORAWAN_UPLINK][chIdx];
  this->channels[RADIOLIB_LORAWAN_UPLINK].dr = currentDr;
  
  if(this->band->bandType == RADIOLIB_LORAWAN_BAND_DYNAMIC) {
    // for dynamic bands, the downlink channel is the one matched to the uplink channel
    this->channels[RADIOLIB_LORAWAN_RX1] = this->channelPlan[RADIOLIB_LORAWAN_DOWNLINK][chIdx];
  
  } else {                // RADIOLIB_LORAWAN_BAND_FIXED
    // for fixed bands, the downlink channel is the uplink channel ID `modulo` number of downlink channels
    LoRaWANChannel_t channelDn = RADIOLIB_LORAWAN_CHANNEL_NONE;
    channelDn.enabled = true;
    channelDn.idx = this->channels[RADIOLIB_LORAWAN_UPLINK].idx % this->band->rx1Span.numChannels;
    channelDn.freq = this->band->rx1Span.freqStart + channelDn.idx*this->band->rx1Span.freqStep;
    channelDn.drMin = this->band->rx1Span.drMin;
    channelDn.drMax = this->band->rx1Span.drMax;
    this->channels[RADIOLIB_LORAWAN_RX1] = channelDn;

  }
  uint8_t rx1Dr = this->band->rx1DrTable[currentDr][this->rx1DrOffset];

  // if downlink dwelltime is enabled, datarate < 2 cannot be used, so clip to 2
  // only in use on AS923_x bands
  if(this->dwellTimeDn && rx1Dr < 2) {
    rx1Dr = 2;
  }
  this->channels[RADIOLIB_LORAWAN_RX1].dr = rx1Dr;

  return(RADIOLIB_ERR_NONE);
}

bool LoRaWANNode::applyChannelMask(uint64_t chMaskGrp0123, uint32_t chMaskGrp45) {
  if(this->band->bandType == RADIOLIB_LORAWAN_BAND_DYNAMIC) {
    for(int i = 0; i < RADIOLIB_LORAWAN_NUM_AVAILABLE_CHANNELS; i++) {
      bool en = chMaskGrp0123 & ((uint64_t)1 << i);
      // if it should be enabled but is not currently defined, stop immediately
      if(en && this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].idx == RADIOLIB_LORAWAN_CHANNEL_INDEX_NONE) {
        return(false);
      }
      this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].enabled = en;
    }
  } else {    // bandType == RADIOLIB_LORAWAN_BAND_FIXED
    // full channel mask received, so clear all existing channels
    LoRaWANChannel_t chnl = RADIOLIB_LORAWAN_CHANNEL_NONE;
    for(size_t i = 0; i < RADIOLIB_LORAWAN_NUM_AVAILABLE_CHANNELS; i++) {
      this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i] = chnl;
    }
    int num = 0;
    uint8_t spanNum = 0;
    int chNum = 0;
    int chOfs = 0;
    for(; chNum < 64; chNum++) {
      if(chMaskGrp0123 & ((uint64_t)1 << chNum)) {
        // if a subband is specified, any channel must be within the specified subband
        // we subtract 1 here because subbands are numbered starting from 1
        if(this->subBand > 0 && (chNum / 8) != (this->subBand - 1)) {
          RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Channel %d not allowed for subband %d", chNum, this->subBand);
          return(false);
        }
        chnl.enabled = true;
        chnl.idx   = chNum;
        chnl.freq  = this->band->txSpans[spanNum].freqStart + chNum*this->band->txSpans[spanNum].freqStep;
        chnl.drMin = this->band->txSpans[spanNum].drMin;
        chnl.drMax = this->band->txSpans[spanNum].drMax;
        // set the default datarate to the middle datarate, rounded up
        chnl.dr = (chnl.drMin + chnl.drMax + 1) / 2;
        this->channelPlan[RADIOLIB_LORAWAN_UPLINK][num++] = chnl;
      }
    }
    if(this->band->numTxSpans > 1) {
      spanNum += 1;
      chNum = 0;
      chOfs = 64;
    }
    for(; chNum < this->band->txSpans[spanNum].numChannels; chNum++) {
      if(chMaskGrp45 & ((uint32_t)1 << chNum)) {
        // if a subband is specified, any channel must be within the specified subband
        // we subtract 1 here because subbands are numbered starting from 1
        if(this->band->numTxSpans == 1 && this->subBand > 0 && (chNum / 8) != (this->subBand - 1)) {
          RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Channel %d not allowed for subband %d", chNum, this->subBand);
          return(false);
        }
        if(this->band->numTxSpans > 1 && this->subBand > 0 && chNum != (this->subBand - 1)) {
          RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Channel %d not allowed for subband %d", chNum, this->subBand);
          return(false);
        }
        chnl.enabled = true;
        chnl.idx   = chNum + chOfs;
        chnl.freq  = this->band->txSpans[spanNum].freqStart + chNum*this->band->txSpans[spanNum].freqStep;
        chnl.drMin = this->band->txSpans[spanNum].drMin;
        chnl.drMax = this->band->txSpans[spanNum].drMax;
        this->channelPlan[RADIOLIB_LORAWAN_UPLINK][num++] = chnl;
      }
    }
  }

#if RADIOLIB_DEBUG_PROTOCOL
  this->printChannels();
#endif

  return(true);
}

#if RADIOLIB_DEBUG_PROTOCOL
void LoRaWANNode::printChannels() {
  for (int i = 0; i < RADIOLIB_LORAWAN_NUM_AVAILABLE_CHANNELS; i++) {
    if(this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].enabled) {
      RADIOLIB_DEBUG_PROTOCOL_PRINTLN("UL: %3d %d %7.3f (%d - %d) | DL: %3d %d %7.3f (%d - %d)",
                              this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].idx,
                              this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].enabled,
                              this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].freq / 10000.0,
                              this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].drMin,
                              this->channelPlan[RADIOLIB_LORAWAN_UPLINK][i].drMax,

                              this->channelPlan[RADIOLIB_LORAWAN_DOWNLINK][i].idx,
                              this->channelPlan[RADIOLIB_LORAWAN_DOWNLINK][i].enabled,
                              this->channelPlan[RADIOLIB_LORAWAN_DOWNLINK][i].freq / 10000.0,
                              this->channelPlan[RADIOLIB_LORAWAN_DOWNLINK][i].drMin,
                              this->channelPlan[RADIOLIB_LORAWAN_DOWNLINK][i].drMax
                            );
    }
  }
}
#endif

uint32_t LoRaWANNode::generateMIC(const uint8_t* msg, size_t len, uint8_t* key) {
  if((msg == NULL) || (len == 0)) {
    return(0);
  }

  RadioLibAES128Instance.init(key);
  uint8_t cmac[RADIOLIB_AES128_BLOCK_SIZE];
  RadioLibAES128Instance.generateCMAC(msg, len, cmac);
  return(((uint32_t)cmac[0]) | ((uint32_t)cmac[1] << 8) | ((uint32_t)cmac[2] << 16) | ((uint32_t)cmac[3]) << 24);
}

bool LoRaWANNode::verifyMIC(uint8_t* msg, size_t len, uint8_t* key) {
  if((msg == NULL) || (len < sizeof(uint32_t))) {
    return(0);
  }

  // extract MIC from the message
  uint32_t micReceived = LoRaWANNode::ntoh<uint32_t>(&msg[len - sizeof(uint32_t)]);

  // calculate the expected value and compare
  uint32_t micCalculated = generateMIC(msg, len - sizeof(uint32_t), key);
  if(micCalculated != micReceived) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("MIC mismatch, expected %08lx, got %08lx", 
                                    (unsigned long)micCalculated, (unsigned long)micReceived);
    return(false);
  }

  return(true);
}

// given an airtime in milliseconds, calculate the minimum uplink interval
// to adhere to a given dutyCycle
RadioLibTime_t LoRaWANNode::dutyCycleInterval(RadioLibTime_t msPerHour, RadioLibTime_t airtime) {
  if(msPerHour == 0 || airtime == 0) {
    return(0);
  }
  RadioLibTime_t oneHourInMs = (RadioLibTime_t)60 * (RadioLibTime_t)60 * (RadioLibTime_t)1000;
  float numPackets = msPerHour / airtime;
  RadioLibTime_t delayMs = oneHourInMs / numPackets + 1;  // + 1 to prevent rounding problems
  return(delayMs);
}

RadioLibTime_t LoRaWANNode::timeUntilUplink() {
  Module* mod = this->phyLayer->getMod();
  RadioLibTime_t nextUplink = this->rxDelayStart + dutyCycleInterval(this->dutyCycle, this->lastToA);
  if(mod->hal->millis() > nextUplink){
    return(0);
  }
  return(nextUplink - mod->hal->millis() + 1);
}

uint8_t LoRaWANNode::getMaxPayloadLen() {
  // configure the uplink channel properties
  this->setPhyProperties(&this->channels[RADIOLIB_LORAWAN_UPLINK], 
                         RADIOLIB_LORAWAN_UPLINK,
                         this->txPowerMax - 2*this->txPowerSteps);

  uint8_t minLen = 0;
  uint8_t maxLen = this->band->payloadLenMax[this->channels[RADIOLIB_LORAWAN_UPLINK].dr];
  if(this->packages[RADIOLIB_LORAWAN_PACKAGE_TS011].enabled) {
    maxLen = RADIOLIB_MIN(maxLen, 222); // payload length is limited to N=222 if under repeater
  }
  maxLen += 13;                         // mandatory FHDR is 12/13 bytes

  // if not limited by dwell-time, just return maximum
  if(!this->dwellTimeUp) {
    // subtract FHDR (13 bytes) as well as any FOpts
    return(maxLen - 13 - this->fOptsUpLen);
  }

  // fast exit in case upper limit is already good
  if(this->phyLayer->getTimeOnAir(maxLen) / 1000 <= this->dwellTimeUp) {
    // subtract FHDR (13 bytes) as well as any FOpts
    return(maxLen - 13 - this->fOptsUpLen);
  }

  // do some binary search to find maximum allowed length
  uint8_t curLen = (minLen + maxLen) / 2;
  while(curLen != minLen && curLen != maxLen) {
    if(this->phyLayer->getTimeOnAir(curLen) / 1000 > this->dwellTimeUp) {
      maxLen = curLen;
    } else {
      minLen = curLen;
    }
    curLen = (minLen + maxLen) / 2;
  }
  // subtract FHDR (13 bytes) as well as any FOpts
  return(curLen - 13 - this->fOptsUpLen);
}

void LoRaWANNode::setSleepFunction(SleepCb_t cb) {
  this->sleepCb = cb;
}

int16_t LoRaWANNode::addAppPackage(uint8_t packageId, PackageCb_t callback) {
  if(packageId >= RADIOLIB_LORAWAN_NUM_SUPPORTED_PACKAGES) {
    return(RADIOLIB_ERR_INVALID_MODE);
  }
  return(this->addAppPackage(packageId, callback, PackageTable[packageId].packFPort));
}

int16_t LoRaWANNode::addAppPackage(uint8_t packageId, PackageCb_t callback, uint8_t fPort) {
  if(packageId >= RADIOLIB_LORAWAN_NUM_SUPPORTED_PACKAGES) {
    return(RADIOLIB_ERR_INVALID_MODE);
  }
  if(PackageTable[packageId].isAppPack == false) {
    return(RADIOLIB_ERR_INVALID_MODE);
  }
  if(PackageTable[packageId].fixedFPort && fPort != PackageTable[packageId].packFPort) {
    return(RADIOLIB_ERR_INVALID_PORT);
  }
  if(callback == NULL) {
    return(RADIOLIB_ERR_NULL_POINTER);
  }
  this->packages[packageId] = PackageTable[packageId];
  this->packages[packageId].packFPort = fPort;
  this->packages[packageId].callback = callback;
  this->packages[packageId].enabled = true;
  return(RADIOLIB_ERR_NONE);
}

int16_t LoRaWANNode::addNwkPackage(uint8_t packageId, PackageCb_t callback) {
  if(packageId >= RADIOLIB_LORAWAN_NUM_SUPPORTED_PACKAGES) {
    return(RADIOLIB_ERR_INVALID_MODE);
  }
  if(PackageTable[packageId].isAppPack == true) {
    return(RADIOLIB_ERR_INVALID_MODE);
  }
  if(callback == NULL) {
    return(RADIOLIB_ERR_NULL_POINTER);
  }
  this->packages[packageId] = PackageTable[packageId];
  this->packages[packageId].callback = callback;
  this->packages[packageId].enabled = true;
  return(RADIOLIB_ERR_NONE);
}

void LoRaWANNode::removePackage(uint8_t packageId) {
  // silently ignore, assume that the user supplies decent index
  if(packageId >= RADIOLIB_LORAWAN_NUM_SUPPORTED_PACKAGES) {
    return;
  }
  this->packages[packageId].enabled = false;
  return;
}

int16_t LoRaWANNode::findDataRate(uint8_t dr, DataRate_t* dataRate) {
  int16_t state = RADIOLIB_ERR_NONE;

  ModemType_t modemNew;

  uint8_t dataRateBand = this->band->dataRates[dr];

  switch(dataRateBand & RADIOLIB_LORAWAN_DATA_RATE_MODEM) {
    case(RADIOLIB_LORAWAN_DATA_RATE_LORA):
      modemNew = ModemType_t::RADIOLIB_MODEM_LORA;
      dataRate->lora.spreadingFactor = ((dataRateBand & RADIOLIB_LORAWAN_DATA_RATE_SF) >> 3) + 7;
      switch(dataRateBand & RADIOLIB_LORAWAN_DATA_RATE_BW) {
        case(RADIOLIB_LORAWAN_DATA_RATE_BW_125_KHZ):
          dataRate->lora.bandwidth = 125.0;
          break;
        case(RADIOLIB_LORAWAN_DATA_RATE_BW_250_KHZ):
          dataRate->lora.bandwidth = 250.0;
          break;
        case(RADIOLIB_LORAWAN_DATA_RATE_BW_500_KHZ):
          dataRate->lora.bandwidth = 500.0;
          break;
        default:
          return(RADIOLIB_ERR_UNSUPPORTED);
      }
      dataRate->lora.codingRate = 5;
      break;
    case(RADIOLIB_LORAWAN_DATA_RATE_FSK):
      modemNew = ModemType_t::RADIOLIB_MODEM_FSK;
      dataRate->fsk.bitRate = 50;
      dataRate->fsk.freqDev = 25;
      break;
    case(RADIOLIB_LORAWAN_DATA_RATE_LR_FHSS):
      modemNew = ModemType_t::RADIOLIB_MODEM_LRFHSS;
      switch(dataRateBand & RADIOLIB_LORAWAN_DATA_RATE_BW) {
        case(RADIOLIB_LORAWAN_DATA_RATE_BW_137_KHZ):
          dataRate->lrFhss.bw = 0x02; // specific encoding
          dataRate->lrFhss.narrowGrid = 1;
          break;
        case(RADIOLIB_LORAWAN_DATA_RATE_BW_336_KHZ):
          dataRate->lrFhss.bw = 0x04; // specific encoding
          dataRate->lrFhss.narrowGrid = 1;
          break;
        case(RADIOLIB_LORAWAN_DATA_RATE_BW_1523_KHZ):
          dataRate->lrFhss.bw = 0x08; // specific encoding
          dataRate->lrFhss.narrowGrid = 0;
          break;
        default:
          return(RADIOLIB_ERR_UNSUPPORTED);
      }
      switch(dataRateBand & RADIOLIB_LORAWAN_DATA_RATE_CR) {
        case(RADIOLIB_LORAWAN_DATA_RATE_CR_1_3):
          dataRate->lrFhss.cr = 0x03;
          break;
        case(RADIOLIB_LORAWAN_DATA_RATE_CR_2_3):
          dataRate->lrFhss.cr = 0x01;
          break;
        default:
          return(RADIOLIB_ERR_UNSUPPORTED);
      }
      break;
    default:
      return(RADIOLIB_ERR_UNSUPPORTED);
  }

  // get the currently configured modem from the radio
  ModemType_t modemCurrent;
  state = this->phyLayer->getModem(&modemCurrent);
  RADIOLIB_ASSERT(state);

  // if the required modem is different than the current one, change over
  if(modemNew != modemCurrent) {
    state = this->phyLayer->standby();
    RADIOLIB_ASSERT(state);
    state = this->phyLayer->setModem(modemNew);
    RADIOLIB_ASSERT(state);
  }

  state = this->phyLayer->checkDataRate(*dataRate);
  return(state);
}

void LoRaWANNode::processAES(const uint8_t* in, size_t len, uint8_t* key, uint8_t* out, uint32_t addr, uint32_t fCnt, uint8_t dir, uint8_t ctrId, bool counter) {
  // figure out how many encryption blocks are there
  size_t numBlocks = len/RADIOLIB_AES128_BLOCK_SIZE;
  if(len % RADIOLIB_AES128_BLOCK_SIZE) {
    numBlocks++;
  }

  // generate the encryption blocks
  uint8_t encBuffer[RADIOLIB_AES128_BLOCK_SIZE] = { 0 };
  uint8_t encBlock[RADIOLIB_AES128_BLOCK_SIZE] = { 0 };
  encBlock[RADIOLIB_LORAWAN_BLOCK_MAGIC_POS] = RADIOLIB_LORAWAN_ENC_BLOCK_MAGIC;
  encBlock[RADIOLIB_LORAWAN_ENC_BLOCK_COUNTER_ID_POS] = ctrId;
  encBlock[RADIOLIB_LORAWAN_BLOCK_DIR_POS] = dir;
  LoRaWANNode::hton<uint32_t>(&encBlock[RADIOLIB_LORAWAN_BLOCK_DEV_ADDR_POS], addr);
  LoRaWANNode::hton<uint32_t>(&encBlock[RADIOLIB_LORAWAN_BLOCK_FCNT_POS], fCnt);

  // now encrypt the input
  // on downlink frames, this has a decryption effect because server actually "decrypts" the plaintext
  size_t remLen = len;
  for(size_t i = 0; i < numBlocks; i++) {

    if(counter) {
      encBlock[RADIOLIB_LORAWAN_ENC_BLOCK_COUNTER_POS] = i + 1;
    }

    // encrypt the buffer
    RadioLibAES128Instance.init(key);
    RadioLibAES128Instance.encryptECB(encBlock, RADIOLIB_AES128_BLOCK_SIZE, encBuffer);

    // now xor the buffer with the input
    size_t xorLen = remLen;
    if(xorLen > RADIOLIB_AES128_BLOCK_SIZE) {
      xorLen = RADIOLIB_AES128_BLOCK_SIZE;
    }
    for(uint8_t j = 0; j < xorLen; j++) {
      out[i*RADIOLIB_AES128_BLOCK_SIZE + j] = in[i*RADIOLIB_AES128_BLOCK_SIZE + j] ^ encBuffer[j];
    }
    remLen -= xorLen;
  }
}

void LoRaWANNode::sleepDelay(RadioLibTime_t ms) {
  // if the user did not provide sleep callback, or the duration is short, just call delay
  if((this->sleepCb == nullptr) || (ms <= RADIOLIB_LORAWAN_DELAY_SLEEP_THRESHOLD)) {
    Module* mod = this->phyLayer->getMod();
    mod->hal->delay(ms);
    return;
  }

  // otherwise, call the user-provided callback
  this->sleepCb(ms);
}

int16_t LoRaWANNode::checkBufferCommon(const uint8_t *buffer, uint16_t size) {
  // check if there are actually values in the buffer
  size_t i = 0;
  for(; i < size; i++) {
    if(buffer[i]) {
      break;
    }
  }
  if(i == size) {
    return(RADIOLIB_ERR_NETWORK_NOT_JOINED);
  }

  // check integrity of the whole buffer (compare checksum to included checksum)
  uint16_t checkSum = LoRaWANNode::checkSum16(buffer, size - 2);
  uint16_t signature = LoRaWANNode::ntoh<uint16_t>(&buffer[size - 2]);
  if(signature != checkSum) {
    RADIOLIB_DEBUG_PROTOCOL_PRINTLN("Calculated checksum: %04x, expected: %04x", checkSum, signature);
    return(RADIOLIB_ERR_CHECKSUM_MISMATCH);
  }
  return(RADIOLIB_ERR_NONE);
}

uint16_t LoRaWANNode::checkSum16(const uint8_t *key, uint16_t keyLen) {
  uint16_t checkSum = 0;
  for(uint16_t i = 0; i < keyLen; i += 2) {
    uint16_t word = (key[i] << 8);
    if(i + 1 < keyLen) {
      word |= key[i + 1];
    }
    checkSum ^= word;
  }
  return(checkSum);
}

#endif
