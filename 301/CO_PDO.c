/*
 * CANopen Receive Process Data Object protocol.
 *
 * @file        CO_PDO.c
 * @ingroup     CO_PDO
 * @author      Janez Paternoster
 * @copyright   2021 Janez Paternoster
 *
 * This file is part of <https://github.com/CANopenNode/CANopenNode>, a CANopen Stack.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this
 * file except in compliance with the License. You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and limitations under the License.
 */

#include <string.h>

#include "301/CO_PDO.h"

#if ((CO_CONFIG_PDO) & (CO_CONFIG_RPDO_ENABLE | CO_CONFIG_TPDO_ENABLE)) != 0

#if ((CO_CONFIG_PDO)&CO_CONFIG_FLAG_OD_DYNAMIC) != 0
#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_OD_IO_ACCESS) == 0
#error Dynamic PDO mapping is not possible without CO_CONFIG_PDO_OD_IO_ACCESS
#endif
#endif

#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_OD_IO_ACCESS) != 0
/*
 * Custom function for write dummy OD object. Will be used only from RPDO.
 *
 * For more information see file CO_ODinterface.h, OD_IO_t.
 */
static ODR_t
OD_write_dummy(OD_stream_t* stream, const void* buf, OD_size_t count, OD_size_t* countWritten) {
    (void)stream;
    (void)buf;
    if (countWritten != NULL) {
        *countWritten = count;
    }
    return ODR_OK;
}

/*
 * Custom function for read dummy OD object. Will be used only from TPDO.
 *
 * For more information see file CO_ODinterface.h, OD_IO_t.
 */
static ODR_t
OD_read_dummy(OD_stream_t* stream, void* buf, OD_size_t count, OD_size_t* countRead) {
    if ((buf == NULL) || (stream == NULL) || (countRead == NULL)) {
        return ODR_DEV_INCOMPAT;
    }

    if (count > stream->dataLength) {
        count = stream->dataLength;
    }

    (void)memset(buf, 0, count);

    *countRead = count;
    return ODR_OK;
}

/*
 * Find mapped variable in Object Dictionary and configure entry in RPDO or TPDO
 *
 * @param PDO This object will be configured. If map is erroneous, then it will stay unchanged.
 * @param map PDO mapping parameter.
 * @param mapIndex from 0 to CO_PDO_MAX_MAPPED_ENTRIES
 * @param isRPDO True for RPDO and false for TPDO.
 * @param OD Object Dictionary.
 *
 * @return ODR_OK on success, otherwise error reason.
 */
static ODR_t
PDOconfigMap(CO_PDO_common_t* PDO, uint32_t map, uint8_t mapIndex, bool_t isRPDO, OD_t* OD) {
    uint16_t index = (uint16_t)(map >> 16);
    uint8_t subIndex = (uint8_t)(map >> 8);
    uint8_t mappedLengthBits = (uint8_t)map;
    uint8_t mappedLength = mappedLengthBits >> 3;
    OD_IO_t* OD_IO = &PDO->OD_IO[mapIndex];

    /* total PDO length can not be more than CO_PDO_MAX_SIZE bytes */
    if (mappedLength > CO_PDO_MAX_SIZE) {
        return ODR_MAP_LEN; /* PDO length exceeded */
    }

    /* is there a reference to the dummy entry */
    if ((index < 0x20U) && (subIndex == 0U)) {
        OD_stream_t* stream = &OD_IO->stream;
        (void)memset(stream, 0, sizeof(OD_stream_t));
        stream->dataLength = mappedLength;
        stream->dataOffset = mappedLength;
        OD_IO->read = OD_read_dummy;
        OD_IO->write = OD_write_dummy;
        return ODR_OK;
    }

    /* find entry in the Object Dictionary */
    OD_IO_t OD_IOcopy;
    OD_entry_t* entry = OD_find(OD, index);
    ODR_t odRet = OD_getSub(entry, subIndex, &OD_IOcopy, false);
    if (odRet != ODR_OK) {
        return odRet;
    }

    /* verify access attributes, byte alignment and length */
    OD_attr_t testAttribute = isRPDO ? (OD_attr_t)(ODA_RPDO) : (OD_attr_t)(ODA_TPDO);
    if (((OD_IOcopy.stream.attribute & testAttribute) == 0U) || ((mappedLengthBits & 0x07U) != 0U)
        || (OD_IOcopy.stream.dataLength < mappedLength)) {
        return ODR_NO_MAP; /* Object cannot be mapped to the PDO. */
    }

    /* Copy values and store mappedLength temporary. */
    *OD_IO = OD_IOcopy;
    OD_IO->stream.dataOffset = mappedLength;

    /* get TPDO request flag byte from extension */
#if OD_FLAGS_PDO_SIZE > 0
    if (!isRPDO) {
        if ((subIndex < (OD_FLAGS_PDO_SIZE * 8U)) && (entry->extension != NULL)) {
            PDO->flagPDObyte[mapIndex] = &entry->extension->flagsPDO[subIndex >> 3];
            PDO->flagPDObitmask[mapIndex] = 1U << (subIndex & 0x07U);
        } else {
            PDO->flagPDObyte[mapIndex] = NULL;
        }
    }
#endif

    return ODR_OK;
}

/*
 * Initialize PDO mapping parameters
 *
 * @param PDO This object.
 * @param OD Object Dictionary.
 * @param OD_PDOMapPar OD entry for "PDO mapping parameter".
 * @param isRPDO True for RPDO and false for TPDO.
 * @param [out] errInfo Additional information in case of error, may be NULL.
 * @param [out] erroneousMap Additional information about erroneous map.
 *
 * @return #CO_ReturnError_t CO_ERROR_NO on success.
 */
static CO_ReturnError_t
PDO_initMapping(CO_PDO_common_t* PDO, OD_t* OD, OD_entry_t* OD_PDOMapPar, bool_t isRPDO, uint32_t* errInfo,
                uint32_t* erroneousMap) {
    ODR_t odRet;
    size_t pdoDataLength = 0;
    uint8_t mappedObjectsCount = 0;

    /* number of mapped application objects in PDO */
    odRet = OD_get_u8(OD_PDOMapPar, 0, &mappedObjectsCount, true);
    if (odRet != ODR_OK) {
        if (errInfo != NULL) {
            *errInfo = ((uint32_t)OD_getIndex(OD_PDOMapPar)) << 8;
        }
        return CO_ERROR_OD_PARAMETERS;
    }

    for (uint8_t i = 0; i < CO_PDO_MAX_MAPPED_ENTRIES; i++) {
        OD_IO_t* OD_IO = &PDO->OD_IO[i];
        uint32_t map = 0;

        odRet = OD_get_u32(OD_PDOMapPar, i + 1U, &map, true);
        if (odRet == ODR_SUB_NOT_EXIST) {
            continue;
        }
        if (odRet != ODR_OK) {
            if (errInfo != NULL) {
                *errInfo = (((uint32_t)OD_getIndex(OD_PDOMapPar)) << 8) | i;
            }
            return CO_ERROR_OD_PARAMETERS;
        }

        odRet = PDOconfigMap(PDO, map, i, isRPDO, OD);
        if (odRet != ODR_OK) {
            /* indicate erroneous mapping in initialization phase */
            OD_IO->stream.dataLength = 0;
            OD_IO->stream.dataOffset = 0xFF;
            if (*erroneousMap == 0U) {
                *erroneousMap = map;
            }
        }

        if (i < mappedObjectsCount) {
            pdoDataLength += OD_IO->stream.dataOffset;
        }
    }
    if ((pdoDataLength > CO_PDO_MAX_SIZE) || ((pdoDataLength == 0U) && (mappedObjectsCount > 0U))) {
        if (*erroneousMap == 0U) {
            *erroneousMap = 1;
        }
    }

    if (*erroneousMap == 0U) {
        PDO->dataLength = (CO_PDO_size_t)pdoDataLength;
        PDO->mappedObjectsCount = mappedObjectsCount;
    }

    return CO_ERROR_NO;
}

#if ((CO_CONFIG_PDO)&CO_CONFIG_FLAG_OD_DYNAMIC) != 0
/*
 * Custom function for writing OD object "PDO mapping parameter"
 *
 * For more information see file CO_ODinterface.h, OD_IO_t.
 */
static ODR_t
OD_write_PDO_mapping(OD_stream_t* stream, const void* buf, OD_size_t count, OD_size_t* countWritten) {
    /* "count" is already verified in *_init() function */
    if ((stream == NULL) || (buf == NULL) || (countWritten == NULL) || (stream->subIndex > CO_PDO_MAX_MAPPED_ENTRIES)) {
        return ODR_DEV_INCOMPAT;
    }

    /* Only common part of the CO_RPDO_t or CO_TPDO_t will be used */
    CO_PDO_common_t* PDO = stream->object;

    /* PDO must be disabled before mapping configuration */
    if ((PDO->valid) || ((PDO->mappedObjectsCount != 0U) && (stream->subIndex > 0U))) {
        return ODR_UNSUPP_ACCESS;
    }

    if (stream->subIndex == 0U) {
        uint8_t mappedObjectsCount = CO_getUint8(buf);
        size_t pdoDataLength = 0;

        if (mappedObjectsCount > CO_PDO_MAX_MAPPED_ENTRIES) {
            return ODR_MAP_LEN;
        }

        /* validate enabled mapping parameters */
        for (uint8_t i = 0; i < mappedObjectsCount; i++) {
            OD_IO_t* OD_IO = &PDO->OD_IO[i];
            size_t dataLength = (size_t)OD_IO->stream.dataLength;
            size_t mappedLength = (size_t)OD_IO->stream.dataOffset;

            if (mappedLength > dataLength) {
                /* erroneous map since device initial values */
                return ODR_NO_MAP;
            }
            pdoDataLength += mappedLength;
        }

        if (pdoDataLength > CO_PDO_MAX_SIZE) {
            return ODR_MAP_LEN;
        }
        if ((pdoDataLength == 0U) && (mappedObjectsCount > 0U)) {
            return ODR_INVALID_VALUE;
        }

        /* success, update PDO */
        PDO->dataLength = (CO_PDO_size_t)pdoDataLength;
        PDO->mappedObjectsCount = mappedObjectsCount;
    } else {
        uint32_t val = CO_getUint32(buf);
        ODR_t odRet = PDOconfigMap(PDO, val, stream->subIndex - 1U, PDO->isRPDO, PDO->OD);
        if (odRet != ODR_OK) {
            return odRet;
        }
    }

    /* write value to the original location in the Object Dictionary */
    return OD_writeOriginal(stream, buf, count, countWritten);
}
#endif /* (CO_CONFIG_PDO) & CO_CONFIG_FLAG_OD_DYNAMIC */
#endif /* (CO_CONFIG_PDO) & CO_CONFIG_PDO_OD_IO_ACCESS */

#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_OD_IO_ACCESS) == 0
static CO_ReturnError_t
PDO_initMapping(CO_PDO_common_t* PDO, OD_t* OD, OD_entry_t* OD_PDOMapPar, bool_t isRPDO, uint32_t* errInfo,
                uint32_t* erroneousMap) {
    ODR_t odRet;
    size_t pdoDataLength = 0;

    /* number of mapped application objects in PDO */
    uint8_t mappedObjectsCount = 0;
    odRet = OD_get_u8(OD_PDOMapPar, 0, &mappedObjectsCount, true);
    if (odRet != ODR_OK) {
        if (errInfo != NULL) {
            *errInfo = ((uint32_t)OD_getIndex(OD_PDOMapPar)) << 8;
        }
        return CO_ERROR_OD_PARAMETERS;
    }
    if (mappedObjectsCount > CO_PDO_MAX_SIZE) {
        *erroneousMap = 1;
        return CO_ERROR_NO;
    }

    /* iterate mapped OD variables */
    for (uint8_t i = 0; i < mappedObjectsCount; i++) {
        uint32_t map = 0;

        odRet = OD_get_u32(OD_PDOMapPar, i + 1, &map, true);
        if (odRet != ODR_OK) {
            if (errInfo != NULL) {
                *errInfo = (((uint32_t)OD_getIndex(OD_PDOMapPar)) << 8) | i;
            }
            return CO_ERROR_OD_PARAMETERS;
        }
        uint16_t index = (uint16_t)(map >> 16);
        uint8_t subIndex = (uint8_t)(map >> 8);
        uint8_t mappedLengthBits = (uint8_t)map;
        uint8_t mappedLength = mappedLengthBits >> 3;
        uint8_t pdoDataStart = pdoDataLength;
        pdoDataLength += mappedLength;

        if ((mappedLengthBits & 0x07) != 0 || pdoDataLength > CO_PDO_MAX_SIZE) {
            *erroneousMap = map;
            return CO_ERROR_NO;
        }

        /* is there a reference to the dummy entry */
        if (index < 0x20 && subIndex == 0) {
            for (uint8_t j = pdoDataStart; j < pdoDataLength; j++) {
                static uint8_t dummyTX = 0;
                static uint8_t dummyRX;
                PDO->mapPointer[j] = isRPDO ? &dummyRX : &dummyTX;
            }
            continue;
        }

        /* find entry in the Object Dictionary, original location */
        OD_IO_t OD_IO;
        OD_entry_t* entry = OD_find(OD, index);
        OD_attr_t testAttribute = isRPDO ? ODA_RPDO : ODA_TPDO;

        ODR_t odRet = OD_getSub(entry, subIndex, &OD_IO, true);
        if (odRet != ODR_OK || (OD_IO.stream.attribute & testAttribute) == 0 || OD_IO.stream.dataLength < mappedLength
            || OD_IO.stream.dataOrig == NULL) {
            *erroneousMap = map;
            return CO_ERROR_NO;
        }

        /* write locations to OD variable data bytes into PDO map pointers */
#ifdef CO_BIG_ENDIAN
        if ((OD_IO.stream.attribute & ODA_MB) != 0) {
            uint8_t* odDataPointer = OD_IO.stream.dataOrig + OD_IO.stream.dataLength - 1;
            for (uint8_t j = pdoDataStart; j < pdoDataLength; j++) {
                PDO->mapPointer[j] = odDataPointer--;
            }
        } else
#endif
        {
            uint8_t* odDataPointer = OD_IO.stream.dataOrig;
            for (uint8_t j = pdoDataStart; j < pdoDataLength; j++) {
                PDO->mapPointer[j] = odDataPointer++;
            }
        }

        /* get TPDO request flag byte from extension */
#if OD_FLAGS_PDO_SIZE > 0
        if (!isRPDO && subIndex < (OD_FLAGS_PDO_SIZE * 8) && entry->extension != NULL) {
            PDO->flagPDObyte[pdoDataStart] = &entry->extension->flagsPDO[subIndex >> 3];
            PDO->flagPDObitmask[pdoDataStart] = 1 << (subIndex & 0x07);
        }
#endif
    }

    PDO->dataLength = PDO->mappedObjectsCount = pdoDataLength;
    return CO_ERROR_NO;
}

#endif /* ((CO_CONFIG_PDO) & CO_CONFIG_PDO_OD_IO_ACCESS) == 0 */

#if ((CO_CONFIG_PDO)&CO_CONFIG_FLAG_OD_DYNAMIC) != 0
/*
 * Custom function for reading OD object "PDO communication parameter"
 *
 * For more information see file CO_ODinterface.h, OD_IO_t.
 */
static ODR_t
OD_read_PDO_commParam(OD_stream_t* stream, void* buf, OD_size_t count, OD_size_t* countRead) {
    ODR_t returnCode = OD_readOriginal(stream, buf, count, countRead);

    /* When reading COB_ID, add Node-Id to the read value, if necessary */
    if ((returnCode == ODR_OK) && (stream->subIndex == 1U) && (*countRead == 4U)) {
        /* Only common part of the CO_RPDO_t or CO_TPDO_t will be used */
        CO_PDO_common_t* PDO = stream->object;
        uint32_t COB_ID = CO_getUint32(buf);
        uint16_t CAN_ID = (uint16_t)(COB_ID & 0x7FFU);

        /* If default CAN-ID is stored in OD (without Node-ID), add Node-ID */
        if ((CAN_ID != 0U) && (CAN_ID == (PDO->preDefinedCanId & 0xFF80U))) {
            COB_ID = (COB_ID & 0xFFFF0000U) | PDO->preDefinedCanId;
        }

        /* If PDO is not valid, set bit 31 */
        if (!PDO->valid) {
            COB_ID |= 0x80000000U;
        }

        (void)CO_setUint32(buf, COB_ID);
    }

    return returnCode;
}
#endif /* (CO_CONFIG_PDO) & CO_CONFIG_FLAG_OD_DYNAMIC */

/*******************************************************************************
 *      R P D O
 ******************************************************************************/
#if ((CO_CONFIG_PDO)&CO_CONFIG_RPDO_ENABLE) != 0
/*
 * @defgroup CO_PDO_receiveErrors_t States for RPDO->receiveError indicates received RPDOs with wrong length.
 * @{
 *
 */
#define CO_RPDO_RX_ACK_NO_ERROR 0U  /* No error */
#define CO_RPDO_RX_ACK_ERROR    1U  /* Error is acknowledged */
#define CO_RPDO_RX_ACK          10U /* Auxiliary value */
#define CO_RPDO_RX_OK           11U /* Correct RPDO received, not acknowledged */
#define CO_RPDO_RX_SHORT        12U /* Too short RPDO received, not acknowledged */
#define CO_RPDO_RX_LONG         13U /* Too long RPDO received, not acknowledged */

/* @} */ /* CO_PDO_receiveErrors_t */

/*
 * Read received message from CAN module.
 *
 * Function will be called (by CAN receive interrupt) every time, when CAN message with correct identifier
 * will be received. For more information and description of parameters see file CO_driver.h.
 * If new message arrives and previous message wasn't processed yet, then
 * previous message will be lost and overwritten by the new message.
 */
static void
CO_PDO_receive(void* object, void* msg) {
    CO_RPDO_t* RPDO = object;
    CO_PDO_common_t* PDO = &RPDO->PDO_common;
    uint8_t DLC = CO_CANrxMsg_readDLC(msg);
    const uint8_t* data = CO_CANrxMsg_readData(msg);
    uint8_t err = RPDO->receiveError;

    if (PDO->valid) {
        if (DLC >= PDO->dataLength) {
            /* indicate errors in PDO length */
            if (DLC == PDO->dataLength) {
                if (err == CO_RPDO_RX_ACK_ERROR) {
                    err = CO_RPDO_RX_OK;
                }
            } else {
                if (err == CO_RPDO_RX_ACK_NO_ERROR) {
                    err = CO_RPDO_RX_LONG;
                }
            }

            /* Determine, to which of the two rx buffers copy the message. */
            uint8_t bufNo = 0;
#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_SYNC_ENABLE) != 0
            if (RPDO->synchronous && (RPDO->SYNC != NULL) && RPDO->SYNC->CANrxToggle) {
                bufNo = 1;
            }
#endif

            /* copy data into appropriate buffer and set 'new message' flag */
            (void)memcpy(RPDO->CANrxData[bufNo], data, CO_PDO_MAX_SIZE);
            CO_FLAG_SET(RPDO->CANrxNew[bufNo]);

#if ((CO_CONFIG_PDO)&CO_CONFIG_FLAG_CALLBACK_PRE) != 0
            /* Optional signal to RTOS, which can resume task, which handles the RPDO. */
            if (RPDO->pFunctSignalPre != NULL) {
                RPDO->pFunctSignalPre(RPDO->functSignalObjectPre);
            }
#endif
        } else if (err == CO_RPDO_RX_ACK_NO_ERROR) {
            err = CO_RPDO_RX_SHORT;
        } else { /* MISRA C 2004 14.10 */
        }
    }

    RPDO->receiveError = err;
}

#if ((CO_CONFIG_PDO)&CO_CONFIG_FLAG_OD_DYNAMIC) != 0
/*
 * Custom function for writing OD object "RPDO communication parameter"
 *
 * For more information see file CO_ODinterface.h, OD_IO_t.
 */
static ODR_t
OD_write_14xx(OD_stream_t* stream, const void* buf, OD_size_t count, OD_size_t* countWritten) {
    /* "count" is also verified in *_init() function */
    if ((stream == NULL) || (buf == NULL) || (countWritten == NULL) || (count > 4U)) {
        return ODR_DEV_INCOMPAT;
    }

    CO_RPDO_t* RPDO = stream->object;
    CO_PDO_common_t* PDO = &RPDO->PDO_common;
    uint8_t bufCopy[4];
    (void)memcpy((void*)bufCopy, (const void*)buf, count);

    switch (stream->subIndex) {
        case 1: { /* COB-ID used by PDO */
            uint32_t COB_ID = CO_getUint32(buf);
            uint16_t CAN_ID = (uint16_t)(COB_ID & 0x7FFU);
            bool_t valid = (COB_ID & 0x80000000U) == 0U;

            /* bits 11...29 must be zero, PDO must be disabled on change, CAN_ID == 0 is
             * not allowed, mapping must be configured before enabling the PDO */
            if (((COB_ID & 0x3FFFF800U) != 0U) || (valid && PDO->valid && (CAN_ID != PDO->configuredCanId))
                || (valid && CO_IS_RESTRICTED_CAN_ID(CAN_ID)) || (valid && (PDO->mappedObjectsCount == 0U))) {
                return ODR_INVALID_VALUE;
            }

            /* parameter changed? */
            if ((valid != PDO->valid) || (CAN_ID != PDO->configuredCanId)) {
                /* if default CAN-ID is written, store to OD without Node-ID */
                if (CAN_ID == PDO->preDefinedCanId) {
                    (void)CO_setUint32(bufCopy, COB_ID & 0xFFFFFF80U);
                }
                if (!valid) {
                    CAN_ID = 0;
                }

                CO_ReturnError_t ret = CO_CANrxBufferInit(PDO->CANdev, PDO->CANdevIdx, CAN_ID, 0x7FF, false,
                                                          (void*)RPDO, CO_PDO_receive);

                if (valid && (ret == CO_ERROR_NO)) {
                    PDO->valid = true;
                    PDO->configuredCanId = CAN_ID;
                } else {
                    PDO->valid = false;
                    CO_FLAG_CLEAR(RPDO->CANrxNew[0]);
#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_SYNC_ENABLE) != 0
                    CO_FLAG_CLEAR(RPDO->CANrxNew[1]);
#endif
                    if (ret != CO_ERROR_NO) {
                        return ODR_DEV_INCOMPAT;
                    }
                }
            }
            break;
        }

        case 2: { /* transmission type */ uint8_t transmissionType = CO_getUint8(buf);
#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_SYNC_ENABLE) != 0
            if ((transmissionType > (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_240)
                && (transmissionType < (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_EVENT_LO)) {
                return ODR_INVALID_VALUE;
            }

            bool_t synchronous = transmissionType <= (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_240;
            /* Remove old message from the second buffer. */
            if (RPDO->synchronous != synchronous) {
                CO_FLAG_CLEAR(RPDO->CANrxNew[1]);
            }

            RPDO->synchronous = synchronous;
#else
            if (transmissionType < CO_PDO_TRANSM_TYPE_SYNC_EVENT_LO) {
                return ODR_INVALID_VALUE;
            }
#endif
            break;
        }

#if ((CO_CONFIG_PDO)&CO_CONFIG_RPDO_TIMERS_ENABLE) != 0
        case 5: { /* event-timer */
            uint32_t eventTime = CO_getUint16(buf);
            RPDO->timeoutTime_us = eventTime * 1000U;
            RPDO->timeoutTimer = 0;
            break;
        }
#endif
        default:
            /* MISRA C 2004 15.3 */
            break;
    }

    /* write value to the original location in the Object Dictionary */
    return OD_writeOriginal(stream, bufCopy, count, countWritten);
}
#endif /* (CO_CONFIG_PDO) & CO_CONFIG_FLAG_OD_DYNAMIC */

CO_ReturnError_t
CO_RPDO_init(CO_RPDO_t* RPDO, OD_t* OD, CO_EM_t* em,
#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_SYNC_ENABLE) != 0
             CO_SYNC_t* SYNC,
#endif
             uint16_t preDefinedCanId, OD_entry_t* OD_14xx_RPDOCommPar, OD_entry_t* OD_16xx_RPDOMapPar,
             CO_CANmodule_t* CANdevRx, uint16_t CANdevRxIdx, uint32_t* errInfo) {
    CO_PDO_common_t* PDO = &RPDO->PDO_common;
    CO_ReturnError_t ret;
    ODR_t odRet;

    /* verify arguments */
    if ((RPDO == NULL) || (OD == NULL) || (em == NULL) || (OD_14xx_RPDOCommPar == NULL) || (OD_16xx_RPDOMapPar == NULL)
        || (CANdevRx == NULL)) {
        return CO_ERROR_ILLEGAL_ARGUMENT;
    }

    /* clear object */
    (void)memset(RPDO, 0, sizeof(CO_RPDO_t));

    /* Configure object variables */
    PDO->em = em;
    PDO->CANdev = CANdevRx;

    /* Configure mapping parameters */
    uint32_t erroneousMap = 0;
    ret = PDO_initMapping(PDO, OD, OD_16xx_RPDOMapPar, true, errInfo, &erroneousMap);
    if (ret != CO_ERROR_NO) {
        return ret;
    }

    /* Configure communication parameter - COB-ID */
    uint32_t COB_ID = 0;
    odRet = OD_get_u32(OD_14xx_RPDOCommPar, 1, &COB_ID, true);
    if (odRet != ODR_OK) {
        if (errInfo != NULL) {
            *errInfo = (((uint32_t)OD_getIndex(OD_14xx_RPDOCommPar)) << 8) | 1U;
        }
        return CO_ERROR_OD_PARAMETERS;
    }

    bool_t valid = (COB_ID & 0x80000000U) == 0U;
    uint16_t CAN_ID = (uint16_t)(COB_ID & 0x7FFU);
    if (valid && ((PDO->mappedObjectsCount == 0U) || (CAN_ID == 0U))) {
        valid = false;
        if (erroneousMap == 0U) {
            erroneousMap = 1;
        }
    }

    if (erroneousMap != 0U) {
        CO_errorReport(PDO->em, CO_EM_PDO_WRONG_MAPPING, CO_EMC_PROTOCOL_ERROR,
                       (erroneousMap != 1U) ? erroneousMap : COB_ID);
    }
    if (!valid) {
        CAN_ID = 0;
    }

    /* If default CAN-ID is stored in OD (without Node-ID), add Node-ID */
    if ((CAN_ID != 0U) && (CAN_ID == (preDefinedCanId & 0xFF80U))) {
        CAN_ID = preDefinedCanId;
    }

    ret = CO_CANrxBufferInit(CANdevRx, CANdevRxIdx, CAN_ID, 0x7FF, false, (void*)RPDO, CO_PDO_receive);
    if (ret != CO_ERROR_NO) {
        return ret;
    }

    PDO->valid = valid;

    /* Configure communication parameter - transmission type */
#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_SYNC_ENABLE) != 0
    uint8_t transmissionType = (uint8_t)(CO_PDO_TRANSM_TYPE_SYNC_EVENT_LO);
    odRet = OD_get_u8(OD_14xx_RPDOCommPar, 2, &transmissionType, true);
    if (odRet != ODR_OK) {
        if (errInfo != NULL) {
            *errInfo = (((uint32_t)OD_getIndex(OD_14xx_RPDOCommPar)) << 8) | 2U;
        }
        return CO_ERROR_OD_PARAMETERS;
    }

    RPDO->SYNC = SYNC;
    RPDO->synchronous = transmissionType <= (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_240;
#endif

    /* Configure communication parameter - event-timer (optional) */
#if ((CO_CONFIG_PDO)&CO_CONFIG_RPDO_TIMERS_ENABLE) != 0
    uint16_t eventTime = 0;
    (void)OD_get_u16(OD_14xx_RPDOCommPar, 5, &eventTime, true);
    RPDO->timeoutTime_us = (uint32_t)eventTime * 1000U;
#endif

    /* Configure OD extensions */
#if ((CO_CONFIG_PDO)&CO_CONFIG_FLAG_OD_DYNAMIC) != 0
    PDO->isRPDO = true;
    PDO->OD = OD;
    PDO->CANdevIdx = CANdevRxIdx;
    PDO->preDefinedCanId = preDefinedCanId;
    PDO->configuredCanId = CAN_ID;
    PDO->OD_communicationParam_ext.object = RPDO;
    PDO->OD_communicationParam_ext.read = OD_read_PDO_commParam;
    PDO->OD_communicationParam_ext.write = OD_write_14xx;
    PDO->OD_mappingParam_extension.object = RPDO;
    PDO->OD_mappingParam_extension.read = OD_readOriginal;
    PDO->OD_mappingParam_extension.write = OD_write_PDO_mapping;
    (void)OD_extension_init(OD_14xx_RPDOCommPar, &PDO->OD_communicationParam_ext);
    (void)OD_extension_init(OD_16xx_RPDOMapPar, &PDO->OD_mappingParam_extension);
#endif

    return CO_ERROR_NO;
}

#if ((CO_CONFIG_PDO)&CO_CONFIG_FLAG_CALLBACK_PRE) != 0
void
CO_RPDO_initCallbackPre(CO_RPDO_t* RPDO, void* object, void (*pFunctSignalPre)(void* object)) {
    if (RPDO != NULL) {
        RPDO->functSignalObjectPre = object;
        RPDO->pFunctSignalPre = pFunctSignalPre;
    }
}
#endif

void
CO_RPDO_process(CO_RPDO_t* RPDO,
#if ((CO_CONFIG_PDO)&CO_CONFIG_RPDO_TIMERS_ENABLE) != 0
                uint32_t timeDifference_us, uint32_t* timerNext_us,
#endif
                bool_t NMTisOperational, bool_t syncWas) {
    (void)syncWas;
#if ((CO_CONFIG_PDO)&CO_CONFIG_RPDO_TIMERS_ENABLE) != 0
    (void)timerNext_us;
#endif

    CO_PDO_common_t* PDO = &RPDO->PDO_common;

    if (PDO->valid && NMTisOperational
#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_SYNC_ENABLE) != 0
        && (syncWas || !RPDO->synchronous)
#endif
    ) {
        /* Verify errors in length of received RPDO CAN message */
        if (RPDO->receiveError > CO_RPDO_RX_ACK) {
            bool_t setError = RPDO->receiveError != CO_RPDO_RX_OK;
            uint16_t code = (RPDO->receiveError == CO_RPDO_RX_SHORT) ? CO_EMC_PDO_LENGTH : CO_EMC_PDO_LENGTH_EXC;
            CO_error(PDO->em, setError, CO_EM_RPDO_WRONG_LENGTH, code, PDO->dataLength);
            RPDO->receiveError = setError ? CO_RPDO_RX_ACK_ERROR : CO_RPDO_RX_ACK_NO_ERROR;
        }

        /* Determine, which of the two rx buffers contains relevant message. */
        uint8_t bufNo = 0;
#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_SYNC_ENABLE) != 0
        if (RPDO->synchronous && (RPDO->SYNC != NULL) && !RPDO->SYNC->CANrxToggle) {
            bufNo = 1;
        }
#endif

        /* copy RPDO into OD variables according to mappings */
        bool_t rpdoReceived = false;
        while (CO_FLAG_READ(RPDO->CANrxNew[bufNo])) {
            rpdoReceived = true;
            uint8_t* dataRPDO = RPDO->CANrxData[bufNo];
            OD_size_t verifyLength = 0U;

            /* Clear the flag. If between the copy operation CANrxNew is set
             * by receive thread, then copy the latest data again. */
            CO_FLAG_CLEAR(RPDO->CANrxNew[bufNo]);

#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_OD_IO_ACCESS) != 0
            for (uint8_t i = 0; i < PDO->mappedObjectsCount; i++) {
                OD_IO_t* OD_IO = &PDO->OD_IO[i];

                /* get mappedLength from temporary storage */
                OD_size_t* dataOffset = &OD_IO->stream.dataOffset;
                uint8_t mappedLength = (uint8_t)(*dataOffset);

                /* additional safety check. */
                verifyLength += (OD_size_t)mappedLength;
                if (verifyLength > CO_PDO_MAX_SIZE) {
                    break;
                }

                /* length of OD variable may be larger than mappedLength */
                OD_size_t ODdataLength = OD_IO->stream.dataLength;
                if (ODdataLength > CO_PDO_MAX_SIZE) {
                    ODdataLength = CO_PDO_MAX_SIZE;
                }
                /* Prepare data for writing into OD variable. If mappedLength
                 * is smaller than ODdataLength, then use auxiliary buffer */
                uint8_t buf[CO_PDO_MAX_SIZE];
                uint8_t* dataOD;
                if (ODdataLength > mappedLength) {
                    (void)memset(buf, 0, sizeof(buf));
                    (void)memcpy(buf, dataRPDO, mappedLength);
                    dataOD = buf;
                } else {
                    dataOD = dataRPDO;
                }

                /* swap multibyte data if big-endian */
#ifdef CO_BIG_ENDIAN
                if ((OD_IO->stream.attribute & ODA_MB) != 0) {
                    uint8_t* lo = dataOD;
                    uint8_t* hi = dataOD + ODdataLength - 1;
                    while (lo < hi) {
                        uint8_t swap = *lo;
                        *lo++ = *hi;
                        *hi-- = swap;
                    }
                }
#endif

                /* Set stream.dataOffset to zero, perform OD_IO.write()
                 * and store mappedLength back to stream.dataOffset */
                *dataOffset = 0;
                OD_size_t countWritten;
                OD_IO->write(&OD_IO->stream, dataOD, ODdataLength, &countWritten);
                *dataOffset = mappedLength;

                dataRPDO += mappedLength;
            }

#else
            verifyLength = (OD_size_t)PDO->dataLength;
            for (uint8_t i = 0; i < PDO->dataLength; i++) {
                *PDO->mapPointer[i] = dataRPDO[i];
            }
#endif /* (CO_CONFIG_PDO) & CO_CONFIG_PDO_OD_IO_ACCESS */

            if ((verifyLength > CO_PDO_MAX_SIZE) || (verifyLength != (OD_size_t)PDO->dataLength)) {
                /* bug in software, should not happen */
                CO_errorReport(PDO->em, CO_EM_GENERIC_SOFTWARE_ERROR, CO_EMC_SOFTWARE_INTERNAL,
                               (0x100000U | verifyLength));
            }
        } /* while (CO_FLAG_READ(RPDO->CANrxNew[bufNo])) */

        /* verify RPDO timeout */
        (void)rpdoReceived;
#if ((CO_CONFIG_PDO)&CO_CONFIG_RPDO_TIMERS_ENABLE) != 0
        if (RPDO->timeoutTime_us > 0U) {
            if (rpdoReceived) {
                if (RPDO->timeoutTimer > RPDO->timeoutTime_us) {
                    CO_errorReset(PDO->em, CO_EM_RPDO_TIME_OUT, RPDO->timeoutTimer);
                }
                /* enable monitoring */
                RPDO->timeoutTimer = 1;
            } else if ((RPDO->timeoutTimer > 0U) && (RPDO->timeoutTimer < RPDO->timeoutTime_us)) {
                RPDO->timeoutTimer += timeDifference_us;

                if (RPDO->timeoutTimer > RPDO->timeoutTime_us) {
                    CO_errorReport(PDO->em, CO_EM_RPDO_TIME_OUT, CO_EMC_RPDO_TIMEOUT, RPDO->timeoutTimer);
                }
            } else { /* MISRA C 2004 14.10 */
            }
#if ((CO_CONFIG_PDO)&CO_CONFIG_FLAG_TIMERNEXT) != 0
            if ((timerNext_us != NULL) && (RPDO->timeoutTimer < RPDO->timeoutTime_us)) {
                uint32_t diff = RPDO->timeoutTime_us - RPDO->timeoutTimer;
                if (*timerNext_us > diff) {
                    *timerNext_us = diff;
                }
            }
#endif
        }
#endif /* (CO_CONFIG_PDO) & CO_CONFIG_RPDO_TIMERS_ENABLE */
    }  /* if (PDO->valid && NMTisOperational) */
    else {
        /* not valid and operational, clear CAN receive flags and timeoutTimer */
#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_SYNC_ENABLE) != 0
        if (!PDO->valid || !NMTisOperational) {
            CO_FLAG_CLEAR(RPDO->CANrxNew[0]);
            CO_FLAG_CLEAR(RPDO->CANrxNew[1]);
#if ((CO_CONFIG_PDO)&CO_CONFIG_RPDO_TIMERS_ENABLE) != 0
            RPDO->timeoutTimer = 0;
#endif
        }
#else
        CO_FLAG_CLEAR(RPDO->CANrxNew[0]);
#if ((CO_CONFIG_PDO)&CO_CONFIG_RPDO_TIMERS_ENABLE) != 0
        RPDO->timeoutTimer = 0;
#endif
#endif
    }
}
#endif /* (CO_CONFIG_PDO) & CO_CONFIG_RPDO_ENABLE */

/*******************************************************************************
 *      T P D O
 ******************************************************************************/
#if ((CO_CONFIG_PDO)&CO_CONFIG_TPDO_ENABLE) != 0

#if ((CO_CONFIG_PDO)&CO_CONFIG_TPDO_RTR_ENABLE) != 0
static void
CO_TPDO_RTR_receive(void* object, void* msg) {
    CO_TPDO_t* TPDO = object;
    CO_PDO_common_t* PDO = &TPDO->PDO_common;
    uint8_t DLC = CO_CANrxMsg_readDLC(msg);
	bool_t RTR = ( CO_CANrxMsg_readIdent(msg) & 0x0800U ) != 0U;

    if ( (PDO->valid) && (DLC == 0U) && RTR ) {
		TPDO->rtrRequest = true;
	}
}
#endif

#if ((CO_CONFIG_PDO)&CO_CONFIG_FLAG_OD_DYNAMIC) != 0
/*
 * Custom function for writing OD object "TPDO communication parameter"
 *
 * For more information see file CO_ODinterface.h, OD_IO_t.
 */
static ODR_t
OD_write_18xx(OD_stream_t* stream, const void* buf, OD_size_t count, OD_size_t* countWritten) {
    /* "count" is also verified in *_init() function */
    if ((stream == NULL) || (buf == NULL) || (countWritten == NULL) || (count > 4U)) {
        return ODR_DEV_INCOMPAT;
    }

    CO_TPDO_t* TPDO = stream->object;
    CO_PDO_common_t* PDO = &TPDO->PDO_common;
    uint8_t bufCopy[4];
    (void)memcpy((void*)bufCopy, (const void*)buf, count);

    switch (stream->subIndex) {
        case 1: { /* COB-ID used by PDO */
            uint32_t COB_ID = CO_getUint32(buf);
            uint16_t CAN_ID = (uint16_t)(COB_ID & 0x7FFU);
            bool_t valid = (COB_ID & 0x80000000U) == 0U;
            bool_t rtr_en = (COB_ID & 0x40000000U) == 0U;

            /* bits 11...29 must be zero, PDO must be disabled on change, CAN_ID == 0 is
             * not allowed, mapping must be configured before enabling the PDO */
            if (((COB_ID & 0x3FFFF800U) != 0U) || (valid && (PDO->valid && (CAN_ID != PDO->configuredCanId)))
                || (valid && CO_IS_RESTRICTED_CAN_ID(CAN_ID)) || (valid && (PDO->mappedObjectsCount == 0U))) {
                return ODR_INVALID_VALUE;
            }

            /* For compatibility you should refuse to enable RTR if it is not supported, but for legacy (easy to use) this code is commented out
            #if ((CO_CONFIG_PDO)&CO_CONFIG_TPDO_RTR_ENABLE) == 0
            if( rtr_en ) {
                return ODR_INVALID_VALUE;
            }
            #endif
            */

            /* parameter changed? */
            if ((valid != PDO->valid) || (CAN_ID != PDO->configuredCanId) || (rtr_en != TPDO->rtr_en)) {
                /* if default CAN-ID is written, store to OD without Node-ID */
                if (CAN_ID == PDO->preDefinedCanId) {
                    (void)CO_setUint32(bufCopy, COB_ID & 0xFFFFFF80U);
                }
                if (!valid) {
                    CAN_ID = 0;
					rtr_en = false;
                }

                CO_CANtx_t* CANtxBuff = CO_CANtxBufferInit(
                    PDO->CANdev, PDO->CANdevIdx, CAN_ID, false, PDO->dataLength,
                    TPDO->transmissionType <= (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_240);

                if (CANtxBuff == NULL) {
                    return ODR_DEV_INCOMPAT;
                }

                TPDO->CANtxBuff = CANtxBuff;
                PDO->valid = valid;
                PDO->configuredCanId = CAN_ID;
                TPDO->rtr_en = rtr_en;
            }
            break;
        }

        case 2: { /* transmission type */ uint8_t transmissionType = CO_getUint8(buf);
#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_SYNC_ENABLE) != 0
            if ((transmissionType > (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_240)
                && (transmissionType < (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_EVENT_LO)) {
                return ODR_INVALID_VALUE;
            }
            TPDO->CANtxBuff->syncFlag = transmissionType <= (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_240;
            TPDO->syncCounter = 255;
#else
            if (transmissionType < CO_PDO_TRANSM_TYPE_SYNC_EVENT_LO) {
                return ODR_INVALID_VALUE;
            }
#endif
            TPDO->transmissionType = transmissionType;
            TPDO->sendRequest = true;
#if ((CO_CONFIG_PDO)&CO_CONFIG_TPDO_TIMERS_ENABLE) != 0
            TPDO->inhibitTimer = 0;
            TPDO->eventTimer = 0;
#endif
            break;
        }

#if ((CO_CONFIG_PDO)&CO_CONFIG_TPDO_TIMERS_ENABLE) != 0
        case 3: { /* inhibit time */
            if (PDO->valid) {
                return ODR_INVALID_VALUE;
            }
            uint32_t inhibitTime = CO_getUint16(buf);
            TPDO->inhibitTime_us = inhibitTime * 100U;
            TPDO->inhibitTimer = 0;
            break;
        }

        case 5: { /* event-timer */
            uint32_t eventTime = CO_getUint16(buf);
            TPDO->eventTime_us = eventTime * 1000U;
            TPDO->eventTimer = 0;
            break;
        }
#endif

#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_SYNC_ENABLE) != 0
        case 6: { /* SYNC start value */
            uint8_t syncStartValue = CO_getUint8(buf);

            if (PDO->valid || (syncStartValue > 240U)) {
                return ODR_INVALID_VALUE;
            }
            TPDO->syncStartValue = syncStartValue;
            break;
        }
#endif
        default:
            /* MISRA C 2004 15.3 */
            break;
    }

    /* write value to the original location in the Object Dictionary */
    return OD_writeOriginal(stream, bufCopy, count, countWritten);
}
#endif /* (CO_CONFIG_PDO) & CO_CONFIG_FLAG_OD_DYNAMIC */

CO_ReturnError_t
CO_TPDO_init(CO_TPDO_t* TPDO, OD_t* OD, CO_EM_t* em,
#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_SYNC_ENABLE) != 0
             CO_SYNC_t* SYNC,
#endif
             uint16_t preDefinedCanId, OD_entry_t* OD_18xx_TPDOCommPar, OD_entry_t* OD_1Axx_TPDOMapPar,
             CO_CANmodule_t* CANdevTx, uint16_t CANdevTxIdx,
#if ((CO_CONFIG_PDO)&CO_CONFIG_TPDO_RTR_ENABLE) != 0
             uint16_t CANdevRxIdx,
#endif
             uint32_t* errInfo) {
    CO_PDO_common_t* PDO = &TPDO->PDO_common;
    ODR_t odRet;

    /* verify arguments */
    if ((TPDO == NULL) || (OD == NULL) || (em == NULL) || (OD_18xx_TPDOCommPar == NULL) || (OD_1Axx_TPDOMapPar == NULL)
        || (CANdevTx == NULL)) {
        return CO_ERROR_ILLEGAL_ARGUMENT;
    }

    /* clear object */
    (void)memset(TPDO, 0, sizeof(CO_TPDO_t));

    /* Configure object variables */
    PDO->em = em;
    PDO->CANdev = CANdevTx;

    /* Configure mapping parameters */
    uint32_t erroneousMap = 0;
    CO_ReturnError_t ret = PDO_initMapping(PDO, OD, OD_1Axx_TPDOMapPar, false, errInfo, &erroneousMap);
    if (ret != CO_ERROR_NO) {
        return ret;
    }

    /* Configure communication parameter - transmission type */
    uint8_t transmissionType = (uint8_t)(CO_PDO_TRANSM_TYPE_SYNC_EVENT_LO);
    odRet = OD_get_u8(OD_18xx_TPDOCommPar, 2, &transmissionType, true);
    if (odRet != ODR_OK) {
        if (errInfo != NULL) {
            *errInfo = (((uint32_t)OD_getIndex(OD_18xx_TPDOCommPar)) << 8) | 2U;
        }
        return CO_ERROR_OD_PARAMETERS;
    }
    if ((transmissionType < (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_EVENT_LO)
#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_SYNC_ENABLE) != 0
        && (transmissionType > (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_240)
#endif
    ) {
        transmissionType = (uint8_t)(CO_PDO_TRANSM_TYPE_SYNC_EVENT_LO);
    }
    TPDO->transmissionType = transmissionType;
    TPDO->sendRequest = true;

    /* Configure communication parameter - COB-ID */
    uint32_t COB_ID = 0;
    odRet = OD_get_u32(OD_18xx_TPDOCommPar, 1, &COB_ID, true);
    if (odRet != ODR_OK) {
        if (errInfo != NULL) {
            *errInfo = (((uint32_t)OD_getIndex(OD_18xx_TPDOCommPar)) << 8) | 1U;
        }
        return CO_ERROR_OD_PARAMETERS;
    }

    bool_t valid = (COB_ID & 0x80000000U) == 0U;
    bool_t rtr_en = (COB_ID & 0x40000000U) == 0U;
    uint16_t CAN_ID = (uint16_t)(COB_ID & 0x7FFU);
    if (valid && ((PDO->mappedObjectsCount == 0U) || (CAN_ID == 0U))) {
        valid = false;
        if (erroneousMap == 0U) {
            erroneousMap = 1;
        }
    }

    if (erroneousMap != 0U) {
        CO_errorReport(PDO->em, CO_EM_PDO_WRONG_MAPPING, CO_EMC_PROTOCOL_ERROR,
                       (erroneousMap != 1U) ? erroneousMap : COB_ID);
    }
    if (!valid) {
        CAN_ID = 0;
		rtr_en = false;
    }

    /* If default CAN-ID is stored in OD (without Node-ID), add Node-ID */
    if ((CAN_ID != 0U) && (CAN_ID == (preDefinedCanId & 0xFF80U))) {
        CAN_ID = preDefinedCanId;
    }

    TPDO->CANtxBuff = CO_CANtxBufferInit(CANdevTx, CANdevTxIdx, CAN_ID, false, PDO->dataLength,
                                         TPDO->transmissionType <= (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_240);

    if (TPDO->CANtxBuff == NULL) {
        return CO_ERROR_ILLEGAL_ARGUMENT;
    }
    
#if ((CO_CONFIG_PDO)&CO_CONFIG_TPDO_RTR_ENABLE) != 0
    ret = CO_CANrxBufferInit(CANdevTx, CANdevRxIdx, CAN_ID, 0x7FF, true, (void*)TPDO, CO_TPDO_RTR_receive);
    if (ret != CO_ERROR_NO) {
        return ret;
    }
#endif

    PDO->valid = valid;
	TPDO->rtr_en = rtr_en;

    /* Configure communication parameter - inhibit time and event-timer (opt) */
#if ((CO_CONFIG_PDO)&CO_CONFIG_TPDO_TIMERS_ENABLE) != 0
    uint16_t inhibitTime = 0;
    uint16_t eventTime = 0;
    (void)OD_get_u16(OD_18xx_TPDOCommPar, 3, &inhibitTime, true);
    (void)OD_get_u16(OD_18xx_TPDOCommPar, 5, &eventTime, true);
    TPDO->inhibitTime_us = (uint32_t)inhibitTime * 100U;
    TPDO->eventTime_us = (uint32_t)eventTime * 1000U;
#endif

    /* Configure communication parameter - SYNC start value (optional) */
#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_SYNC_ENABLE) != 0
    TPDO->syncStartValue = 0;
    (void)OD_get_u8(OD_18xx_TPDOCommPar, 6, &TPDO->syncStartValue, true);
    TPDO->SYNC = SYNC;
    TPDO->syncCounter = 255;
#endif

    /* Configure OD extensions */
#if ((CO_CONFIG_PDO)&CO_CONFIG_FLAG_OD_DYNAMIC) != 0
    PDO->isRPDO = false;
    PDO->OD = OD;
    PDO->CANdevIdx = CANdevTxIdx;
    PDO->preDefinedCanId = preDefinedCanId;
    PDO->configuredCanId = CAN_ID;
    PDO->OD_communicationParam_ext.object = TPDO;
    PDO->OD_communicationParam_ext.read = OD_read_PDO_commParam;
    PDO->OD_communicationParam_ext.write = OD_write_18xx;
    PDO->OD_mappingParam_extension.object = TPDO;
    PDO->OD_mappingParam_extension.read = OD_readOriginal;
    PDO->OD_mappingParam_extension.write = OD_write_PDO_mapping;
    (void)OD_extension_init(OD_18xx_TPDOCommPar, &PDO->OD_communicationParam_ext);
    (void)OD_extension_init(OD_1Axx_TPDOMapPar, &PDO->OD_mappingParam_extension);
#endif

    return CO_ERROR_NO;
}

/*
 * Send TPDO message.
 *
 * Function prepares TPDO data from Object Dictionary variables. It is called
 * from CO_TPDO_process() according to TPDO communication parameters.
 *
 * @param TPDO TPDO object.
 *
 * @return Same as CO_CANsend().
 */
static CO_ReturnError_t
CO_TPDOsend(CO_TPDO_t* TPDO) {
    CO_PDO_common_t* PDO = &TPDO->PDO_common;
    uint8_t* dataTPDO = &TPDO->CANtxBuff->data[0];
    OD_size_t verifyLength = 0U;

#if OD_FLAGS_PDO_SIZE > 0
    bool_t eventDriven = ((TPDO->transmissionType == (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_ACYCLIC)
                          || (TPDO->transmissionType >= (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_EVENT_LO));
#endif

#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_OD_IO_ACCESS) != 0
    for (uint8_t i = 0; i < PDO->mappedObjectsCount; i++) {
        OD_IO_t* OD_IO = &PDO->OD_IO[i];
        OD_stream_t* stream = &OD_IO->stream;

        /* get mappedLength from temporary storage */
        uint8_t mappedLength = (uint8_t)stream->dataOffset;

        /* additional safety check */
        verifyLength += (OD_size_t)mappedLength;
        if (verifyLength > CO_PDO_MAX_SIZE) {
            break;
        }

        /* length of OD variable may be larger than mappedLength */
        OD_size_t ODdataLength = stream->dataLength;
        if (ODdataLength > CO_PDO_MAX_SIZE) {
            ODdataLength = CO_PDO_MAX_SIZE;
        }
        /* If mappedLength is smaller than ODdataLength, use auxiliary buffer */
        uint8_t buf[CO_PDO_MAX_SIZE];
        uint8_t* dataTPDOCopy;
        if (ODdataLength > mappedLength) {
            (void)memset(buf, 0, sizeof(buf));
            dataTPDOCopy = buf;
        } else {
            dataTPDOCopy = dataTPDO;
        }

        /* Set stream.dataOffset to zero, perform OD_IO.read() and store mappedLength back to stream.dataOffset */
        stream->dataOffset = 0;
        OD_size_t countRd;
        OD_IO->read(stream, dataTPDOCopy, ODdataLength, &countRd);
        stream->dataOffset = mappedLength;

        /* swap multibyte data if big-endian */
#ifdef CO_BIG_ENDIAN
        if ((stream->attribute & ODA_MB) != 0) {
            uint8_t* lo = dataTPDOCopy;
            uint8_t* hi = dataTPDOCopy + ODdataLength - 1;
            while (lo < hi) {
                uint8_t swap = *lo;
                *lo++ = *hi;
                *hi-- = swap;
            }
        }
#endif

        /* If auxiliary buffer, copy it to the TPDO */
        if (ODdataLength > mappedLength) {
            (void)memcpy(dataTPDO, buf, mappedLength);
        }

        /* In event driven TPDO indicate transmission of OD variable */
#if OD_FLAGS_PDO_SIZE > 0
        uint8_t* flagPDObyte = PDO->flagPDObyte[i];
        if ((flagPDObyte != NULL) && eventDriven) {
            *flagPDObyte |= PDO->flagPDObitmask[i];
        }
#endif

        dataTPDO += mappedLength;
    }
#else
    verifyLength = (OD_size_t)PDO->dataLength;
    for (uint8_t i = 0; i < PDO->dataLength; i++) {
        dataTPDO[i] = *PDO->mapPointer[i];

        /* In event driven TPDO indicate transmission of OD variable */
#if OD_FLAGS_PDO_SIZE > 0
        uint8_t* flagPDObyte = PDO->flagPDObyte[i];
        if (flagPDObyte != NULL && eventDriven) {
            *flagPDObyte |= PDO->flagPDObitmask[i];
        }
#endif
    }
#endif /* (CO_CONFIG_PDO) & CO_CONFIG_PDO_OD_IO_ACCESS */

    if ((verifyLength > CO_PDO_MAX_SIZE) || (verifyLength != (OD_size_t)PDO->dataLength)) {
        /* bug in software, should not happen */
        CO_errorReport(PDO->em, CO_EM_GENERIC_SOFTWARE_ERROR, CO_EMC_SOFTWARE_INTERNAL, (0x200000U | verifyLength));
        return CO_ERROR_DATA_CORRUPT;
    }

    TPDO->sendRequest = false;
#if ((CO_CONFIG_PDO)&CO_CONFIG_TPDO_TIMERS_ENABLE) != 0
    TPDO->eventTimer = TPDO->eventTime_us;
    TPDO->inhibitTimer = TPDO->inhibitTime_us;
#endif
    return CO_CANsend(PDO->CANdev, TPDO->CANtxBuff);
}

void
CO_TPDO_process(CO_TPDO_t* TPDO,
#if (((CO_CONFIG_PDO)&CO_CONFIG_TPDO_TIMERS_ENABLE) != 0) || defined CO_DOXYGEN
                uint32_t timeDifference_us, uint32_t* timerNext_us,
#endif
                bool_t NMTisOperational, bool_t syncWas) {
    CO_PDO_common_t* PDO = &TPDO->PDO_common;
#if (((CO_CONFIG_PDO)&CO_CONFIG_TPDO_TIMERS_ENABLE)) != 0
    (void)timerNext_us;
#endif
    (void)syncWas;

    #if ((CO_CONFIG_PDO)&CO_CONFIG_TPDO_RTR_ENABLE) != 0
	if( TPDO->rtrRequest) {
		TPDO->rtrRequest = false;
		if( TPDO->rtr_en ) {
			TPDO->sendRequest = true;
		}
	}
    #endif

    if (PDO->valid && NMTisOperational) {

        /* check for event timer or application event */
#if (((CO_CONFIG_PDO)&CO_CONFIG_TPDO_TIMERS_ENABLE) != 0) || (OD_FLAGS_PDO_SIZE > 0)
        if ((TPDO->transmissionType == (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_ACYCLIC)
            || (TPDO->transmissionType >= (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_EVENT_LO)) {
            /* event timer */
#if ((CO_CONFIG_PDO)&CO_CONFIG_TPDO_TIMERS_ENABLE) != 0
            if (TPDO->eventTime_us != 0U) {
                TPDO->eventTimer = (TPDO->eventTimer > timeDifference_us) ? (TPDO->eventTimer - timeDifference_us) : 0U;
                if (TPDO->eventTimer == 0U) {
                    TPDO->sendRequest = true;
                }
#if ((CO_CONFIG_PDO)&CO_CONFIG_FLAG_TIMERNEXT) != 0
                if ((timerNext_us != NULL) && (*timerNext_us > TPDO->eventTimer)) {
                    /* Schedule for next event time */
                    *timerNext_us = TPDO->eventTimer;
                }
#endif
            }
#endif
            /* check for any OD_requestTPDO() */
#if OD_FLAGS_PDO_SIZE > 0
            if (!TPDO->sendRequest) {
                for (uint8_t i = 0; i < PDO->mappedObjectsCount; i++) {
                    uint8_t* flagPDObyte = PDO->flagPDObyte[i];
                    if (flagPDObyte != NULL) {
                        if ((*flagPDObyte & PDO->flagPDObitmask[i]) == 0U) {
                            TPDO->sendRequest = true;
                            break;
                        }
                    }
                }
            }
#endif
        }
#endif /* ((CO_CONFIG_PDO)&CO_CONFIG_TPDO_TIMERS_ENABLE)||(OD_FLAGS_PDO_SIZE>0) */

        /* Send PDO by application request or by Event timer */
        if (TPDO->transmissionType >= (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_EVENT_LO) {
#if ((CO_CONFIG_PDO)&CO_CONFIG_TPDO_TIMERS_ENABLE) != 0
            TPDO->inhibitTimer = (TPDO->inhibitTimer > timeDifference_us) ? (TPDO->inhibitTimer - timeDifference_us)
                                                                          : 0U;

            /* send TPDO */
            if (TPDO->sendRequest && (TPDO->inhibitTimer == 0U)) {
                (void)CO_TPDOsend(TPDO);
            }

#if ((CO_CONFIG_PDO)&CO_CONFIG_FLAG_TIMERNEXT) != 0
            if (TPDO->sendRequest && (timerNext_us != NULL) && (*timerNext_us > TPDO->inhibitTimer)) {
                /* Schedule for just beyond inhibit window */
                *timerNext_us = TPDO->inhibitTimer;
            }
#endif
#else
            if (TPDO->sendRequest) {
                (void)CO_TPDOsend(TPDO);
            }
#endif
        } /* if (TPDO->transmissionType >= CO_PDO_TRANSM_TYPE_SYNC_EVENT_LO) */

        /* Synchronous PDOs */
#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_SYNC_ENABLE) != 0
        else if ((TPDO->SYNC != NULL) && syncWas) {
            /* send synchronous acyclic TPDO */
            if (TPDO->transmissionType == (uint8_t)CO_PDO_TRANSM_TYPE_SYNC_ACYCLIC) {
                if (TPDO->sendRequest) {
                    (void)CO_TPDOsend(TPDO);
                }
            }
            /* send synchronous cyclic TPDO */
            else {
                /* is the start of synchronous TPDO transmission */
                if (TPDO->syncCounter == 255U) {
                    if ((TPDO->SYNC->counterOverflowValue != 0U) && (TPDO->syncStartValue != 0U)) {
                        /* syncStartValue is in use */
                        TPDO->syncCounter = 254;
                    } else {
                        /* Send first TPDO somewhere in the middle */
                        TPDO->syncCounter = (TPDO->transmissionType / 2U) + 1U;
                    }
                }
                /* If the syncStartValue is in use, start first TPDO after SYNC with matched syncStartValue. */
                if (TPDO->syncCounter == 254U) {
                    if (TPDO->SYNC->counter == TPDO->syncStartValue) {
                        TPDO->syncCounter = TPDO->transmissionType;
                        (void)CO_TPDOsend(TPDO);
                    }
                }
                /* Send TPDO after every N-th Sync */
                else if (--TPDO->syncCounter == 0U) {
                    TPDO->syncCounter = TPDO->transmissionType;
                    (void)CO_TPDOsend(TPDO);
                } else { /* MISRA C 2004 14.10 */
                }
            }
        }      /* else if (TPDO->SYNC && syncWas) */
        else { /* MISRA C 2004 14.10 */
        }
#endif

    } else {
        /* Not operational or valid, reset triggers */
        TPDO->sendRequest = true;
#if ((CO_CONFIG_PDO)&CO_CONFIG_TPDO_TIMERS_ENABLE) != 0
        TPDO->inhibitTimer = 0;
        TPDO->eventTimer = 0;
#endif
#if ((CO_CONFIG_PDO)&CO_CONFIG_PDO_SYNC_ENABLE) != 0
        TPDO->syncCounter = 255;
#endif
    }
}
#endif /* (CO_CONFIG_PDO) & CO_CONFIG_TPDO_ENABLE */
#endif /* (CO_CONFIG_PDO) & (CO_CONFIG_RPDO_ENABLE | CO_CONFIG_TPDO_ENABLE) */
