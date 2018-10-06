#include "davis.h"

#include <math.h>

static ssize_t davisFindInternal(uint16_t deviceType, caerDeviceDiscoveryResult *discoveredDevices);

static caerDeviceHandle davisOpenInternal(uint16_t deviceType, uint16_t deviceID, uint8_t busNumberRestrict,
	uint8_t devAddressRestrict, const char *serialNumberRestrict);

static void davisLog(enum caer_log_level logLevel, davisHandle handle, const char *format, ...) ATTRIBUTE_FORMAT(3);
static bool davisSendDefaultFPGAConfig(caerDeviceHandle cdh);
static bool davisSendDefaultChipConfig(caerDeviceHandle cdh);
static void davisEventTranslator(void *vhd, const uint8_t *buffer, size_t bytesSent);
static void davisTSMasterStatusUpdater(void *userDataPtr, int status, uint32_t param);

// FX3 Debug Transfer Support
static void allocateDebugTransfers(davisHandle handle);
static void cancelAndDeallocateDebugTransfers(davisHandle handle);
static void LIBUSB_CALL libUsbDebugCallback(struct libusb_transfer *transfer);
static void debugTranslator(davisHandle handle, const uint8_t *buffer, size_t bytesSent);

static void davisLog(enum caer_log_level logLevel, davisHandle handle, const char *format, ...) {
	va_list argumentList;
	va_start(argumentList, format);
	caerLogVAFull(caerLogFileDescriptorsGetFirst(), caerLogFileDescriptorsGetSecond(),
		atomic_load_explicit(&handle->state.deviceLogLevel, memory_order_relaxed), logLevel, handle->info.deviceString,
		format, argumentList);
	va_end(argumentList);
}

static inline bool ensureSpaceForEvents(
	caerEventPacketHeader *packet, size_t position, size_t numEvents, davisHandle handle) {
	if ((position + numEvents) <= (size_t) caerEventPacketHeaderGetEventCapacity(*packet)) {
		return (true);
	}

	caerEventPacketHeader grownPacket
		= caerEventPacketGrow(*packet, caerEventPacketHeaderGetEventCapacity(*packet) * 2);
	if (grownPacket == NULL) {
		davisLog(CAER_LOG_CRITICAL, handle, "Failed to grow event packet of type %d.",
			caerEventPacketHeaderGetEventType(*packet));
		return (false);
	}

	*packet = grownPacket;
	return (true);
}

ssize_t davisFindAll(caerDeviceDiscoveryResult *discoveredDevices) {
	return (davisFindInternal(CAER_DEVICE_DAVIS, discoveredDevices));
}

ssize_t davisFindFX2(caerDeviceDiscoveryResult *discoveredDevices) {
	return (davisFindInternal(CAER_DEVICE_DAVIS_FX2, discoveredDevices));
}

ssize_t davisFindFX3(caerDeviceDiscoveryResult *discoveredDevices) {
	return (davisFindInternal(CAER_DEVICE_DAVIS_FX3, discoveredDevices));
}

static ssize_t davisFindInternal(uint16_t deviceType, caerDeviceDiscoveryResult *discoveredDevices) {
	// Set to NULL initially (for error return).
	*discoveredDevices = NULL;

	ssize_t resultFX2              = 0;
	struct usb_info *foundDavisFX2 = NULL;

	if ((deviceType == CAER_DEVICE_DAVIS) || (deviceType == CAER_DEVICE_DAVIS_FX2)) {
		resultFX2 = usbDeviceFind(USB_DEFAULT_DEVICE_VID, DAVIS_FX2_DEVICE_PID, DAVIS_FX2_REQUIRED_LOGIC_REVISION,
			DAVIS_FX2_REQUIRED_FIRMWARE_VERSION, &foundDavisFX2);
	}

	ssize_t resultFX3              = 0;
	struct usb_info *foundDavisFX3 = NULL;

	if ((deviceType == CAER_DEVICE_DAVIS) || (deviceType == CAER_DEVICE_DAVIS_FX3)) {
		resultFX3 = usbDeviceFind(USB_DEFAULT_DEVICE_VID, DAVIS_FX3_DEVICE_PID, DAVIS_FX3_REQUIRED_LOGIC_REVISION,
			DAVIS_FX3_REQUIRED_FIRMWARE_VERSION, &foundDavisFX3);
	}

	if ((resultFX2 < 0) || (resultFX3 < 0)) {
		// Free any possible found devices.
		free(foundDavisFX2);
		free(foundDavisFX3);

		// Error code, return right away.
		return (-1);
	}

	size_t resultAll = (size_t)(resultFX2 + resultFX3);

	if (resultAll == 0) {
		// Nothing found. No devices to free.
		return (0);
	}

	// Allocate memory for discovered devices in expected format.
	*discoveredDevices = calloc(resultAll, sizeof(struct caer_device_discovery_result));
	if (*discoveredDevices == NULL) {
		free(foundDavisFX2);
		free(foundDavisFX3);

		return (-1);
	}

	// Transform from generic USB format into device discovery one.
	size_t j = 0;

	caerLogDisable(true);
	for (size_t i = 0; i < (size_t) resultFX2; i++, j++) {
		// This is a DAVIS FX2.
		(*discoveredDevices)[j].deviceType         = CAER_DEVICE_DAVIS_FX2;
		(*discoveredDevices)[j].deviceErrorOpen    = foundDavisFX2[i].errorOpen;
		(*discoveredDevices)[j].deviceErrorVersion = foundDavisFX2[i].errorVersion;
		struct caer_davis_info *davisInfoPtr       = &((*discoveredDevices)[j].deviceInfo.davisInfo);

		davisInfoPtr->deviceUSBBusNumber     = foundDavisFX2[i].busNumber;
		davisInfoPtr->deviceUSBDeviceAddress = foundDavisFX2[i].devAddress;
		strncpy(davisInfoPtr->deviceSerialNumber, foundDavisFX2[i].serialNumber, MAX_SERIAL_NUMBER_LENGTH + 1);

		// Reopen DAVIS device to get additional info, if possible at all.
		if (!foundDavisFX2[i].errorOpen && !foundDavisFX2[i].errorVersion) {
			caerDeviceHandle davis
				= davisOpenFX2(0, davisInfoPtr->deviceUSBBusNumber, davisInfoPtr->deviceUSBDeviceAddress, NULL);
			if (davis != NULL) {
				*davisInfoPtr = caerDavisInfoGet(davis);

				davisClose(davis);
			}
		}

		// Set/Reset to invalid values, not part of discovery.
		davisInfoPtr->deviceID     = -1;
		davisInfoPtr->deviceString = NULL;
	}

	for (size_t i = 0; i < (size_t) resultFX3; i++, j++) {
		// This is a DAVIS FX3.
		(*discoveredDevices)[j].deviceType         = CAER_DEVICE_DAVIS_FX3;
		(*discoveredDevices)[j].deviceErrorOpen    = foundDavisFX3[i].errorOpen;
		(*discoveredDevices)[j].deviceErrorVersion = foundDavisFX3[i].errorVersion;
		struct caer_davis_info *davisInfoPtr       = &((*discoveredDevices)[j].deviceInfo.davisInfo);

		davisInfoPtr->deviceUSBBusNumber     = foundDavisFX3[i].busNumber;
		davisInfoPtr->deviceUSBDeviceAddress = foundDavisFX3[i].devAddress;
		strncpy(davisInfoPtr->deviceSerialNumber, foundDavisFX3[i].serialNumber, MAX_SERIAL_NUMBER_LENGTH + 1);

		// Reopen DAVIS device to get additional info, if possible at all.
		if (!foundDavisFX3[i].errorOpen && !foundDavisFX3[i].errorVersion) {
			caerDeviceHandle davis
				= davisOpenFX3(0, davisInfoPtr->deviceUSBBusNumber, davisInfoPtr->deviceUSBDeviceAddress, NULL);
			if (davis != NULL) {
				*davisInfoPtr = caerDavisInfoGet(davis);

				davisClose(davis);
			}
		}

		// Set/Reset to invalid values, not part of discovery.
		davisInfoPtr->deviceID     = -1;
		davisInfoPtr->deviceString = NULL;
	}
	caerLogDisable(false);

	free(foundDavisFX2);
	free(foundDavisFX3);
	return ((ssize_t) resultAll);
}

static inline void apsInitFrame(davisHandle handle) {
	davisCommonState state = &handle->state;

	state->aps.ignoreEvents                      = false;
	state->aps.autoExposure.tmpData              = 0;
	state->aps.autoExposure.currentFrameExposure = 0;
	state->aps.roi.tmpData                       = 0;
	state->aps.roi.update                        = 0;

	state->aps.currentReadoutType = APS_READOUT_RESET;
	for (size_t i = 0; i < APS_READOUT_TYPES_NUM; i++) {
		state->aps.countX[i] = 0;
		state->aps.countY[i] = 0;
	}

	// Write out start of frame timestamp.
	state->aps.frame.tsStartFrame = state->timestamps.current;

	// Send APS info event out (as special event).
	if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
			(size_t) state->currentPackets.specialPosition, 1, handle)) {
		caerSpecialEvent currentSpecialEvent
			= caerSpecialEventPacketGetEvent(state->currentPackets.special, state->currentPackets.specialPosition);
		caerSpecialEventSetTimestamp(currentSpecialEvent, state->timestamps.current);
		caerSpecialEventSetType(currentSpecialEvent, APS_FRAME_START);
		caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
		state->currentPackets.specialPosition++;
	}
}

static inline void apsROIUpdateSizes(davisCommonState state) {
	// Calculate APS ROI sizes.
	uint16_t startColumn = state->aps.roi.positionX;
	uint16_t startRow    = state->aps.roi.positionY;
	uint16_t endColumn   = state->aps.roi.sizeX;
	uint16_t endRow      = state->aps.roi.sizeY;

	// Position is already set to startCol/Row, so we don't have to reset
	// it here. We only have to calculate size from start and end Col/Row.
	state->aps.roi.sizeX = U16T(endColumn + 1 - startColumn);
	state->aps.roi.sizeY = U16T(endRow + 1 - startRow);

	if (state->aps.invertXY) {
		state->aps.expectedCountX = state->aps.roi.sizeY;
		state->aps.expectedCountY = state->aps.roi.sizeX;
	}
	else {
		state->aps.expectedCountX = state->aps.roi.sizeX;
		state->aps.expectedCountY = state->aps.roi.sizeY;
	}
}

static inline void apsUpdateFrame(davisHandle handle, uint16_t data) {
	davisCommonState state = &handle->state;

	uint16_t xPos = (state->aps.flipX)
						? (U16T(state->aps.expectedCountX - 1 - state->aps.countX[state->aps.currentReadoutType]))
						: (state->aps.countX[state->aps.currentReadoutType]);
	uint16_t yPos = (state->aps.flipY)
						? (U16T(state->aps.expectedCountY - 1 - state->aps.countY[state->aps.currentReadoutType]))
						: (state->aps.countY[state->aps.currentReadoutType]);

	if (IS_DAVIS640H(handle->info.chipID)) {
		yPos = U16T(yPos + state->aps.cDavisSupport.offset);
	}

	if (state->aps.invertXY) {
		SWAP_VAR(uint16_t, xPos, yPos);
	}

	size_t pixelPosition = (size_t)(yPos * state->aps.roi.sizeX) + xPos;

	// Standard CDS support.
	bool isCDavisGS = (IS_DAVIS640H(handle->info.chipID) && state->aps.globalShutter);

	if (((state->aps.currentReadoutType == APS_READOUT_RESET) && (!isCDavisGS))
		|| ((state->aps.currentReadoutType == APS_READOUT_SIGNAL) && isCDavisGS)) {
		state->aps.frame.pixels[pixelPosition] = data;
	}
	else {
		uint16_t resetValue  = 0;
		uint16_t signalValue = 0;

		if (isCDavisGS) {
			// DAVIS640H GS has inverted samples, signal read comes first
			// and was stored above inside state->aps.currentResetFrame.
			resetValue  = data;
			signalValue = state->aps.frame.pixels[pixelPosition];
		}
		else {
			resetValue  = state->aps.frame.pixels[pixelPosition];
			signalValue = data;
		}

		int32_t pixelValue = 0;

		if ((resetValue < 384) || (signalValue == 0)) {
			// If the signal value is 0, that is only possible if the camera
			// has seen tons of light. In that case, the photo-diode current
			// may be greater than the reset current, and the reset value
			// never goes back up fully, which results in black spots where
			// there is too much light. This confuses algorithms, so we filter
			// this out here by setting the pixel to white in that case.
			// Another effect of the same thing is the reset value not going
			// back up to a decent value, so we also filter that out here.
			pixelValue = 1023;
		}
		else {
			// Do CDS.
			pixelValue = resetValue - signalValue;

			// Check for underflow.
			pixelValue = (pixelValue < 0) ? (0) : (pixelValue);

			// Check for overflow.
			pixelValue = (pixelValue > 1023) ? (1023) : (pixelValue);
		}

		// Normalize the ADC value to 16bit generic depth. This depends on ADC used.
		pixelValue = pixelValue << (16 - APS_ADC_DEPTH);

		state->aps.frame.pixels[pixelPosition] = htole16(U16T(pixelValue));
	}

	// RGB support: first 320 pixels are even, then odd.
	if (IS_DAVIS640H(handle->info.chipID)) {
		if (state->aps.cDavisSupport.offsetDirection == 0) { // Increasing
			state->aps.cDavisSupport.offset++;

			if (state->aps.cDavisSupport.offset == 321) {
				// Switch to decreasing after last even pixel.
				state->aps.cDavisSupport.offsetDirection = 1;
				state->aps.cDavisSupport.offset          = 318;
			}
		}
		else { // Decreasing
			state->aps.cDavisSupport.offset = I16T(state->aps.cDavisSupport.offset - 3);
		}
	}

// Separate debug support.
#if APS_DEBUG_FRAME == 1
	// Check for overflow.
	data = (data > 1023) ? (1023) : (data);

	// Normalize the ADC value to 16bit generic depth. This depends on ADC used.
	data = U16T(data << (16 - APS_ADC_DEPTH));

	// Reset read, put into resetPixels here.
	if (state->aps.currentReadoutType == APS_READOUT_RESET) {
		state->aps.frame.resetPixels[pixelPosition] = data;
	}

	// Signal read, put into pixels here.
	if (state->aps.currentReadoutType == APS_READOUT_SIGNAL) {
		state->aps.frame.signalPixels[pixelPosition] = data;
	}

	davisLog(CAER_LOG_DEBUG, handle,
		"APS ADC Sample: column=%" PRIu16 ", row=%" PRIu16 ", index=%zu, data=%" PRIu16 ".",
		state->aps.countX[state->aps.currentReadoutType], state->aps.countY[state->aps.currentReadoutType],
		pixelPosition, data);
#endif
}

static inline bool apsEndFrame(davisHandle handle) {
	davisCommonState state = &handle->state;

	bool validFrame = true;

	for (size_t i = 0; i < APS_READOUT_TYPES_NUM; i++) {
		davisLog(CAER_LOG_DEBUG, handle, "APS Frame End: CountX[%zu] is %d.", i, state->aps.countX[i]);

		if (state->aps.countX[i] != state->aps.expectedCountX) {
			davisLog(CAER_LOG_ERROR, handle, "APS Frame End - %zu: wrong column count %d detected, expected %d.", i,
				state->aps.countX[i], state->aps.expectedCountX);
			validFrame = false;
		}
	}

	// Send APS info event out (as special event).
	if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
			(size_t) state->currentPackets.specialPosition, 1, handle)) {
		caerSpecialEvent currentSpecialEvent
			= caerSpecialEventPacketGetEvent(state->currentPackets.special, state->currentPackets.specialPosition);
		caerSpecialEventSetTimestamp(currentSpecialEvent, state->timestamps.current);
		caerSpecialEventSetType(currentSpecialEvent, APS_FRAME_END);
		caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
		state->currentPackets.specialPosition++;
	}

	return (validFrame);
}

static inline float calculateIMUAccelScale(uint8_t imuAccelScale) {
	// Accelerometer scale is:
	// 0 - +-2 g - 16384 LSB/g
	// 1 - +-4 g - 8192 LSB/g
	// 2 - +-8 g - 4096 LSB/g
	// 3 - +-16 g - 2048 LSB/g
	float accelScale = 65536.0F / (float) U32T(4 * (1 << imuAccelScale));

	return (accelScale);
}

static inline float calculateIMUGyroScale(uint8_t imuGyroScale) {
	// Gyroscope scale is:
	// 0 - +-250 °/s - 131 LSB/°/s
	// 1 - +-500 °/s - 65.5 LSB/°/s
	// 2 - +-1000 °/s - 32.8 LSB/°/s
	// 3 - +-2000 °/s - 16.4 LSB/°/s
	float gyroScale = 65536.0F / (float) U32T(500 * (1 << imuGyroScale));

	return (gyroScale);
}

static inline void freeAllDataMemory(davisCommonState state) {
	dataExchangeDestroy(&state->dataExchange);

	// Since the current event packets aren't necessarily
	// already assigned to the current packet container, we
	// free them separately from it.
	if (state->currentPackets.polarity != NULL) {
		free(&state->currentPackets.polarity->packetHeader);
		state->currentPackets.polarity = NULL;

		containerGenerationSetPacket(&state->container, POLARITY_EVENT, NULL);
	}

	if (state->currentPackets.special != NULL) {
		free(&state->currentPackets.special->packetHeader);
		state->currentPackets.special = NULL;

		containerGenerationSetPacket(&state->container, SPECIAL_EVENT, NULL);
	}

	if (state->currentPackets.frame != NULL) {
		free(&state->currentPackets.frame->packetHeader);
		state->currentPackets.frame = NULL;

		containerGenerationSetPacket(&state->container, FRAME_EVENT, NULL);
	}

	if (state->currentPackets.imu6 != NULL) {
		free(&state->currentPackets.imu6->packetHeader);
		state->currentPackets.imu6 = NULL;

		containerGenerationSetPacket(&state->container, IMU6_EVENT, NULL);
	}

	containerGenerationDestroy(&state->container);

	if (state->aps.frame.pixels != NULL) {
		free(state->aps.frame.pixels);
		state->aps.frame.pixels = NULL;
	}

	if (state->aps.frame.resetPixels != NULL) {
		free(state->aps.frame.resetPixels);
		state->aps.frame.resetPixels = NULL;
	}

	if (state->aps.frame.signalPixels != NULL) {
		free(state->aps.frame.signalPixels);
		state->aps.frame.signalPixels = NULL;
	}
}

caerDeviceHandle davisOpenAll(
	uint16_t deviceID, uint8_t busNumberRestrict, uint8_t devAddressRestrict, const char *serialNumberRestrict) {
	return (
		davisOpenInternal(CAER_DEVICE_DAVIS, deviceID, busNumberRestrict, devAddressRestrict, serialNumberRestrict));
}

caerDeviceHandle davisOpenFX2(
	uint16_t deviceID, uint8_t busNumberRestrict, uint8_t devAddressRestrict, const char *serialNumberRestrict) {
	return (davisOpenInternal(
		CAER_DEVICE_DAVIS_FX2, deviceID, busNumberRestrict, devAddressRestrict, serialNumberRestrict));
}

caerDeviceHandle davisOpenFX3(
	uint16_t deviceID, uint8_t busNumberRestrict, uint8_t devAddressRestrict, const char *serialNumberRestrict) {
	return (davisOpenInternal(
		CAER_DEVICE_DAVIS_FX3, deviceID, busNumberRestrict, devAddressRestrict, serialNumberRestrict));
}

static caerDeviceHandle davisOpenInternal(uint16_t deviceType, uint16_t deviceID, uint8_t busNumberRestrict,
	uint8_t devAddressRestrict, const char *serialNumberRestrict) {
	errno = 0;

	caerLog(CAER_LOG_DEBUG, __func__, "Initializing %s.", DAVIS_DEVICE_NAME);

	davisHandle handle = calloc(1, sizeof(*handle));
	if (handle == NULL) {
		// Failed to allocate memory for device handle!
		caerLog(CAER_LOG_CRITICAL, __func__, "Failed to allocate memory for device handle.");
		errno = CAER_ERROR_MEMORY_ALLOCATION;
		return (NULL);
	}

	// Set main deviceType correctly right away.
	handle->deviceType = deviceType;

	davisCommonState state = &handle->state;

	// Initialize state variables to default values (if not zero, taken care of by calloc above).
	dataExchangeSettingsInit(&state->dataExchange);

	// Packet settings (size (in events) and time interval (in µs)).
	containerGenerationSettingsInit(&state->container);

	// Logging settings (initialize to global log-level).
	enum caer_log_level globalLogLevel = caerLogLevelGet();
	atomic_store(&state->deviceLogLevel, globalLogLevel);
	atomic_store(&handle->usbState.usbLogLevel, globalLogLevel);

	// Set device thread name. Maximum length of 15 chars due to Linux limitations.
	char usbThreadName[MAX_THREAD_NAME_LENGTH + 1];
	snprintf(usbThreadName, MAX_THREAD_NAME_LENGTH + 1, "%s ID-%" PRIu16, DAVIS_DEVICE_NAME, deviceID);
	usbThreadName[MAX_THREAD_NAME_LENGTH] = '\0';

	usbSetThreadName(&handle->usbState, usbThreadName);
	handle->info.deviceString = usbThreadName; // Temporary, until replaced by full string.

	// Try to open a DAVIS device on a specific USB port.
	bool deviceFound        = false;
	struct usb_info usbInfo = {0, 0, "", false, false, 0, 0};

	if ((deviceType == CAER_DEVICE_DAVIS) || (deviceType == CAER_DEVICE_DAVIS_FX2)) {
		deviceFound = usbDeviceOpen(&handle->usbState, USB_DEFAULT_DEVICE_VID, DAVIS_FX2_DEVICE_PID, busNumberRestrict,
			devAddressRestrict, serialNumberRestrict, DAVIS_FX2_REQUIRED_LOGIC_REVISION,
			DAVIS_FX2_REQUIRED_FIRMWARE_VERSION, &usbInfo);
	}

	if ((!deviceFound) && ((deviceType == CAER_DEVICE_DAVIS) || (deviceType == CAER_DEVICE_DAVIS_FX3))) {
		deviceFound = usbDeviceOpen(&handle->usbState, USB_DEFAULT_DEVICE_VID, DAVIS_FX3_DEVICE_PID, busNumberRestrict,
			devAddressRestrict, serialNumberRestrict, DAVIS_FX3_REQUIRED_LOGIC_REVISION,
			DAVIS_FX3_REQUIRED_FIRMWARE_VERSION, &usbInfo);

		if (deviceFound) {
			handle->fx3Support.enabled = true;
		}
	}

	if (!deviceFound) {
		if (errno == CAER_ERROR_OPEN_ACCESS) {
			davisLog(CAER_LOG_CRITICAL, handle, "Failed to open device, no matching device could be found or opened.");
		}
		else {
			davisLog(CAER_LOG_CRITICAL, handle,
				"Failed to open device, see above log message for more information (errno=%d).", errno);
		}

		free(handle);

		// errno set by usbDeviceOpen().
		return (NULL);
	}

	char *usbInfoString = usbGenerateDeviceString(usbInfo, DAVIS_DEVICE_NAME, deviceID);
	if (usbInfoString == NULL) {
		davisLog(CAER_LOG_CRITICAL, handle, "Failed to generate USB information string.");

		usbDeviceClose(&handle->usbState);
		free(handle);

		errno = CAER_ERROR_MEMORY_ALLOCATION;
		return (NULL);
	}

	// Setup USB.
	usbSetDataCallback(&handle->usbState, &davisEventTranslator, handle);
	usbSetDataEndpoint(&handle->usbState, USB_DEFAULT_DATA_ENDPOINT);
	usbSetTransfersNumber(&handle->usbState, 8);
	usbSetTransfersSize(&handle->usbState, 8192);

	// Start USB handling thread.
	if (!usbThreadStart(&handle->usbState)) {
		usbDeviceClose(&handle->usbState);
		free(usbInfoString);
		free(handle);

		errno = CAER_ERROR_COMMUNICATION;
		return (NULL);
	}

	// Populate info variables based on data from device.
	uint32_t param32 = 0;

	handle->info.deviceID = I16T(deviceID);
	strncpy(handle->info.deviceSerialNumber, usbInfo.serialNumber, MAX_SERIAL_NUMBER_LENGTH + 1);
	handle->info.deviceUSBBusNumber     = usbInfo.busNumber;
	handle->info.deviceUSBDeviceAddress = usbInfo.devAddress;
	handle->info.deviceString           = usbInfoString;

	handle->info.firmwareVersion = usbInfo.firmwareVersion;
	handle->info.logicVersion    = usbInfo.logicVersion;

	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_CHIP_IDENTIFIER, &param32);
	handle->info.chipID = I16T(param32);
	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_DEVICE_IS_MASTER, &param32);
	handle->info.deviceIsMaster = param32;
	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_LOGIC_CLOCK, &param32);
	state->deviceClocks.logicClock = U16T(param32);
	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_ADC_CLOCK, &param32);
	state->deviceClocks.adcClock = U16T(param32);
	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_USB_CLOCK, &param32);
	state->deviceClocks.usbClock = U16T(param32);
	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_CLOCK_DEVIATION, &param32);
	state->deviceClocks.clockDeviationFactor = U16T(param32);

	// Calculate actual clock frequencies.
	state->deviceClocks.logicClockActual = (float) ((double) state->deviceClocks.logicClock
													* ((double) state->deviceClocks.clockDeviationFactor / 1000.0));
	state->deviceClocks.adcClockActual   = (float) ((double) state->deviceClocks.adcClock
                                                  * ((double) state->deviceClocks.clockDeviationFactor / 1000.0));
	state->deviceClocks.usbClockActual   = (float) ((double) state->deviceClocks.usbClock
                                                  * ((double) state->deviceClocks.clockDeviationFactor / 1000.0));

	davisLog(CAER_LOG_DEBUG, handle, "Clock frequencies: LOGIC %f, ADC %f, USB %f.",
		(double) state->deviceClocks.logicClockActual, (double) state->deviceClocks.adcClockActual,
		(double) state->deviceClocks.usbClockActual);

	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_HAS_PIXEL_FILTER, &param32);
	handle->info.dvsHasPixelFilter = param32;
	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_HAS_BACKGROUND_ACTIVITY_FILTER, &param32);
	handle->info.dvsHasBackgroundActivityFilter = param32;
	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_HAS_ROI_FILTER, &param32);
	handle->info.dvsHasROIFilter = param32;
	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_HAS_SKIP_FILTER, &param32);
	handle->info.dvsHasSkipFilter = param32;
	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_HAS_POLARITY_FILTER, &param32);
	handle->info.dvsHasPolarityFilter = param32;
	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_HAS_STATISTICS, &param32);
	handle->info.dvsHasStatistics = param32;

	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_COLOR_FILTER, &param32);
	handle->info.apsColorFilter = U8T(param32);
	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_HAS_GLOBAL_SHUTTER, &param32);
	handle->info.apsHasGlobalShutter = param32;

	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_HAS_GENERATOR, &param32);
	handle->info.extInputHasGenerator = param32;

	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_HAS_STATISTICS, &param32);
	handle->info.muxHasStatistics = param32;

	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_SIZE_COLUMNS, &param32);
	state->dvs.sizeX = I16T(param32);
	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_SIZE_ROWS, &param32);
	state->dvs.sizeY = I16T(param32);

	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_ORIENTATION_INFO, &param32);
	state->dvs.invertXY = param32 & 0x04;

	davisLog(CAER_LOG_DEBUG, handle, "DVS Size X: %d, Size Y: %d, Invert: %d.", state->dvs.sizeX, state->dvs.sizeY,
		state->dvs.invertXY);

	if (state->dvs.invertXY) {
		handle->info.dvsSizeX = state->dvs.sizeY;
		handle->info.dvsSizeY = state->dvs.sizeX;
	}
	else {
		handle->info.dvsSizeX = state->dvs.sizeX;
		handle->info.dvsSizeY = state->dvs.sizeY;
	}

	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_SIZE_COLUMNS, &param32);
	state->aps.sizeX = I16T(param32);
	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_SIZE_ROWS, &param32);
	state->aps.sizeY = I16T(param32);

	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_ORIENTATION_INFO, &param32);
	state->aps.invertXY = param32 & 0x04;
	state->aps.flipX    = param32 & 0x02;
	state->aps.flipY    = param32 & 0x01;

	davisLog(CAER_LOG_DEBUG, handle, "APS Size X: %d, Size Y: %d, Invert: %d, Flip X: %d, Flip Y: %d.",
		state->aps.sizeX, state->aps.sizeY, state->aps.invertXY, state->aps.flipX, state->aps.flipY);

	if (state->aps.invertXY) {
		handle->info.apsSizeX = state->aps.sizeY;
		handle->info.apsSizeY = state->aps.sizeX;
	}
	else {
		handle->info.apsSizeX = state->aps.sizeX;
		handle->info.apsSizeY = state->aps.sizeY;
	}

	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_ORIENTATION_INFO, &param32);
	state->imu.flipX = param32 & 0x04;
	state->imu.flipY = param32 & 0x02;
	state->imu.flipZ = param32 & 0x01;

	davisLog(CAER_LOG_DEBUG, handle, "IMU Flip X: %d, Flip Y: %d, Flip Z: %d.", state->imu.flipX, state->imu.flipY,
		state->imu.flipZ);

	// On FX3, start the debug transfers once everything else is ready.
	if (handle->fx3Support.enabled) {
		allocateDebugTransfers(handle);
	}

	davisLog(CAER_LOG_DEBUG, handle, "Initialized device successfully with USB Bus=%" PRIu8 ":Addr=%" PRIu8 ".",
		usbInfo.busNumber, usbInfo.devAddress);

	return ((caerDeviceHandle) handle);
}

bool davisClose(caerDeviceHandle cdh) {
	davisHandle handle = (davisHandle) cdh;

	davisLog(CAER_LOG_DEBUG, handle, "Shutting down ...");

	// Stop debug transfers on FX3 devices.
	if (handle->fx3Support.enabled) {
		cancelAndDeallocateDebugTransfers(handle);
	}

	// Shut down USB handling thread.
	usbThreadStop(&handle->usbState);

	// Finally, close the device fully.
	usbDeviceClose(&handle->usbState);

	davisLog(CAER_LOG_DEBUG, handle, "Shutdown successful.");

	// Free memory.
	free(handle->info.deviceString);
	free(handle);

	return (true);
}

struct caer_davis_info caerDavisInfoGet(caerDeviceHandle cdh) {
	davisHandle handle = (davisHandle) cdh;

	// Check if the pointer is valid.
	if (handle == NULL) {
		struct caer_davis_info emptyInfo = {0, .deviceString = NULL};
		return (emptyInfo);
	}

	// Check if device type is supported.
	if ((handle->deviceType != CAER_DEVICE_DAVIS) && (handle->deviceType != CAER_DEVICE_DAVIS_FX2)
		&& (handle->deviceType != CAER_DEVICE_DAVIS_FX3) && (handle->deviceType != CAER_DEVICE_DAVIS_RPI)) {
		struct caer_davis_info emptyInfo = {0, .deviceString = NULL};
		return (emptyInfo);
	}

	// Return a copy of the device information.
	return (handle->info);
}

bool davisSendDefaultConfig(caerDeviceHandle cdh) {
	// First send default chip/bias config.
	if (!davisSendDefaultChipConfig(cdh)) {
		return (false);
	}

	// Send default FPGA config.
	if (!davisSendDefaultFPGAConfig(cdh)) {
		return (false);
	}

	return (true);
}

static bool davisSendDefaultFPGAConfig(caerDeviceHandle cdh) {
	davisHandle handle = (davisHandle) cdh;

	davisConfigSet(cdh, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_TIMESTAMP_RESET, false);
	davisConfigSet(cdh, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_DROP_EXTINPUT_ON_TRANSFER_STALL, true);
	davisConfigSet(cdh, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_DROP_DVS_ON_TRANSFER_STALL, true);

	davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_WAIT_ON_TRANSFER_STALL, false);
	davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_EXTERNAL_AER_CONTROL, false);
	if (handle->info.dvsHasPixelFilter) {
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_0_ROW, U32T(handle->info.dvsSizeY));
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_0_COLUMN, U32T(handle->info.dvsSizeX));
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_1_ROW, U32T(handle->info.dvsSizeY));
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_1_COLUMN, U32T(handle->info.dvsSizeX));
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_2_ROW, U32T(handle->info.dvsSizeY));
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_2_COLUMN, U32T(handle->info.dvsSizeX));
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_3_ROW, U32T(handle->info.dvsSizeY));
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_3_COLUMN, U32T(handle->info.dvsSizeX));
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_4_ROW, U32T(handle->info.dvsSizeY));
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_4_COLUMN, U32T(handle->info.dvsSizeX));
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_5_ROW, U32T(handle->info.dvsSizeY));
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_5_COLUMN, U32T(handle->info.dvsSizeX));
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_6_ROW, U32T(handle->info.dvsSizeY));
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_6_COLUMN, U32T(handle->info.dvsSizeX));
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_7_ROW, U32T(handle->info.dvsSizeY));
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_7_COLUMN, U32T(handle->info.dvsSizeX));
	}
	if (handle->info.dvsHasBackgroundActivityFilter) {
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_BACKGROUND_ACTIVITY, true);
		davisConfigSet(
			cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_BACKGROUND_ACTIVITY_TIME, 8); // in 250µs blocks (so 2ms)
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_REFRACTORY_PERIOD, false);
		davisConfigSet(
			cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_REFRACTORY_PERIOD_TIME, 1); // in 250µs blocks (so 250µs)
	}
	if (handle->info.dvsHasROIFilter) {
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_ROI_START_COLUMN, 0);
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_ROI_START_ROW, 0);
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_ROI_END_COLUMN, U32T(handle->info.dvsSizeX - 1));
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_ROI_END_ROW, U32T(handle->info.dvsSizeY - 1));
	}
	if (handle->info.dvsHasSkipFilter) {
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_SKIP_EVENTS, false);
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_SKIP_EVENTS_EVERY, 5);
	}
	if (handle->info.dvsHasPolarityFilter) {
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_POLARITY_FLATTEN, false);
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_POLARITY_SUPPRESS, false);
		davisConfigSet(
			cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_POLARITY_SUPPRESS_TYPE, false); // Suppress OFF events.
	}

	davisConfigSet(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_WAIT_ON_TRANSFER_STALL, true);
	davisConfigSet(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_GLOBAL_SHUTTER, handle->info.apsHasGlobalShutter);
	davisConfigSet(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_START_COLUMN_0, 0);
	davisConfigSet(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_START_ROW_0, 0);
	davisConfigSet(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_END_COLUMN_0, U32T(handle->info.apsSizeX - 1));
	davisConfigSet(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_END_ROW_0, U32T(handle->info.apsSizeY - 1));
	davisConfigSet(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_AUTOEXPOSURE, false);
	davisConfigSet(
		cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_EXPOSURE, 4000); // in µs, converted to cycles @ ADCClock later
	davisConfigSet(
		cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_FRAME_INTERVAL, 40000); // in µs, converted to cycles @ ADCClock later

	if (IS_DAVIS640H(handle->info.chipID)) {
		davisConfigSet(cdh, DAVIS_CONFIG_APS, DAVIS640H_CONFIG_APS_TRANSFER, 1500);   // in cycles @ ADCClock
		davisConfigSet(cdh, DAVIS_CONFIG_APS, DAVIS640H_CONFIG_APS_RSFDSETTLE, 900);  // in cycles @ ADCClock
		davisConfigSet(cdh, DAVIS_CONFIG_APS, DAVIS640H_CONFIG_APS_GSPDRESET, 900);   // in cycles @ ADCClock
		davisConfigSet(cdh, DAVIS_CONFIG_APS, DAVIS640H_CONFIG_APS_GSRESETFALL, 900); // in cycles @ ADCClock
		davisConfigSet(cdh, DAVIS_CONFIG_APS, DAVIS640H_CONFIG_APS_GSTXFALL, 900);    // in cycles @ ADCClock
		davisConfigSet(cdh, DAVIS_CONFIG_APS, DAVIS640H_CONFIG_APS_GSFDRESET, 900);   // in cycles @ ADCClock
	}

	davisConfigSet(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_SAMPLE_RATE_DIVIDER, 0);
	davisConfigSet(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_ACCEL_DLPF, 1);
	davisConfigSet(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_ACCEL_FULL_SCALE, 1);
	davisConfigSet(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_GYRO_DLPF, 1);
	davisConfigSet(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_GYRO_FULL_SCALE, 1);

	davisConfigSet(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_DETECT_RISING_EDGES, false);
	davisConfigSet(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_DETECT_FALLING_EDGES, false);
	davisConfigSet(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_DETECT_PULSES, true);
	davisConfigSet(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_DETECT_PULSE_POLARITY, true);
	davisConfigSet(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_DETECT_PULSE_LENGTH,
		10); // in µs, converted to cycles @ LogicClock later

	if (handle->info.extInputHasGenerator) {
		// Disable generator by default. Has to be enabled manually after sendDefaultConfig() by user!
		davisConfigSet(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_RUN_GENERATOR, false);
		davisConfigSet(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_POLARITY, true);
		davisConfigSet(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_INTERVAL,
			10); // in µs, converted to cycles @ LogicClock later
		davisConfigSet(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_LENGTH,
			5); // in µs, converted to cycles @ LogicClock later
		davisConfigSet(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_GENERATE_INJECT_ON_RISING_EDGE, false);
		davisConfigSet(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_GENERATE_INJECT_ON_FALLING_EDGE, false);
	}

	davisConfigSet(cdh, DAVIS_CONFIG_USB, DAVIS_CONFIG_USB_EARLY_PACKET_DELAY, 8); // in 125µs blocks (so 1ms)

	return (true);
}

#define CF_N_TYPE(COARSE, FINE)                                                                      \
	(struct caer_bias_coarsefine) {                                                                  \
		.coarseValue = COARSE, .fineValue = FINE, .enabled = true, .sexN = true, .typeNormal = true, \
		.currentLevelNormal = true                                                                   \
	}

#define CF_P_TYPE(COARSE, FINE)                                                                       \
	(struct caer_bias_coarsefine) {                                                                   \
		.coarseValue = COARSE, .fineValue = FINE, .enabled = true, .sexN = false, .typeNormal = true, \
		.currentLevelNormal = true                                                                    \
	}

#define CF_N_TYPE_CAS(COARSE, FINE)                                                                   \
	(struct caer_bias_coarsefine) {                                                                   \
		.coarseValue = COARSE, .fineValue = FINE, .enabled = true, .sexN = true, .typeNormal = false, \
		.currentLevelNormal = true                                                                    \
	}

/*
 * #define CF_P_TYPE_CAS(COARSE, FINE) (struct caer_bias_coarsefine) \
 *	{ .coarseValue = COARSE, .fineValue = FINE, .enabled = true, .sexN = false, \
 *	.typeNormal = false, .currentLevelNormal = true }
 */

#define CF_N_TYPE_OFF(COARSE, FINE)                                                                   \
	(struct caer_bias_coarsefine) {                                                                   \
		.coarseValue = COARSE, .fineValue = FINE, .enabled = false, .sexN = true, .typeNormal = true, \
		.currentLevelNormal = true                                                                    \
	}

#define CF_P_TYPE_OFF(COARSE, FINE)                                                                    \
	(struct caer_bias_coarsefine) {                                                                    \
		.coarseValue = COARSE, .fineValue = FINE, .enabled = false, .sexN = false, .typeNormal = true, \
		.currentLevelNormal = true                                                                     \
	}

#define SHIFTSOURCE(REF, REG, OPMODE)                                                         \
	(struct caer_bias_shiftedsource) {                                                        \
		.refValue = REF, .regValue = REG, .operatingMode = OPMODE, .voltageLevel = SPLIT_GATE \
	}

#define VDAC(VOLT, CURR)                           \
	(struct caer_bias_vdac) {                      \
		.voltageValue = VOLT, .currentValue = CURR \
	}

static bool davisSendDefaultChipConfig(caerDeviceHandle cdh) {
	davisHandle handle = (davisHandle) cdh;

	// Default bias configuration.
	if (IS_DAVIS240(handle->info.chipID)) {
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_DIFFBN, caerBiasCoarseFineGenerate(CF_N_TYPE(4, 39)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_ONBN, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 255)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_OFFBN, caerBiasCoarseFineGenerate(CF_N_TYPE(4, 0)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_APSCASEPC, caerBiasCoarseFineGenerate(CF_N_TYPE_CAS(5, 185)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_DIFFCASBNC, caerBiasCoarseFineGenerate(CF_N_TYPE_CAS(5, 115)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_APSROSFBN, caerBiasCoarseFineGenerate(CF_N_TYPE(6, 219)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_LOCALBUFBN, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 164)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_PIXINVBN, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 129)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_PRBP, caerBiasCoarseFineGenerate(CF_P_TYPE(2, 58)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_PRSFBP, caerBiasCoarseFineGenerate(CF_P_TYPE(1, 16)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_REFRBP, caerBiasCoarseFineGenerate(CF_P_TYPE(4, 25)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_AEPDBN, caerBiasCoarseFineGenerate(CF_N_TYPE(6, 91)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_LCOLTIMEOUTBN, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 49)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_AEPUXBP, caerBiasCoarseFineGenerate(CF_P_TYPE(4, 80)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_AEPUYBP, caerBiasCoarseFineGenerate(CF_P_TYPE(7, 152)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_IFTHRBN, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 255)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_IFREFRBN, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 255)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_PADFOLLBN, caerBiasCoarseFineGenerate(CF_N_TYPE(7, 215)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_APSOVERFLOWLEVELBN,
			caerBiasCoarseFineGenerate(CF_N_TYPE(6, 253)));

		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_BIASBUFFER, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 254)));

		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_SSP,
			caerBiasShiftedSourceGenerate(SHIFTSOURCE(1, 33, SHIFTED_SOURCE)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_SSN,
			caerBiasShiftedSourceGenerate(SHIFTSOURCE(1, 33, SHIFTED_SOURCE)));
	}

	if (IS_DAVIS128(handle->info.chipID) || IS_DAVIS208(handle->info.chipID) || IS_DAVIS346(handle->info.chipID)
		|| IS_DAVIS640(handle->info.chipID)) {
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_APSOVERFLOWLEVEL, caerBiasVDACGenerate(VDAC(27, 6)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_APSCAS, caerBiasVDACGenerate(VDAC(21, 6)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_ADCREFHIGH, caerBiasVDACGenerate(VDAC(32, 7)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_ADCREFLOW, caerBiasVDACGenerate(VDAC(1, 7)));

		if (IS_DAVIS346(handle->info.chipID) || IS_DAVIS640(handle->info.chipID)) {
			// Only DAVIS346 and 640 have ADC testing.
			davisConfigSet(
				cdh, DAVIS_CONFIG_BIAS, DAVIS346_CONFIG_BIAS_ADCTESTVOLTAGE, caerBiasVDACGenerate(VDAC(21, 7)));
		}

		if (IS_DAVIS208(handle->info.chipID)) {
			davisConfigSet(
				cdh, DAVIS_CONFIG_BIAS, DAVIS208_CONFIG_BIAS_RESETHIGHPASS, caerBiasVDACGenerate(VDAC(63, 7)));
			davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS208_CONFIG_BIAS_REFSS, caerBiasVDACGenerate(VDAC(11, 5)));

			davisConfigSet(
				cdh, DAVIS_CONFIG_BIAS, DAVIS208_CONFIG_BIAS_REGBIASBP, caerBiasCoarseFineGenerate(CF_P_TYPE(5, 20)));
			davisConfigSet(
				cdh, DAVIS_CONFIG_BIAS, DAVIS208_CONFIG_BIAS_REFSSBN, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 20)));
		}

		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_LOCALBUFBN, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 164)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_PADFOLLBN, caerBiasCoarseFineGenerate(CF_N_TYPE_OFF(7, 215)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_DIFFBN, caerBiasCoarseFineGenerate(CF_N_TYPE(4, 39)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_ONBN, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 255)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_OFFBN, caerBiasCoarseFineGenerate(CF_N_TYPE(4, 1)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_PIXINVBN, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 129)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_PRBP, caerBiasCoarseFineGenerate(CF_P_TYPE(2, 58)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_PRSFBP, caerBiasCoarseFineGenerate(CF_P_TYPE(1, 16)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_REFRBP, caerBiasCoarseFineGenerate(CF_P_TYPE(4, 25)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_READOUTBUFBP, caerBiasCoarseFineGenerate(CF_P_TYPE(6, 20)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_APSROSFBN, caerBiasCoarseFineGenerate(CF_N_TYPE(6, 219)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_ADCCOMPBP, caerBiasCoarseFineGenerate(CF_P_TYPE(5, 20)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_COLSELLOWBN, caerBiasCoarseFineGenerate(CF_N_TYPE(0, 1)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_DACBUFBP, caerBiasCoarseFineGenerate(CF_P_TYPE(6, 60)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_LCOLTIMEOUTBN, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 49)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_AEPDBN, caerBiasCoarseFineGenerate(CF_N_TYPE(6, 91)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_AEPUXBP, caerBiasCoarseFineGenerate(CF_P_TYPE(4, 80)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_AEPUYBP, caerBiasCoarseFineGenerate(CF_P_TYPE(7, 152)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_IFREFRBN, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 255)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_IFTHRBN, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 255)));

		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_BIASBUFFER, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 254)));

		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_SSP,
			caerBiasShiftedSourceGenerate(SHIFTSOURCE(1, 33, SHIFTED_SOURCE)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_SSN,
			caerBiasShiftedSourceGenerate(SHIFTSOURCE(1, 33, SHIFTED_SOURCE)));

		// This comes last here so it overrides previous settings for 640 only.
		if (IS_DAVIS640(handle->info.chipID)) {
			// Slow down pixels for big 640x480 array, to avoid overwhelming the AER bus.
			davisConfigSet(
				cdh, DAVIS_CONFIG_BIAS, DAVIS640_CONFIG_BIAS_PRBP, caerBiasCoarseFineGenerate(CF_P_TYPE(2, 3)));
			davisConfigSet(
				cdh, DAVIS_CONFIG_BIAS, DAVIS640_CONFIG_BIAS_PRSFBP, caerBiasCoarseFineGenerate(CF_P_TYPE(1, 1)));
			davisConfigSet(
				cdh, DAVIS_CONFIG_BIAS, DAVIS640_CONFIG_BIAS_ONBN, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 155)));
			davisConfigSet(
				cdh, DAVIS_CONFIG_BIAS, DAVIS640_CONFIG_BIAS_OFFBN, caerBiasCoarseFineGenerate(CF_N_TYPE(1, 4)));

			davisConfigSet(
				cdh, DAVIS_CONFIG_BIAS, DAVIS640_CONFIG_BIAS_BIASBUFFER, caerBiasCoarseFineGenerate(CF_N_TYPE(6, 125)));
		}
	}

	if (IS_DAVIS640H(handle->info.chipID)) {
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_APSCAS, caerBiasVDACGenerate(VDAC(21, 4)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_OVG1LO, caerBiasVDACGenerate(VDAC(63, 4)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_OVG2LO, caerBiasVDACGenerate(VDAC(0, 0)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_TX2OVG2HI, caerBiasVDACGenerate(VDAC(63, 0)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_GND07, caerBiasVDACGenerate(VDAC(13, 4)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_ADCTESTVOLTAGE, caerBiasVDACGenerate(VDAC(21, 0)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_ADCREFHIGH, caerBiasVDACGenerate(VDAC(46, 7)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_ADCREFLOW, caerBiasVDACGenerate(VDAC(3, 7)));

		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_IFREFRBN, caerBiasCoarseFineGenerate(CF_N_TYPE_OFF(5, 255)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_IFTHRBN, caerBiasCoarseFineGenerate(CF_N_TYPE_OFF(5, 255)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_LOCALBUFBN,
			caerBiasCoarseFineGenerate(CF_N_TYPE_OFF(5, 164)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_PADFOLLBN, caerBiasCoarseFineGenerate(CF_N_TYPE_OFF(7, 209)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_PIXINVBN, caerBiasCoarseFineGenerate(CF_N_TYPE(4, 164)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_DIFFBN, caerBiasCoarseFineGenerate(CF_N_TYPE(3, 75)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_ONBN, caerBiasCoarseFineGenerate(CF_N_TYPE(6, 95)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_OFFBN, caerBiasCoarseFineGenerate(CF_N_TYPE(2, 41)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_PRBP, caerBiasCoarseFineGenerate(CF_P_TYPE(1, 88)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_PRSFBP, caerBiasCoarseFineGenerate(CF_P_TYPE(1, 173)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_REFRBP, caerBiasCoarseFineGenerate(CF_P_TYPE(2, 62)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_ARRAYBIASBUFFERBN,
			caerBiasCoarseFineGenerate(CF_N_TYPE(6, 128)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_ARRAYLOGICBUFFERBN,
			caerBiasCoarseFineGenerate(CF_N_TYPE(5, 255)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_FALLTIMEBN, caerBiasCoarseFineGenerate(CF_N_TYPE(7, 41)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_RISETIMEBP, caerBiasCoarseFineGenerate(CF_P_TYPE(6, 162)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_READOUTBUFBP,
			caerBiasCoarseFineGenerate(CF_P_TYPE_OFF(6, 20)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_APSROSFBN, caerBiasCoarseFineGenerate(CF_N_TYPE(7, 82)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_ADCCOMPBP, caerBiasCoarseFineGenerate(CF_P_TYPE(4, 159)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_DACBUFBP, caerBiasCoarseFineGenerate(CF_P_TYPE(6, 194)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_LCOLTIMEOUTBN, caerBiasCoarseFineGenerate(CF_N_TYPE(5, 49)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_AEPDBN, caerBiasCoarseFineGenerate(CF_N_TYPE(6, 91)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_AEPUXBP, caerBiasCoarseFineGenerate(CF_P_TYPE(4, 80)));
		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_AEPUYBP, caerBiasCoarseFineGenerate(CF_P_TYPE(7, 152)));

		davisConfigSet(
			cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_BIASBUFFER, caerBiasCoarseFineGenerate(CF_N_TYPE(6, 251)));

		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_SSP,
			caerBiasShiftedSourceGenerate(SHIFTSOURCE(1, 33, TIED_TO_RAIL)));
		davisConfigSet(cdh, DAVIS_CONFIG_BIAS, DAVIS640H_CONFIG_BIAS_SSN,
			caerBiasShiftedSourceGenerate(SHIFTSOURCE(2, 33, SHIFTED_SOURCE)));
	}

	// Default chip configuration.
	davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_DIGITALMUX0, 0);
	davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_DIGITALMUX1, 0);
	davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_DIGITALMUX2, 0);
	davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_DIGITALMUX3, 0);
	davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_ANALOGMUX0, 0);
	davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_ANALOGMUX1, 0);
	davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_ANALOGMUX2, 0);
	davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_BIASMUX0, 0);
	davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_RESETCALIBNEURON, true);
	davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_TYPENCALIBNEURON, false);
	davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_RESETTESTPIXEL, true);
	davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_AERNAROW, false); // Use nArow in the AER state machine.
	davisConfigSet(
		cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_USEAOUT, false); // Enable analog pads for aMUX output (testing).

	// No GlobalShutter flag set here, we already set it above for the APS GS flag,
	// and that is automatically propagated to the chip config shift-register in
	// configSet() and kept in sync.

	// Special extra pixels control for DAVIS240 A/B.
	davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS240_CONFIG_CHIP_SPECIALPIXELCONTROL, false);

	// Select which gray counter to use with the internal ADC: '0' means the external gray counter is used, which
	// has to be supplied off-chip. '1' means the on-chip gray counter is used instead.
	davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_SELECTGRAYCOUNTER, 1);

	// Test ADC functionality: if true, the ADC takes its input voltage not from the pixel, but from the
	// VDAC 'AdcTestVoltage'. If false, the voltage comes from the pixels.
	davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS346_CONFIG_CHIP_TESTADC, false);

	if (IS_DAVIS208(handle->info.chipID)) {
		davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS208_CONFIG_CHIP_SELECTPREAMPAVG, false);
		davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS208_CONFIG_CHIP_SELECTBIASREFSS, false);
		davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS208_CONFIG_CHIP_SELECTSENSE, true);
		davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS208_CONFIG_CHIP_SELECTPOSFB, false);
		davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS208_CONFIG_CHIP_SELECTHIGHPASS, false);
	}

	if (IS_DAVIS640H(handle->info.chipID)) {
		davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS640H_CONFIG_CHIP_ADJUSTOVG1LO, true);
		davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS640H_CONFIG_CHIP_ADJUSTOVG2LO, false);
		davisConfigSet(cdh, DAVIS_CONFIG_CHIP, DAVIS640H_CONFIG_CHIP_ADJUSTTX2OVG2HI, false);
	}

	return (true);
}

bool davisConfigSet(caerDeviceHandle cdh, int8_t modAddr, uint8_t paramAddr, uint32_t param) {
	davisHandle handle     = (davisHandle) cdh;
	davisCommonState state = &handle->state;

	switch (modAddr) {
		case CAER_HOST_CONFIG_USB:
			return (usbConfigSet(&handle->usbState, paramAddr, param));
			break;

		case CAER_HOST_CONFIG_DATAEXCHANGE:
			return (dataExchangeConfigSet(&state->dataExchange, paramAddr, param));
			break;

		case CAER_HOST_CONFIG_PACKETS:
			return (containerGenerationConfigSet(&state->container, paramAddr, param));
			break;

		case CAER_HOST_CONFIG_LOG:
			switch (paramAddr) {
				case CAER_HOST_CONFIG_LOG_LEVEL:
					atomic_store(&state->deviceLogLevel, U8T(param));

					// Set USB log-level to this value too.
					atomic_store(&handle->usbState.usbLogLevel, U8T(param));
					break;

				default:
					return (false);
					break;
			}
			break;

		case DAVIS_CONFIG_MUX:
			switch (paramAddr) {
				case DAVIS_CONFIG_MUX_RUN:
				case DAVIS_CONFIG_MUX_TIMESTAMP_RUN:
				case DAVIS_CONFIG_MUX_RUN_CHIP:
				case DAVIS_CONFIG_MUX_DROP_EXTINPUT_ON_TRANSFER_STALL:
				case DAVIS_CONFIG_MUX_DROP_DVS_ON_TRANSFER_STALL:
					return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_MUX, paramAddr, param));
					break;

				case DAVIS_CONFIG_MUX_TIMESTAMP_RESET: {
					// Use multi-command VR for more efficient implementation of reset,
					// that also guarantees returning to the default state.
					if (param) {
						struct spi_config_params spiMultiConfig[2];

						spiMultiConfig[0].moduleAddr = DAVIS_CONFIG_MUX;
						spiMultiConfig[0].paramAddr  = DAVIS_CONFIG_MUX_TIMESTAMP_RESET;
						spiMultiConfig[0].param      = true;

						spiMultiConfig[1].moduleAddr = DAVIS_CONFIG_MUX;
						spiMultiConfig[1].paramAddr  = DAVIS_CONFIG_MUX_TIMESTAMP_RESET;
						spiMultiConfig[1].param      = false;

						return (spiConfigSendMultiple(&handle->usbState, spiMultiConfig, 2));
					}
					break;
				}

				default:
					return (false);
					break;
			}
			break;

		case DAVIS_CONFIG_DVS:
			switch (paramAddr) {
				case DAVIS_CONFIG_DVS_RUN:
				case DAVIS_CONFIG_DVS_WAIT_ON_TRANSFER_STALL:
				case DAVIS_CONFIG_DVS_EXTERNAL_AER_CONTROL:
					return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_DVS, paramAddr, param));
					break;

				case DAVIS_CONFIG_DVS_FILTER_PIXEL_0_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_1_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_2_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_3_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_4_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_5_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_6_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_7_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_0_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_1_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_2_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_3_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_4_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_5_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_6_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_7_COLUMN:
					if (handle->info.dvsHasPixelFilter) {
						return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_FILTER_BACKGROUND_ACTIVITY:
				case DAVIS_CONFIG_DVS_FILTER_BACKGROUND_ACTIVITY_TIME:
				case DAVIS_CONFIG_DVS_FILTER_REFRACTORY_PERIOD:
				case DAVIS_CONFIG_DVS_FILTER_REFRACTORY_PERIOD_TIME:
					if (handle->info.dvsHasBackgroundActivityFilter) {
						return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_FILTER_ROI_START_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_ROI_START_ROW:
				case DAVIS_CONFIG_DVS_FILTER_ROI_END_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_ROI_END_ROW:
					if (handle->info.dvsHasROIFilter) {
						return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_FILTER_SKIP_EVENTS:
				case DAVIS_CONFIG_DVS_FILTER_SKIP_EVENTS_EVERY:
					if (handle->info.dvsHasSkipFilter) {
						return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_FILTER_POLARITY_FLATTEN:
				case DAVIS_CONFIG_DVS_FILTER_POLARITY_SUPPRESS:
				case DAVIS_CONFIG_DVS_FILTER_POLARITY_SUPPRESS_TYPE:
					if (handle->info.dvsHasPolarityFilter) {
						return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_FILTER_PIXEL_AUTO_TRAIN:
					if (handle->info.dvsHasPixelFilter) {
						atomic_store(&handle->state.dvs.pixelFilterAutoTrain.autoTrainRunning, param);
					}
					else {
						return (false);
					}
					break;

				default:
					return (false);
					break;
			}
			break;

		case DAVIS_CONFIG_APS:
			switch (paramAddr) {
				case DAVIS_CONFIG_APS_RUN:
				case DAVIS_CONFIG_APS_WAIT_ON_TRANSFER_STALL:
				case DAVIS_CONFIG_APS_START_COLUMN_0:
				case DAVIS_CONFIG_APS_START_ROW_0:
				case DAVIS_CONFIG_APS_END_COLUMN_0:
				case DAVIS_CONFIG_APS_END_ROW_0:
					return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_APS, paramAddr, param));
					break;

				case DAVIS_CONFIG_APS_EXPOSURE:
					// Exposure and Frame Interval are in µs, must be converted to native FPGA cycles
					// by multiplying with ADC clock value.
					if (!atomic_load(&state->aps.autoExposure.enabled)) {
						state->aps.autoExposure.lastSetExposure = param;

						float exposureCC = roundf((float) param * state->deviceClocks.adcClockActual);
						return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_USB, paramAddr, U32T(exposureCC)));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_APS_FRAME_INTERVAL: {
					// Exposure and Frame Interval are in µs, must be converted to native FPGA cycles
					// by multiplying with ADC clock value.
					float intervalCC = roundf((float) param * state->deviceClocks.adcClockActual);
					return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_USB, paramAddr, U32T(intervalCC)));
					break;
				}

				case DAVIS_CONFIG_APS_GLOBAL_SHUTTER:
					if (handle->info.apsHasGlobalShutter) {
						// Keep in sync with chip config module GlobalShutter parameter.
						if (!spiConfigSend(
								&handle->usbState, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_GLOBAL_SHUTTER, param)) {
							return (false);
						}

						return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_APS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS640H_CONFIG_APS_TRANSFER:
				case DAVIS640H_CONFIG_APS_RSFDSETTLE:
				case DAVIS640H_CONFIG_APS_GSPDRESET:
				case DAVIS640H_CONFIG_APS_GSRESETFALL:
				case DAVIS640H_CONFIG_APS_GSTXFALL:
				case DAVIS640H_CONFIG_APS_GSFDRESET:
					// Support for DAVISRGB extra timing parameters.
					if (IS_DAVIS640H(handle->info.chipID)) {
						return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_APS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_APS_SNAPSHOT: {
					// Use multi-command VR for more efficient implementation of snapshot,
					// that also guarantees returning to the default state (not running).
					if (param) {
						struct spi_config_params spiMultiConfig[2];

						spiMultiConfig[0].moduleAddr = DAVIS_CONFIG_APS;
						spiMultiConfig[0].paramAddr  = DAVIS_CONFIG_APS_RUN;
						spiMultiConfig[0].param      = true;

						spiMultiConfig[1].moduleAddr = DAVIS_CONFIG_APS;
						spiMultiConfig[1].paramAddr  = DAVIS_CONFIG_APS_RUN;
						spiMultiConfig[1].param      = false;

						return (spiConfigSendMultiple(&handle->usbState, spiMultiConfig, 2));
					}
					break;
				}

				case DAVIS_CONFIG_APS_AUTOEXPOSURE:
					atomic_store(&state->aps.autoExposure.enabled, param);
					break;

				default:
					return (false);
					break;
			}
			break;

		case DAVIS_CONFIG_IMU:
			switch (paramAddr) {
				case DAVIS_CONFIG_IMU_RUN_ACCELEROMETER:
				case DAVIS_CONFIG_IMU_RUN_GYROSCOPE:
				case DAVIS_CONFIG_IMU_RUN_TEMPERATURE:
				case DAVIS_CONFIG_IMU_SAMPLE_RATE_DIVIDER:
				case DAVIS_CONFIG_IMU_ACCEL_DLPF:
				case DAVIS_CONFIG_IMU_ACCEL_FULL_SCALE:
				case DAVIS_CONFIG_IMU_GYRO_DLPF:
				case DAVIS_CONFIG_IMU_GYRO_FULL_SCALE:
					return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_IMU, paramAddr, param));
					break;

				default:
					return (false);
					break;
			}
			break;

		case DAVIS_CONFIG_EXTINPUT:
			switch (paramAddr) {
				case DAVIS_CONFIG_EXTINPUT_RUN_DETECTOR:
				case DAVIS_CONFIG_EXTINPUT_DETECT_RISING_EDGES:
				case DAVIS_CONFIG_EXTINPUT_DETECT_FALLING_EDGES:
				case DAVIS_CONFIG_EXTINPUT_DETECT_PULSES:
				case DAVIS_CONFIG_EXTINPUT_DETECT_PULSE_POLARITY:
				case DAVIS_CONFIG_EXTINPUT_DETECT_PULSE_LENGTH:
					return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_EXTINPUT, paramAddr, param));
					break;

				case DAVIS_CONFIG_EXTINPUT_RUN_GENERATOR:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_POLARITY:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_INTERVAL:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_LENGTH:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_INJECT_ON_RISING_EDGE:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_INJECT_ON_FALLING_EDGE:
					if (handle->info.extInputHasGenerator) {
						return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_EXTINPUT, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				default:
					return (false);
					break;
			}
			break;

		case DAVIS_CONFIG_BIAS: // Also DAVIS_CONFIG_CHIP (starts at address 128).
			if (paramAddr < 128) {
				// BIASING (DAVIS_CONFIG_BIAS).
				if (IS_DAVIS240(handle->info.chipID)) {
					// DAVIS240 uses the old bias generator with 22 branches, and uses all of them.
					if (paramAddr < 22) {
						return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_BIAS, paramAddr, param));
					}
				}
				else if (IS_DAVIS128(handle->info.chipID) || IS_DAVIS208(handle->info.chipID)
						 || IS_DAVIS346(handle->info.chipID) || IS_DAVIS640(handle->info.chipID)) {
					// All new DAVISes use the new bias generator with 37 branches.
					switch (paramAddr) {
						// Same and shared between all of the above chips.
						case DAVIS128_CONFIG_BIAS_APSOVERFLOWLEVEL:
						case DAVIS128_CONFIG_BIAS_APSCAS:
						case DAVIS128_CONFIG_BIAS_ADCREFHIGH:
						case DAVIS128_CONFIG_BIAS_ADCREFLOW:
						case DAVIS128_CONFIG_BIAS_LOCALBUFBN:
						case DAVIS128_CONFIG_BIAS_PADFOLLBN:
						case DAVIS128_CONFIG_BIAS_DIFFBN:
						case DAVIS128_CONFIG_BIAS_ONBN:
						case DAVIS128_CONFIG_BIAS_OFFBN:
						case DAVIS128_CONFIG_BIAS_PIXINVBN:
						case DAVIS128_CONFIG_BIAS_PRBP:
						case DAVIS128_CONFIG_BIAS_PRSFBP:
						case DAVIS128_CONFIG_BIAS_REFRBP:
						case DAVIS128_CONFIG_BIAS_READOUTBUFBP:
						case DAVIS128_CONFIG_BIAS_APSROSFBN:
						case DAVIS128_CONFIG_BIAS_ADCCOMPBP:
						case DAVIS128_CONFIG_BIAS_COLSELLOWBN:
						case DAVIS128_CONFIG_BIAS_DACBUFBP:
						case DAVIS128_CONFIG_BIAS_LCOLTIMEOUTBN:
						case DAVIS128_CONFIG_BIAS_AEPDBN:
						case DAVIS128_CONFIG_BIAS_AEPUXBP:
						case DAVIS128_CONFIG_BIAS_AEPUYBP:
						case DAVIS128_CONFIG_BIAS_IFREFRBN:
						case DAVIS128_CONFIG_BIAS_IFTHRBN:
						case DAVIS128_CONFIG_BIAS_BIASBUFFER:
						case DAVIS128_CONFIG_BIAS_SSP:
						case DAVIS128_CONFIG_BIAS_SSN:
							return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_BIAS, paramAddr, param));
							break;

						case DAVIS346_CONFIG_BIAS_ADCTESTVOLTAGE:
							// Only supported by DAVIS346 and DAVIS640 chips.
							if (IS_DAVIS346(handle->info.chipID) || IS_DAVIS640(handle->info.chipID)) {
								return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_BIAS, paramAddr, param));
							}
							break;

						case DAVIS208_CONFIG_BIAS_RESETHIGHPASS:
						case DAVIS208_CONFIG_BIAS_REFSS:
						case DAVIS208_CONFIG_BIAS_REGBIASBP:
						case DAVIS208_CONFIG_BIAS_REFSSBN:
							// Only supported by DAVIS208 chips.
							if (IS_DAVIS208(handle->info.chipID)) {
								return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_BIAS, paramAddr, param));
							}
							break;

						default:
							return (false);
							break;
					}
				}
				else if (IS_DAVIS640H(handle->info.chipID)) {
					// DAVISRGB also uses the 37 branches bias generator, with different values.
					switch (paramAddr) {
						case DAVIS640H_CONFIG_BIAS_APSCAS:
						case DAVIS640H_CONFIG_BIAS_OVG1LO:
						case DAVIS640H_CONFIG_BIAS_OVG2LO:
						case DAVIS640H_CONFIG_BIAS_TX2OVG2HI:
						case DAVIS640H_CONFIG_BIAS_GND07:
						case DAVIS640H_CONFIG_BIAS_ADCTESTVOLTAGE:
						case DAVIS640H_CONFIG_BIAS_ADCREFHIGH:
						case DAVIS640H_CONFIG_BIAS_ADCREFLOW:
						case DAVIS640H_CONFIG_BIAS_IFREFRBN:
						case DAVIS640H_CONFIG_BIAS_IFTHRBN:
						case DAVIS640H_CONFIG_BIAS_LOCALBUFBN:
						case DAVIS640H_CONFIG_BIAS_PADFOLLBN:
						case DAVIS640H_CONFIG_BIAS_PIXINVBN:
						case DAVIS640H_CONFIG_BIAS_DIFFBN:
						case DAVIS640H_CONFIG_BIAS_ONBN:
						case DAVIS640H_CONFIG_BIAS_OFFBN:
						case DAVIS640H_CONFIG_BIAS_PRBP:
						case DAVIS640H_CONFIG_BIAS_PRSFBP:
						case DAVIS640H_CONFIG_BIAS_REFRBP:
						case DAVIS640H_CONFIG_BIAS_ARRAYBIASBUFFERBN:
						case DAVIS640H_CONFIG_BIAS_ARRAYLOGICBUFFERBN:
						case DAVIS640H_CONFIG_BIAS_FALLTIMEBN:
						case DAVIS640H_CONFIG_BIAS_RISETIMEBP:
						case DAVIS640H_CONFIG_BIAS_READOUTBUFBP:
						case DAVIS640H_CONFIG_BIAS_APSROSFBN:
						case DAVIS640H_CONFIG_BIAS_ADCCOMPBP:
						case DAVIS640H_CONFIG_BIAS_DACBUFBP:
						case DAVIS640H_CONFIG_BIAS_LCOLTIMEOUTBN:
						case DAVIS640H_CONFIG_BIAS_AEPDBN:
						case DAVIS640H_CONFIG_BIAS_AEPUXBP:
						case DAVIS640H_CONFIG_BIAS_AEPUYBP:
						case DAVIS640H_CONFIG_BIAS_BIASBUFFER:
						case DAVIS640H_CONFIG_BIAS_SSP:
						case DAVIS640H_CONFIG_BIAS_SSN:
							return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_BIAS, paramAddr, param));
							break;

						default:
							return (false);
							break;
					}
				}
			}
			else {
				// CHIP CONFIGURATION (DAVIS_CONFIG_CHIP).
				switch (paramAddr) {
					// Chip configuration common to all chips.
					case DAVIS128_CONFIG_CHIP_DIGITALMUX0:
					case DAVIS128_CONFIG_CHIP_DIGITALMUX1:
					case DAVIS128_CONFIG_CHIP_DIGITALMUX2:
					case DAVIS128_CONFIG_CHIP_DIGITALMUX3:
					case DAVIS128_CONFIG_CHIP_ANALOGMUX0:
					case DAVIS128_CONFIG_CHIP_ANALOGMUX1:
					case DAVIS128_CONFIG_CHIP_ANALOGMUX2:
					case DAVIS128_CONFIG_CHIP_BIASMUX0:
					case DAVIS128_CONFIG_CHIP_RESETCALIBNEURON:
					case DAVIS128_CONFIG_CHIP_TYPENCALIBNEURON:
					case DAVIS128_CONFIG_CHIP_RESETTESTPIXEL:
					case DAVIS128_CONFIG_CHIP_AERNAROW:
					case DAVIS128_CONFIG_CHIP_USEAOUT:
						return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_CHIP, paramAddr, param));
						break;

					case DAVIS240_CONFIG_CHIP_SPECIALPIXELCONTROL:
						// Only supported by DAVIS240 A/B chips.
						if (IS_DAVIS240A(handle->info.chipID) || IS_DAVIS240B(handle->info.chipID)) {
							return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS128_CONFIG_CHIP_GLOBAL_SHUTTER:
						// Only supported by some chips.
						if (handle->info.apsHasGlobalShutter) {
							// Keep in sync with APS module GlobalShutter parameter.
							if (!spiConfigSend(
									&handle->usbState, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_GLOBAL_SHUTTER, param)) {
								return (false);
							}

							return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS128_CONFIG_CHIP_SELECTGRAYCOUNTER:
						// Only supported by the new DAVIS chips.
						if (IS_DAVIS128(handle->info.chipID) || IS_DAVIS208(handle->info.chipID)
							|| IS_DAVIS346(handle->info.chipID) || IS_DAVIS640(handle->info.chipID)
							|| IS_DAVIS640H(handle->info.chipID)) {
							return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS346_CONFIG_CHIP_TESTADC:
						// Only supported by some of the new DAVIS chips.
						if (IS_DAVIS346(handle->info.chipID) || IS_DAVIS640(handle->info.chipID)
							|| IS_DAVIS640H(handle->info.chipID)) {
							return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS640H_CONFIG_CHIP_ADJUSTOVG1LO:    // Also DAVIS208_CONFIG_CHIP_SELECTPREAMPAVG.
					case DAVIS640H_CONFIG_CHIP_ADJUSTOVG2LO:    // Also DAVIS208_CONFIG_CHIP_SELECTBIASREFSS.
					case DAVIS640H_CONFIG_CHIP_ADJUSTTX2OVG2HI: // Also DAVIS208_CONFIG_CHIP_SELECTSENSE.
						// Only supported by DAVIS208 and DAVISRGB.
						if (IS_DAVIS208(handle->info.chipID) || IS_DAVIS640H(handle->info.chipID)) {
							return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS208_CONFIG_CHIP_SELECTPOSFB:
					case DAVIS208_CONFIG_CHIP_SELECTHIGHPASS:
						// Only supported by DAVIS208.
						if (IS_DAVIS208(handle->info.chipID)) {
							return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					default:
						return (false);
						break;
				}
			}

			return (false);
			break;

		case DAVIS_CONFIG_SYSINFO:
			// No SystemInfo parameters can ever be set!
			return (false);
			break;

		case DAVIS_CONFIG_USB:
			switch (paramAddr) {
				case DAVIS_CONFIG_USB_RUN:
					return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_USB, paramAddr, param));
					break;

				case DAVIS_CONFIG_USB_EARLY_PACKET_DELAY: {
					// Early packet delay is 125µs slices on host, but in cycles
					// @ USB_CLOCK_FREQ on FPGA, so we must multiply here.
					float delayCC = roundf((float) param * 125.0F * state->deviceClocks.usbClockActual);
					return (spiConfigSend(&handle->usbState, DAVIS_CONFIG_USB, paramAddr, U32T(delayCC)));
					break;
				}

				default:
					return (false);
					break;
			}
			break;

		default:
			return (false);
			break;
	}

	return (true);
}

bool davisConfigGet(caerDeviceHandle cdh, int8_t modAddr, uint8_t paramAddr, uint32_t *param) {
	davisHandle handle     = (davisHandle) cdh;
	davisCommonState state = &handle->state;

	switch (modAddr) {
		case CAER_HOST_CONFIG_USB:
			return (usbConfigGet(&handle->usbState, paramAddr, param));
			break;

		case CAER_HOST_CONFIG_DATAEXCHANGE:
			return (dataExchangeConfigGet(&state->dataExchange, paramAddr, param));
			break;

		case CAER_HOST_CONFIG_PACKETS:
			return (containerGenerationConfigGet(&state->container, paramAddr, param));
			break;

		case CAER_HOST_CONFIG_LOG:
			switch (paramAddr) {
				case CAER_HOST_CONFIG_LOG_LEVEL:
					*param = atomic_load(&state->deviceLogLevel);
					break;

				default:
					return (false);
					break;
			}
			break;

		case DAVIS_CONFIG_MUX:
			switch (paramAddr) {
				case DAVIS_CONFIG_MUX_RUN:
				case DAVIS_CONFIG_MUX_TIMESTAMP_RUN:
				case DAVIS_CONFIG_MUX_RUN_CHIP:
				case DAVIS_CONFIG_MUX_DROP_EXTINPUT_ON_TRANSFER_STALL:
				case DAVIS_CONFIG_MUX_DROP_DVS_ON_TRANSFER_STALL:
					return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_MUX, paramAddr, param));
					break;

				case DAVIS_CONFIG_MUX_TIMESTAMP_RESET:
					// Always false because it's an impulse, it resets itself automatically.
					*param = false;
					break;

				case DAVIS_CONFIG_MUX_STATISTICS_EXTINPUT_DROPPED:
				case DAVIS_CONFIG_MUX_STATISTICS_EXTINPUT_DROPPED + 1:
				case DAVIS_CONFIG_MUX_STATISTICS_DVS_DROPPED:
				case DAVIS_CONFIG_MUX_STATISTICS_DVS_DROPPED + 1:
					if (handle->info.muxHasStatistics) {
						return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_MUX, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				default:
					return (false);
					break;
			}
			break;

		case DAVIS_CONFIG_DVS:
			switch (paramAddr) {
				case DAVIS_CONFIG_DVS_RUN:
				case DAVIS_CONFIG_DVS_WAIT_ON_TRANSFER_STALL:
				case DAVIS_CONFIG_DVS_EXTERNAL_AER_CONTROL:
					return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, paramAddr, param));
					break;

				case DAVIS_CONFIG_DVS_FILTER_PIXEL_0_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_1_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_2_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_3_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_4_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_5_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_6_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_7_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_0_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_1_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_2_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_3_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_4_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_5_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_6_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_7_COLUMN:
					if (handle->info.dvsHasPixelFilter) {
						return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_FILTER_BACKGROUND_ACTIVITY:
				case DAVIS_CONFIG_DVS_FILTER_BACKGROUND_ACTIVITY_TIME:
				case DAVIS_CONFIG_DVS_FILTER_REFRACTORY_PERIOD:
				case DAVIS_CONFIG_DVS_FILTER_REFRACTORY_PERIOD_TIME:
					if (handle->info.dvsHasBackgroundActivityFilter) {
						return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_FILTER_ROI_START_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_ROI_START_ROW:
				case DAVIS_CONFIG_DVS_FILTER_ROI_END_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_ROI_END_ROW:
					if (handle->info.dvsHasROIFilter) {
						return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_FILTER_SKIP_EVENTS:
				case DAVIS_CONFIG_DVS_FILTER_SKIP_EVENTS_EVERY:
					if (handle->info.dvsHasSkipFilter) {
						return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_FILTER_POLARITY_FLATTEN:
				case DAVIS_CONFIG_DVS_FILTER_POLARITY_SUPPRESS:
				case DAVIS_CONFIG_DVS_FILTER_POLARITY_SUPPRESS_TYPE:
					if (handle->info.dvsHasPolarityFilter) {
						return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_STATISTICS_EVENTS_ROW:
				case DAVIS_CONFIG_DVS_STATISTICS_EVENTS_ROW + 1:
				case DAVIS_CONFIG_DVS_STATISTICS_EVENTS_COLUMN:
				case DAVIS_CONFIG_DVS_STATISTICS_EVENTS_COLUMN + 1:
				case DAVIS_CONFIG_DVS_STATISTICS_EVENTS_DROPPED:
				case DAVIS_CONFIG_DVS_STATISTICS_EVENTS_DROPPED + 1:
					if (handle->info.dvsHasStatistics) {
						return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_STATISTICS_FILTERED_PIXELS:
				case DAVIS_CONFIG_DVS_STATISTICS_FILTERED_PIXELS + 1:
					if (handle->info.dvsHasStatistics && handle->info.dvsHasPixelFilter) {
						return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_STATISTICS_FILTERED_BACKGROUND_ACTIVITY:
				case DAVIS_CONFIG_DVS_STATISTICS_FILTERED_BACKGROUND_ACTIVITY + 1:
				case DAVIS_CONFIG_DVS_STATISTICS_FILTERED_REFRACTORY_PERIOD:
				case DAVIS_CONFIG_DVS_STATISTICS_FILTERED_REFRACTORY_PERIOD + 1:
					if (handle->info.dvsHasStatistics && handle->info.dvsHasBackgroundActivityFilter) {
						return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_FILTER_PIXEL_AUTO_TRAIN:
					if (handle->info.dvsHasPixelFilter) {
						*param = atomic_load(&handle->state.dvs.pixelFilterAutoTrain.autoTrainRunning);
					}
					else {
						return (false);
					}
					break;

				default:
					return (false);
					break;
			}
			break;

		case DAVIS_CONFIG_APS:
			switch (paramAddr) {
				case DAVIS_CONFIG_APS_RUN:
				case DAVIS_CONFIG_APS_WAIT_ON_TRANSFER_STALL:
				case DAVIS_CONFIG_APS_START_COLUMN_0:
				case DAVIS_CONFIG_APS_END_COLUMN_0:
				case DAVIS_CONFIG_APS_START_ROW_0:
				case DAVIS_CONFIG_APS_END_ROW_0:
					return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_APS, paramAddr, param));
					break;

				case DAVIS_CONFIG_APS_EXPOSURE:
					// Use stored value, no need to call out to USB for this one.
					*param = state->aps.autoExposure.lastSetExposure;
					break;

				case DAVIS_CONFIG_APS_FRAME_INTERVAL: {
					// Frame Interval is in µs, must be converted from native FPGA cycles
					// by dividing with ADC clock value.
					uint32_t cyclesValue = 0;
					if (!spiConfigReceive(&handle->usbState, DAVIS_CONFIG_APS, paramAddr, &cyclesValue)) {
						return (false);
					}

					float intervalCC = roundf((float) cyclesValue / state->deviceClocks.adcClockActual);
					*param           = U32T(intervalCC);

					return (true);
					break;
				}

				case DAVIS_CONFIG_APS_GLOBAL_SHUTTER:
					if (handle->info.apsHasGlobalShutter) {
						return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_APS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS640H_CONFIG_APS_TRANSFER:
				case DAVIS640H_CONFIG_APS_RSFDSETTLE:
				case DAVIS640H_CONFIG_APS_GSPDRESET:
				case DAVIS640H_CONFIG_APS_GSRESETFALL:
				case DAVIS640H_CONFIG_APS_GSTXFALL:
				case DAVIS640H_CONFIG_APS_GSFDRESET:
					// Support for DAVISRGB extra timing parameters.
					if (IS_DAVIS640H(handle->info.chipID)) {
						return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_APS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_APS_SNAPSHOT:
					// Always false because it's an impulse, it resets itself automatically.
					*param = false;
					break;

				case DAVIS_CONFIG_APS_AUTOEXPOSURE:
					*param = atomic_load(&state->aps.autoExposure.enabled);
					break;

				default:
					return (false);
					break;
			}
			break;

		case DAVIS_CONFIG_IMU:
			switch (paramAddr) {
				case DAVIS_CONFIG_IMU_RUN_ACCELEROMETER:
				case DAVIS_CONFIG_IMU_RUN_GYROSCOPE:
				case DAVIS_CONFIG_IMU_RUN_TEMPERATURE:
				case DAVIS_CONFIG_IMU_SAMPLE_RATE_DIVIDER:
				case DAVIS_CONFIG_IMU_ACCEL_DLPF:
				case DAVIS_CONFIG_IMU_ACCEL_FULL_SCALE:
				case DAVIS_CONFIG_IMU_GYRO_DLPF:
				case DAVIS_CONFIG_IMU_GYRO_FULL_SCALE:
					return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_IMU, paramAddr, param));
					break;

				default:
					return (false);
					break;
			}
			break;

		case DAVIS_CONFIG_EXTINPUT:
			switch (paramAddr) {
				case DAVIS_CONFIG_EXTINPUT_RUN_DETECTOR:
				case DAVIS_CONFIG_EXTINPUT_DETECT_RISING_EDGES:
				case DAVIS_CONFIG_EXTINPUT_DETECT_FALLING_EDGES:
				case DAVIS_CONFIG_EXTINPUT_DETECT_PULSES:
				case DAVIS_CONFIG_EXTINPUT_DETECT_PULSE_POLARITY:
				case DAVIS_CONFIG_EXTINPUT_DETECT_PULSE_LENGTH:
					return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_EXTINPUT, paramAddr, param));
					break;

				case DAVIS_CONFIG_EXTINPUT_RUN_GENERATOR:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_POLARITY:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_INTERVAL:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_LENGTH:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_INJECT_ON_RISING_EDGE:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_INJECT_ON_FALLING_EDGE:
					if (handle->info.extInputHasGenerator) {
						return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_EXTINPUT, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				default:
					return (false);
					break;
			}
			break;

		case DAVIS_CONFIG_BIAS: // Also DAVIS_CONFIG_CHIP (starts at address 128).
			if (paramAddr < 128) {
				// BIASING (DAVIS_CONFIG_BIAS).
				if (IS_DAVIS240(handle->info.chipID)) {
					// DAVIS240 uses the old bias generator with 22 branches, and uses all of them.
					if (paramAddr < 22) {
						return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_BIAS, paramAddr, param));
					}
				}
				else if (IS_DAVIS128(handle->info.chipID) || IS_DAVIS208(handle->info.chipID)
						 || IS_DAVIS346(handle->info.chipID) || IS_DAVIS640(handle->info.chipID)) {
					// All new DAVISes use the new bias generator with 37 branches.
					switch (paramAddr) {
						// Same and shared between all of the above chips.
						case DAVIS128_CONFIG_BIAS_APSOVERFLOWLEVEL:
						case DAVIS128_CONFIG_BIAS_APSCAS:
						case DAVIS128_CONFIG_BIAS_ADCREFHIGH:
						case DAVIS128_CONFIG_BIAS_ADCREFLOW:
						case DAVIS128_CONFIG_BIAS_LOCALBUFBN:
						case DAVIS128_CONFIG_BIAS_PADFOLLBN:
						case DAVIS128_CONFIG_BIAS_DIFFBN:
						case DAVIS128_CONFIG_BIAS_ONBN:
						case DAVIS128_CONFIG_BIAS_OFFBN:
						case DAVIS128_CONFIG_BIAS_PIXINVBN:
						case DAVIS128_CONFIG_BIAS_PRBP:
						case DAVIS128_CONFIG_BIAS_PRSFBP:
						case DAVIS128_CONFIG_BIAS_REFRBP:
						case DAVIS128_CONFIG_BIAS_READOUTBUFBP:
						case DAVIS128_CONFIG_BIAS_APSROSFBN:
						case DAVIS128_CONFIG_BIAS_ADCCOMPBP:
						case DAVIS128_CONFIG_BIAS_COLSELLOWBN:
						case DAVIS128_CONFIG_BIAS_DACBUFBP:
						case DAVIS128_CONFIG_BIAS_LCOLTIMEOUTBN:
						case DAVIS128_CONFIG_BIAS_AEPDBN:
						case DAVIS128_CONFIG_BIAS_AEPUXBP:
						case DAVIS128_CONFIG_BIAS_AEPUYBP:
						case DAVIS128_CONFIG_BIAS_IFREFRBN:
						case DAVIS128_CONFIG_BIAS_IFTHRBN:
						case DAVIS128_CONFIG_BIAS_BIASBUFFER:
						case DAVIS128_CONFIG_BIAS_SSP:
						case DAVIS128_CONFIG_BIAS_SSN:
							return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_BIAS, paramAddr, param));
							break;

						case DAVIS346_CONFIG_BIAS_ADCTESTVOLTAGE:
							// Only supported by DAVIS346 and DAVIS640 chips.
							if (IS_DAVIS346(handle->info.chipID) || IS_DAVIS640(handle->info.chipID)) {
								return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_BIAS, paramAddr, param));
							}
							break;

						case DAVIS208_CONFIG_BIAS_RESETHIGHPASS:
						case DAVIS208_CONFIG_BIAS_REFSS:
						case DAVIS208_CONFIG_BIAS_REGBIASBP:
						case DAVIS208_CONFIG_BIAS_REFSSBN:
							// Only supported by DAVIS208 chips.
							if (IS_DAVIS208(handle->info.chipID)) {
								return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_BIAS, paramAddr, param));
							}
							break;

						default:
							return (false);
							break;
					}
				}
				else if (IS_DAVIS640H(handle->info.chipID)) {
					// DAVISRGB also uses the 37 branches bias generator, with different values.
					switch (paramAddr) {
						case DAVIS640H_CONFIG_BIAS_APSCAS:
						case DAVIS640H_CONFIG_BIAS_OVG1LO:
						case DAVIS640H_CONFIG_BIAS_OVG2LO:
						case DAVIS640H_CONFIG_BIAS_TX2OVG2HI:
						case DAVIS640H_CONFIG_BIAS_GND07:
						case DAVIS640H_CONFIG_BIAS_ADCTESTVOLTAGE:
						case DAVIS640H_CONFIG_BIAS_ADCREFHIGH:
						case DAVIS640H_CONFIG_BIAS_ADCREFLOW:
						case DAVIS640H_CONFIG_BIAS_IFREFRBN:
						case DAVIS640H_CONFIG_BIAS_IFTHRBN:
						case DAVIS640H_CONFIG_BIAS_LOCALBUFBN:
						case DAVIS640H_CONFIG_BIAS_PADFOLLBN:
						case DAVIS640H_CONFIG_BIAS_PIXINVBN:
						case DAVIS640H_CONFIG_BIAS_DIFFBN:
						case DAVIS640H_CONFIG_BIAS_ONBN:
						case DAVIS640H_CONFIG_BIAS_OFFBN:
						case DAVIS640H_CONFIG_BIAS_PRBP:
						case DAVIS640H_CONFIG_BIAS_PRSFBP:
						case DAVIS640H_CONFIG_BIAS_REFRBP:
						case DAVIS640H_CONFIG_BIAS_ARRAYBIASBUFFERBN:
						case DAVIS640H_CONFIG_BIAS_ARRAYLOGICBUFFERBN:
						case DAVIS640H_CONFIG_BIAS_FALLTIMEBN:
						case DAVIS640H_CONFIG_BIAS_RISETIMEBP:
						case DAVIS640H_CONFIG_BIAS_READOUTBUFBP:
						case DAVIS640H_CONFIG_BIAS_APSROSFBN:
						case DAVIS640H_CONFIG_BIAS_ADCCOMPBP:
						case DAVIS640H_CONFIG_BIAS_DACBUFBP:
						case DAVIS640H_CONFIG_BIAS_LCOLTIMEOUTBN:
						case DAVIS640H_CONFIG_BIAS_AEPDBN:
						case DAVIS640H_CONFIG_BIAS_AEPUXBP:
						case DAVIS640H_CONFIG_BIAS_AEPUYBP:
						case DAVIS640H_CONFIG_BIAS_BIASBUFFER:
						case DAVIS640H_CONFIG_BIAS_SSP:
						case DAVIS640H_CONFIG_BIAS_SSN:
							return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_BIAS, paramAddr, param));
							break;

						default:
							return (false);
							break;
					}
				}
			}
			else {
				// CHIP CONFIGURATION (DAVIS_CONFIG_CHIP).
				switch (paramAddr) {
					// Chip configuration common to all chips.
					case DAVIS128_CONFIG_CHIP_DIGITALMUX0:
					case DAVIS128_CONFIG_CHIP_DIGITALMUX1:
					case DAVIS128_CONFIG_CHIP_DIGITALMUX2:
					case DAVIS128_CONFIG_CHIP_DIGITALMUX3:
					case DAVIS128_CONFIG_CHIP_ANALOGMUX0:
					case DAVIS128_CONFIG_CHIP_ANALOGMUX1:
					case DAVIS128_CONFIG_CHIP_ANALOGMUX2:
					case DAVIS128_CONFIG_CHIP_BIASMUX0:
					case DAVIS128_CONFIG_CHIP_RESETCALIBNEURON:
					case DAVIS128_CONFIG_CHIP_TYPENCALIBNEURON:
					case DAVIS128_CONFIG_CHIP_RESETTESTPIXEL:
					case DAVIS128_CONFIG_CHIP_AERNAROW:
					case DAVIS128_CONFIG_CHIP_USEAOUT:
						return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_CHIP, paramAddr, param));
						break;

					case DAVIS240_CONFIG_CHIP_SPECIALPIXELCONTROL:
						// Only supported by DAVIS240 A/B chips.
						if (IS_DAVIS240A(handle->info.chipID) || IS_DAVIS240B(handle->info.chipID)) {
							return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS128_CONFIG_CHIP_GLOBAL_SHUTTER:
						// Only supported by some chips.
						if (handle->info.apsHasGlobalShutter) {
							return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS128_CONFIG_CHIP_SELECTGRAYCOUNTER:
						// Only supported by the new DAVIS chips.
						if (IS_DAVIS128(handle->info.chipID) || IS_DAVIS208(handle->info.chipID)
							|| IS_DAVIS346(handle->info.chipID) || IS_DAVIS640(handle->info.chipID)
							|| IS_DAVIS640H(handle->info.chipID)) {
							return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS346_CONFIG_CHIP_TESTADC:
						// Only supported by some of the new DAVIS chips.
						if (IS_DAVIS346(handle->info.chipID) || IS_DAVIS640(handle->info.chipID)
							|| IS_DAVIS640H(handle->info.chipID)) {
							return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS640H_CONFIG_CHIP_ADJUSTOVG1LO:    // Also DAVIS208_CONFIG_CHIP_SELECTPREAMPAVG.
					case DAVIS640H_CONFIG_CHIP_ADJUSTOVG2LO:    // Also DAVIS208_CONFIG_CHIP_SELECTBIASREFSS.
					case DAVIS640H_CONFIG_CHIP_ADJUSTTX2OVG2HI: // Also DAVIS208_CONFIG_CHIP_SELECTSENSE.
						// Only supported by DAVIS208 and DAVISRGB.
						if (IS_DAVIS208(handle->info.chipID) || IS_DAVIS640H(handle->info.chipID)) {
							return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS208_CONFIG_CHIP_SELECTPOSFB:
					case DAVIS208_CONFIG_CHIP_SELECTHIGHPASS:
						// Only supported by DAVIS208.
						if (IS_DAVIS208(handle->info.chipID)) {
							return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					default:
						return (false);
						break;
				}
			}

			return (false);
			break;

		case DAVIS_CONFIG_SYSINFO:
			// Disallow system info parameters being read directly. Go via info struct!
			return (false);
			break;

		case DAVIS_CONFIG_USB:
			switch (paramAddr) {
				case DAVIS_CONFIG_USB_RUN:
					return (spiConfigReceive(&handle->usbState, DAVIS_CONFIG_USB, paramAddr, param));
					break;

				case DAVIS_CONFIG_USB_EARLY_PACKET_DELAY: {
					// Early packet delay is 125µs slices on host, but in cycles
					// @ USB_CLOCK_FREQ on FPGA, so we must divide here.
					uint32_t cyclesValue = 0;
					if (!spiConfigReceive(&handle->usbState, DAVIS_CONFIG_USB, paramAddr, &cyclesValue)) {
						return (false);
					}

					float delayCC = roundf((float) cyclesValue / (125.0F * state->deviceClocks.usbClockActual));
					*param        = U32T(delayCC);

					return (true);
					break;
				}

				default:
					return (false);
					break;
			}
			break;

		default:
			return (false);
			break;
	}

	return (true);
}

bool davisDataStart(caerDeviceHandle cdh, void (*dataNotifyIncrease)(void *ptr), void (*dataNotifyDecrease)(void *ptr),
	void *dataNotifyUserPtr, void (*dataShutdownNotify)(void *ptr), void *dataShutdownUserPtr) {
	davisHandle handle     = (davisHandle) cdh;
	davisCommonState state = &handle->state;

	// Store new data available/not available anymore call-backs.
	dataExchangeSetNotify(&state->dataExchange, dataNotifyIncrease, dataNotifyDecrease, dataNotifyUserPtr);

	usbSetShutdownCallback(&handle->usbState, dataShutdownNotify, dataShutdownUserPtr);

	containerGenerationCommitTimestampReset(&state->container);

	if (!dataExchangeBufferInit(&state->dataExchange)) {
		davisLog(CAER_LOG_CRITICAL, handle, "Failed to initialize data exchange buffer.");
		return (false);
	}

	// Allocate packets.
	if (!containerGenerationAllocate(&state->container, DAVIS_EVENT_TYPES)) {
		freeAllDataMemory(state);

		davisLog(CAER_LOG_CRITICAL, handle, "Failed to allocate event packet container.");
		return (false);
	}

	state->currentPackets.polarity
		= caerPolarityEventPacketAllocate(DAVIS_POLARITY_DEFAULT_SIZE, I16T(handle->info.deviceID), 0);
	if (state->currentPackets.polarity == NULL) {
		freeAllDataMemory(state);

		davisLog(CAER_LOG_CRITICAL, handle, "Failed to allocate polarity event packet.");
		return (false);
	}

	state->currentPackets.special
		= caerSpecialEventPacketAllocate(DAVIS_SPECIAL_DEFAULT_SIZE, I16T(handle->info.deviceID), 0);
	if (state->currentPackets.special == NULL) {
		freeAllDataMemory(state);

		davisLog(CAER_LOG_CRITICAL, handle, "Failed to allocate special event packet.");
		return (false);
	}

	state->currentPackets.frame = caerFrameEventPacketAllocate(DAVIS_FRAME_DEFAULT_SIZE, I16T(handle->info.deviceID), 0,
		handle->info.apsSizeX, handle->info.apsSizeY, APS_ADC_CHANNELS);
	if (state->currentPackets.frame == NULL) {
		freeAllDataMemory(state);

		davisLog(CAER_LOG_CRITICAL, handle, "Failed to allocate frame event packet.");
		return (false);
	}

	state->currentPackets.imu6 = caerIMU6EventPacketAllocate(DAVIS_IMU_DEFAULT_SIZE, I16T(handle->info.deviceID), 0);
	if (state->currentPackets.imu6 == NULL) {
		freeAllDataMemory(state);

		davisLog(CAER_LOG_CRITICAL, handle, "Failed to allocate IMU6 event packet.");
		return (false);
	}

	state->aps.frame.pixels
		= calloc((size_t)(state->aps.sizeX * state->aps.sizeY * APS_ADC_CHANNELS), sizeof(uint16_t));
	if (state->aps.frame.pixels == NULL) {
		freeAllDataMemory(state);

		davisLog(CAER_LOG_CRITICAL, handle, "Failed to allocate APS pixels memory.");
		return (false);
	}

	state->aps.frame.resetPixels
		= calloc((size_t)(state->aps.sizeX * state->aps.sizeY * APS_ADC_CHANNELS), sizeof(uint16_t));
	if (state->aps.frame.resetPixels == NULL) {
		freeAllDataMemory(state);

		davisLog(CAER_LOG_CRITICAL, handle, "Failed to allocate APS reset pixels memory (debug).");
		return (false);
	}

	state->aps.frame.signalPixels
		= calloc((size_t)(state->aps.sizeX * state->aps.sizeY * APS_ADC_CHANNELS), sizeof(uint16_t));
	if (state->aps.frame.signalPixels == NULL) {
		freeAllDataMemory(state);

		davisLog(CAER_LOG_CRITICAL, handle, "Failed to allocate APS signal pixels memory (debug).");
		return (false);
	}

	// Default IMU settings (for event parsing).
	uint32_t param32 = 0;

	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_ACCEL_FULL_SCALE, &param32);
	state->imu.accelScale = calculateIMUAccelScale(U8T(param32));
	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_GYRO_FULL_SCALE, &param32);
	state->imu.gyroScale = calculateIMUGyroScale(U8T(param32));

	// Ignore multi-part events (APS and IMU) at startup, so that any initial
	// incomplete event is ignored. The START events reset this as soon as
	// the first one is observed.
	state->aps.ignoreEvents = true;
	state->imu.ignoreEvents = true;

	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_GLOBAL_SHUTTER, &param32);
	state->aps.globalShutter = param32;

	if (!usbDataTransfersStart(&handle->usbState)) {
		freeAllDataMemory(state);

		davisLog(CAER_LOG_CRITICAL, handle, "Failed to start data transfers.");
		return (false);
	}

	if (dataExchangeStartProducers(&state->dataExchange)) {
		// Enable data transfer on USB end-point 2.
		davisConfigSet(cdh, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_RUN_CHIP, true);

		// Wait 200 ms for biases to stabilize.
		struct timespec biasEnSleep = {.tv_sec = 0, .tv_nsec = 200000000};
		thrd_sleep(&biasEnSleep, NULL);

		davisConfigSet(cdh, DAVIS_CONFIG_USB, DAVIS_CONFIG_USB_RUN, true);
		davisConfigSet(cdh, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_TIMESTAMP_RUN, true);
		davisConfigSet(cdh, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_RUN, true);

		// Wait 50 ms for data transfer to be ready.
		struct timespec noDataSleep = {.tv_sec = 0, .tv_nsec = 50000000};
		thrd_sleep(&noDataSleep, NULL);

		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_RUN, true);
		davisConfigSet(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_RUN, true);
		davisConfigSet(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_RUN_ACCELEROMETER, true);
		davisConfigSet(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_RUN_GYROSCOPE, true);
		davisConfigSet(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_RUN_TEMPERATURE, true);
		davisConfigSet(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_RUN_DETECTOR, true);
	}

	return (true);
}

bool davisDataStop(caerDeviceHandle cdh) {
	davisHandle handle     = (davisHandle) cdh;
	davisCommonState state = &handle->state;

	if (dataExchangeStopProducers(&state->dataExchange)) {
		// Disable data transfer on USB end-point 2. Reverse order of enabling.
		davisConfigSet(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_RUN, false);
		davisConfigSet(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_RUN, false);
		davisConfigSet(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_RUN_ACCELEROMETER, false);
		davisConfigSet(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_RUN_GYROSCOPE, false);
		davisConfigSet(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_RUN_TEMPERATURE, false);
		davisConfigSet(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_RUN_DETECTOR, false);

		davisConfigSet(cdh, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_RUN, false);
		davisConfigSet(cdh, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_TIMESTAMP_RUN, false);
		davisConfigSet(cdh, DAVIS_CONFIG_USB, DAVIS_CONFIG_USB_RUN, false);

		davisConfigSet(cdh, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_RUN_CHIP, false);
	}

	usbDataTransfersStop(&handle->usbState);

	dataExchangeBufferEmpty(&state->dataExchange);

	// Free current, uncommitted packets and ringbuffer.
	freeAllDataMemory(state);

	// Reset packet positions.
	state->currentPackets.polarityPosition = 0;
	state->currentPackets.specialPosition  = 0;
	state->currentPackets.framePosition    = 0;
	state->currentPackets.imu6Position     = 0;

	// Reset private composite events. 'aps.currentEvent' is taken care of in freeAllDataMemory().
	memset(&state->imu.currentEvent, 0, sizeof(struct caer_imu6_event));

	return (true);
}

caerEventPacketContainer davisDataGet(caerDeviceHandle cdh) {
	davisHandle handle     = (davisHandle) cdh;
	davisCommonState state = &handle->state;

	return (dataExchangeGet(&state->dataExchange, &handle->usbState.dataTransfersRun));
}

#define TS_WRAP_ADD 0x8000

static void davisEventTranslator(void *vhd, const uint8_t *buffer, size_t bytesSent) {
	davisHandle handle     = vhd;
	davisCommonState state = &handle->state;

	// Return right away if not running anymore. This prevents useless work if many
	// buffers are still waiting when shut down, as well as incorrect event sequences
	// if a TS_RESET is stuck on ring-buffer commit further down, and detects shut-down;
	// then any subsequent buffers should also detect shut-down and not be handled.
	if (!usbDataTransfersAreRunning(&handle->usbState)) {
		return;
	}

	// Truncate off any extra partial event.
	if ((bytesSent & 0x01) != 0) {
		davisLog(CAER_LOG_ALERT, handle, "%zu bytes received via USB, which is not a multiple of two.", bytesSent);
		bytesSent &= ~((size_t) 0x01);
	}

	for (size_t bytesIdx = 0; bytesIdx < bytesSent; bytesIdx += 2) {
		// Allocate new packets for next iteration as needed.
		if (!containerGenerationAllocate(&state->container, DAVIS_EVENT_TYPES)) {
			davisLog(CAER_LOG_CRITICAL, handle, "Failed to allocate event packet container.");
			return;
		}

		if (state->currentPackets.special == NULL) {
			state->currentPackets.special = caerSpecialEventPacketAllocate(
				DAVIS_SPECIAL_DEFAULT_SIZE, I16T(handle->info.deviceID), state->timestamps.wrapOverflow);
			if (state->currentPackets.special == NULL) {
				davisLog(CAER_LOG_CRITICAL, handle, "Failed to allocate special event packet.");
				return;
			}
		}

		if (state->currentPackets.polarity == NULL) {
			state->currentPackets.polarity = caerPolarityEventPacketAllocate(
				DAVIS_POLARITY_DEFAULT_SIZE, I16T(handle->info.deviceID), state->timestamps.wrapOverflow);
			if (state->currentPackets.polarity == NULL) {
				davisLog(CAER_LOG_CRITICAL, handle, "Failed to allocate polarity event packet.");
				return;
			}
		}

		if (state->currentPackets.frame == NULL) {
			state->currentPackets.frame
				= caerFrameEventPacketAllocate(DAVIS_FRAME_DEFAULT_SIZE, I16T(handle->info.deviceID),
					state->timestamps.wrapOverflow, handle->info.apsSizeX, handle->info.apsSizeY, APS_ADC_CHANNELS);
			if (state->currentPackets.frame == NULL) {
				davisLog(CAER_LOG_CRITICAL, handle, "Failed to allocate frame event packet.");
				return;
			}
		}

		if (state->currentPackets.imu6 == NULL) {
			state->currentPackets.imu6 = caerIMU6EventPacketAllocate(
				DAVIS_IMU_DEFAULT_SIZE, I16T(handle->info.deviceID), state->timestamps.wrapOverflow);
			if (state->currentPackets.imu6 == NULL) {
				davisLog(CAER_LOG_CRITICAL, handle, "Failed to allocate IMU6 event packet.");
				return;
			}
		}

		bool tsReset   = false;
		bool tsBigWrap = false;

		uint16_t event = le16toh(*((const uint16_t *) (&buffer[bytesIdx])));

		// Check if timestamp.
		if ((event & 0x8000) != 0) {
			handleTimestampUpdateNewLogic(&state->timestamps, event, handle->info.deviceString, &state->deviceLogLevel);

			containerGenerationCommitTimestampInit(&state->container, state->timestamps.current);
		}
		else {
			// Look at the code, to determine event and data type.
			uint8_t code  = U8T((event & 0x7000) >> 12);
			uint16_t data = (event & 0x0FFF);

			switch (code) {
				case 0: // Special event
					switch (data) {
						case 0: // Ignore this, but log it.
							davisLog(CAER_LOG_ERROR, handle, "Caught special reserved event!");
							break;

						case 1: { // Timetamp reset
							handleTimestampResetNewLogic(
								&state->timestamps, handle->info.deviceString, &state->deviceLogLevel);

							containerGenerationCommitTimestampReset(&state->container);
							containerGenerationCommitTimestampInit(&state->container, state->timestamps.current);

							// Defer timestamp reset event to later, so we commit it
							// alone, in its own packet.
							// Commit packets when doing a reset to clearly separate them.
							tsReset = true;

							// Update Master/Slave status on incoming TS resets.
							// Async call to not deadlock here.
							spiConfigReceiveAsync(&handle->usbState, DAVIS_CONFIG_SYSINFO,
								DAVIS_CONFIG_SYSINFO_DEVICE_IS_MASTER, &davisTSMasterStatusUpdater, &handle->info);
							break;
						}

						case 2: { // External input (falling edge)
							davisLog(CAER_LOG_DEBUG, handle, "External input (falling edge) event received.");

							if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
									(size_t) state->currentPackets.specialPosition, 1, handle)) {
								caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(
									state->currentPackets.special, state->currentPackets.specialPosition);
								caerSpecialEventSetTimestamp(currentSpecialEvent, state->timestamps.current);
								caerSpecialEventSetType(currentSpecialEvent, EXTERNAL_INPUT_FALLING_EDGE);
								caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
								state->currentPackets.specialPosition++;
							}

							break;
						}

						case 3: { // External input (rising edge)
							davisLog(CAER_LOG_DEBUG, handle, "External input (rising edge) event received.");

							if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
									(size_t) state->currentPackets.specialPosition, 1, handle)) {
								caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(
									state->currentPackets.special, state->currentPackets.specialPosition);
								caerSpecialEventSetTimestamp(currentSpecialEvent, state->timestamps.current);
								caerSpecialEventSetType(currentSpecialEvent, EXTERNAL_INPUT_RISING_EDGE);
								caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
								state->currentPackets.specialPosition++;
							}

							break;
						}

						case 4: { // External input (pulse)
							davisLog(CAER_LOG_DEBUG, handle, "External input (pulse) event received.");

							if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
									(size_t) state->currentPackets.specialPosition, 1, handle)) {
								caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(
									state->currentPackets.special, state->currentPackets.specialPosition);
								caerSpecialEventSetTimestamp(currentSpecialEvent, state->timestamps.current);
								caerSpecialEventSetType(currentSpecialEvent, EXTERNAL_INPUT_PULSE);
								caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
								state->currentPackets.specialPosition++;
							}

							break;
						}

						case 5: { // IMU Start (6 axes)
							davisLog(CAER_LOG_DEBUG, handle, "IMU6 Start event received.");

							state->imu.ignoreEvents = false;
							state->imu.count        = 0;
							state->imu.type         = 0;

							memset(&state->imu.currentEvent, 0, sizeof(struct caer_imu6_event));

							break;
						}

						case 7: { // IMU End
							if (state->imu.ignoreEvents) {
								break;
							}
							davisLog(CAER_LOG_DEBUG, handle, "IMU End event received.");

							if (state->imu.count == IMU_TOTAL_COUNT) {
								// Timestamp at event-stream insertion point.
								caerIMU6EventSetTimestamp(&state->imu.currentEvent, state->timestamps.current);

								caerIMU6EventValidate(&state->imu.currentEvent, state->currentPackets.imu6);

								// IMU6 and APS operate on an internal event and copy that to the actual output
								// packet here, in the END state, for a reason: if a packetContainer, with all its
								// packets, is committed due to hitting any of the triggers that are not TS reset
								// or TS wrap-around related, like number of polarity events, the event in the packet
								// would be left incomplete, and the event in the new packet would be corrupted.
								// We could avoid this like for the TS reset/TS wrap-around case (see forceCommit) by
								// just deleting that event, but these kinds of commits happen much more often and the
								// possible data loss would be too significant. So instead we keep a private event,
								// fill it, and then only copy it into the packet here in the END state, at which point
								// the whole event is ready and cannot be broken/corrupted in any way anymore.
								if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.imu6,
										(size_t) state->currentPackets.imu6Position, 1, handle)) {
									caerIMU6Event imuCurrentEvent = caerIMU6EventPacketGetEvent(
										state->currentPackets.imu6, state->currentPackets.imu6Position);
									memcpy(imuCurrentEvent, &state->imu.currentEvent, sizeof(struct caer_imu6_event));
									state->currentPackets.imu6Position++;
								}
							}
							else {
								davisLog(CAER_LOG_INFO, handle,
									"IMU End: failed to validate IMU sample count (%" PRIu8 "), discarding samples.",
									state->imu.count);
							}
							break;
						}

						case 8: { // APS Global Shutter Frame Start
							davisLog(CAER_LOG_DEBUG, handle, "APS GS Frame Start event received.");
							state->aps.globalShutter = true;

							apsInitFrame(handle);

							break;
						}

						case 9: { // APS Rolling Shutter Frame Start
							davisLog(CAER_LOG_DEBUG, handle, "APS RS Frame Start event received.");
							state->aps.globalShutter = false;

							apsInitFrame(handle);

							break;
						}

						case 10: { // APS Frame End
							if (state->aps.ignoreEvents) {
								break;
							}
							davisLog(CAER_LOG_DEBUG, handle, "APS Frame End event received.");

							// NOTE: IMU6 and APS operate on an internal event and copy that to the actual output
							// packet here, in the END state, for a reason: if a packetContainer, with all its
							// packets, is committed due to hitting any of the triggers that are not TS reset
							// or TS wrap-around related, like number of polarity events, the event in the packet
							// would be left incomplete, and the event in the new packet would be corrupted.
							// We could avoid this like for the TS reset/TS wrap-around case (see tsReset) by
							// just deleting that event, but these kinds of commits happen much more often and the
							// possible data loss would be too significant. So instead we keep a private event,
							// fill it, and then only copy it into the packet here in the END state, at which point
							// the whole event is ready and cannot be broken/corrupted in any way anymore.
							bool validFrame = apsEndFrame(handle);

							// Validate event and advance frame packet position.
							if (validFrame) {
								// Get next frame.
								if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.frame,
										(size_t) state->currentPackets.framePosition, 1, handle)) {
									caerFrameEvent frameEvent = caerFrameEventPacketGetEvent(
										state->currentPackets.frame, state->currentPackets.framePosition);
									state->currentPackets.framePosition++;

									// Setup new frame.
									caerFrameEventSetColorFilter(frameEvent, handle->info.apsColorFilter);
									caerFrameEventSetROIIdentifier(frameEvent, 0);
									caerFrameEventSetTSStartOfFrame(frameEvent, state->aps.frame.tsStartFrame);
									caerFrameEventSetTSStartOfExposure(frameEvent, state->aps.frame.tsStartExposure);
									caerFrameEventSetTSEndOfExposure(frameEvent, state->aps.frame.tsEndExposure);
									caerFrameEventSetTSEndOfFrame(frameEvent, state->timestamps.current);
									caerFrameEventSetPositionX(frameEvent, state->aps.roi.positionX);
									caerFrameEventSetPositionY(frameEvent, state->aps.roi.positionY);
									caerFrameEventSetLengthXLengthYChannelNumber(frameEvent, state->aps.roi.sizeX,
										state->aps.roi.sizeY, APS_ADC_CHANNELS, state->currentPackets.frame);
									caerFrameEventValidate(frameEvent, state->currentPackets.frame);

									// Copy pixels over.
									memcpy(caerFrameEventGetPixelArrayUnsafe(frameEvent), state->aps.frame.pixels,
										caerFrameEventGetPixelsSize(frameEvent));

									// Automatic exposure control support.
									if (atomic_load_explicit(&state->aps.autoExposure.enabled, memory_order_relaxed)) {
										float exposureFrameCC
											= roundf((float) state->aps.autoExposure.currentFrameExposure
													 / state->deviceClocks.adcClockActual);

										int32_t newExposureValue = autoExposureCalculate(&state->aps.autoExposure.state,
											frameEvent, U32T(exposureFrameCC), state->aps.autoExposure.lastSetExposure,
											atomic_load_explicit(&state->deviceLogLevel, memory_order_relaxed),
											handle->info.deviceString);

										if (newExposureValue >= 0) {
											// Update exposure value. Done in main thread to avoid deadlock inside
											// callback.
											davisLog(CAER_LOG_DEBUG, handle,
												"Automatic exposure control set exposure to %" PRIi32 " µs.",
												newExposureValue);

											state->aps.autoExposure.lastSetExposure = U32T(newExposureValue);

											float newExposureCC
												= roundf((float) newExposureValue * state->deviceClocks.adcClockActual);

											spiConfigSendAsync(&handle->usbState, DAVIS_CONFIG_APS,
												DAVIS_CONFIG_APS_EXPOSURE, U32T(newExposureCC), NULL, NULL);
										}
									}
								}

// Separate debug support.
#if APS_DEBUG_FRAME == 1
								// Get debug frames.
								if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.frame,
										(size_t) state->currentPackets.framePosition, 2, handle)) {
									// Reset frame.
									caerFrameEvent resetFrameEvent = caerFrameEventPacketGetEvent(
										state->currentPackets.frame, state->currentPackets.framePosition);
									state->currentPackets.framePosition++;

									// Setup new frame.
									caerFrameEventSetColorFilter(resetFrameEvent, handle->info.apsColorFilter);
									caerFrameEventSetROIIdentifier(resetFrameEvent, 1);
									caerFrameEventSetTSStartOfFrame(resetFrameEvent, state->aps.frame.tsStartFrame);
									caerFrameEventSetTSStartOfExposure(
										resetFrameEvent, state->aps.frame.tsStartExposure);
									caerFrameEventSetTSEndOfExposure(resetFrameEvent, state->aps.frame.tsEndExposure);
									caerFrameEventSetTSEndOfFrame(resetFrameEvent, state->timestamps.current);
									caerFrameEventSetPositionX(resetFrameEvent, state->aps.roi.positionX);
									caerFrameEventSetPositionY(resetFrameEvent, state->aps.roi.positionY);
									caerFrameEventSetLengthXLengthYChannelNumber(resetFrameEvent, state->aps.roi.sizeX,
										state->aps.roi.sizeY, APS_ADC_CHANNELS, state->currentPackets.frame);
									caerFrameEventValidate(resetFrameEvent, state->currentPackets.frame);

									// Copy pixels over.
									memcpy(caerFrameEventGetPixelArrayUnsafe(resetFrameEvent),
										state->aps.frame.resetPixels, caerFrameEventGetPixelsSize(resetFrameEvent));

									// Signal frame.
									caerFrameEvent signalFrameEvent = caerFrameEventPacketGetEvent(
										state->currentPackets.frame, state->currentPackets.framePosition);
									state->currentPackets.framePosition++;

									// Setup new frame.
									caerFrameEventSetColorFilter(signalFrameEvent, handle->info.apsColorFilter);
									caerFrameEventSetROIIdentifier(signalFrameEvent, 2);
									caerFrameEventSetTSStartOfFrame(signalFrameEvent, state->aps.frame.tsStartFrame);
									caerFrameEventSetTSStartOfExposure(
										signalFrameEvent, state->aps.frame.tsStartExposure);
									caerFrameEventSetTSEndOfExposure(signalFrameEvent, state->aps.frame.tsEndExposure);
									caerFrameEventSetTSEndOfFrame(signalFrameEvent, state->timestamps.current);
									caerFrameEventSetPositionX(signalFrameEvent, state->aps.roi.positionX);
									caerFrameEventSetPositionY(signalFrameEvent, state->aps.roi.positionY);
									caerFrameEventSetLengthXLengthYChannelNumber(signalFrameEvent, state->aps.roi.sizeX,
										state->aps.roi.sizeY, APS_ADC_CHANNELS, state->currentPackets.frame);
									caerFrameEventValidate(signalFrameEvent, state->currentPackets.frame);

									// Copy pixels over.
									memcpy(caerFrameEventGetPixelArrayUnsafe(signalFrameEvent),
										state->aps.frame.signalPixels, caerFrameEventGetPixelsSize(signalFrameEvent));
								}
#endif
							}

							break;
						}

						case 11: { // APS Reset Column Start
							if (state->aps.ignoreEvents) {
								break;
							}
							davisLog(CAER_LOG_DEBUG, handle, "APS Reset Column Start event received.");

							state->aps.currentReadoutType        = APS_READOUT_RESET;
							state->aps.countY[APS_READOUT_RESET] = 0;

							state->aps.cDavisSupport.offsetDirection = 0;
							state->aps.cDavisSupport.offset          = 1; // First pixel of row always even.

							break;
						}

						case 12: { // APS Signal Column Start
							if (state->aps.ignoreEvents) {
								break;
							}
							davisLog(CAER_LOG_DEBUG, handle, "APS Signal Column Start event received.");

							state->aps.currentReadoutType         = APS_READOUT_SIGNAL;
							state->aps.countY[APS_READOUT_SIGNAL] = 0;

							state->aps.cDavisSupport.offsetDirection = 0;
							state->aps.cDavisSupport.offset          = 1; // First pixel of row always even.

							break;
						}

						case 13: { // APS Column End
							if (state->aps.ignoreEvents) {
								break;
							}
							davisLog(CAER_LOG_DEBUG, handle, "APS Column End event received.");

							davisLog(CAER_LOG_DEBUG, handle, "APS Column End: CountX[%d] is %d.",
								state->aps.currentReadoutType, state->aps.countX[state->aps.currentReadoutType]);
							davisLog(CAER_LOG_DEBUG, handle, "APS Column End: CountY[%d] is %d.",
								state->aps.currentReadoutType, state->aps.countY[state->aps.currentReadoutType]);

							if (state->aps.countY[state->aps.currentReadoutType] != state->aps.expectedCountY) {
								davisLog(CAER_LOG_ERROR, handle,
									"APS Column End - %d - %d: wrong row count %d detected, expected %d.",
									state->aps.currentReadoutType, state->aps.countX[state->aps.currentReadoutType],
									state->aps.countY[state->aps.currentReadoutType], state->aps.expectedCountY);
							}

							state->aps.countX[state->aps.currentReadoutType]++;

							break;
						}

						case 14: { // APS Exposure Start
							if (state->aps.ignoreEvents) {
								break;
							}
							davisLog(CAER_LOG_DEBUG, handle, "APS Exposure Start event received.");

							state->aps.frame.tsStartExposure = state->timestamps.current;

							// Send APS info event out (as special event).
							if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
									(size_t) state->currentPackets.specialPosition, 1, handle)) {
								caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(
									state->currentPackets.special, state->currentPackets.specialPosition);
								caerSpecialEventSetTimestamp(currentSpecialEvent, state->timestamps.current);
								caerSpecialEventSetType(currentSpecialEvent, APS_EXPOSURE_START);
								caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
								state->currentPackets.specialPosition++;
							}

							break;
						}

						case 15: { // APS Exposure End
							if (state->aps.ignoreEvents) {
								break;
							}
							davisLog(CAER_LOG_DEBUG, handle, "APS Exposure End event received.");

							state->aps.frame.tsEndExposure = state->timestamps.current;

							// Send APS info event out (as special event).
							if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
									(size_t) state->currentPackets.specialPosition, 1, handle)) {
								caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(
									state->currentPackets.special, state->currentPackets.specialPosition);
								caerSpecialEventSetTimestamp(currentSpecialEvent, state->timestamps.current);
								caerSpecialEventSetType(currentSpecialEvent, APS_EXPOSURE_END);
								caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
								state->currentPackets.specialPosition++;
							}

							break;
						}

						case 16: { // External generator (falling edge)
							davisLog(CAER_LOG_DEBUG, handle, "External generator (falling edge) event received.");

							if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
									(size_t) state->currentPackets.specialPosition, 1, handle)) {
								caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(
									state->currentPackets.special, state->currentPackets.specialPosition);
								caerSpecialEventSetTimestamp(currentSpecialEvent, state->timestamps.current);
								caerSpecialEventSetType(currentSpecialEvent, EXTERNAL_GENERATOR_FALLING_EDGE);
								caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
								state->currentPackets.specialPosition++;
							}

							break;
						}

						case 17: { // External generator (rising edge)
							davisLog(CAER_LOG_DEBUG, handle, "External generator (rising edge) event received.");

							if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
									(size_t) state->currentPackets.specialPosition, 1, handle)) {
								caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(
									state->currentPackets.special, state->currentPackets.specialPosition);
								caerSpecialEventSetTimestamp(currentSpecialEvent, state->timestamps.current);
								caerSpecialEventSetType(currentSpecialEvent, EXTERNAL_GENERATOR_RISING_EDGE);
								caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
								state->currentPackets.specialPosition++;
							}

							break;
						}

						default:
							davisLog(CAER_LOG_ERROR, handle, "Caught special event that can't be handled: %d.", data);
							break;
					}
					break;

				case 1: // Y address
					// Check range conformity.
					if (data >= state->dvs.sizeY) {
						davisLog(CAER_LOG_ALERT, handle, "DVS: Y address out of range (0-%d): %" PRIu16 ".",
							state->dvs.sizeY - 1, data);
						break; // Skip invalid Y address (don't update lastY).
					}

					state->dvs.lastY = data;

					break;

				case 2:   // X address, Polarity OFF
				case 3: { // X address, Polarity ON
					// Check range conformity.
					if (data >= state->dvs.sizeX) {
						davisLog(CAER_LOG_ALERT, handle, "DVS: X address out of range (0-%d): %" PRIu16 ".",
							state->dvs.sizeX - 1, data);
						break; // Skip invalid event.
					}

					// Invert polarity for PixelParade high gain pixels (DavisSense), because of negative gain from
					// pre-amplifier. uint8_t polarity = ((IS_DAVIS208(handle->info.chipID)) && (data < 192)) ?
					// U8T(~code) : (code);

					if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.polarity,
							(size_t) state->currentPackets.polarityPosition, 1, handle)) {
						caerPolarityEvent currentPolarityEvent = caerPolarityEventPacketGetEvent(
							state->currentPackets.polarity, state->currentPackets.polarityPosition);

						// Timestamp at event-stream insertion point.
						caerPolarityEventSetTimestamp(currentPolarityEvent, state->timestamps.current);
						caerPolarityEventSetPolarity(currentPolarityEvent, (code & 0x01));
						if (state->dvs.invertXY) {
							caerPolarityEventSetY(currentPolarityEvent, data);
							caerPolarityEventSetX(currentPolarityEvent, state->dvs.lastY);
						}
						else {
							caerPolarityEventSetY(currentPolarityEvent, state->dvs.lastY);
							caerPolarityEventSetX(currentPolarityEvent, data);
						}
						caerPolarityEventValidate(currentPolarityEvent, state->currentPackets.polarity);
						state->currentPackets.polarityPosition++;
					}

					break;
				}

				case 4: {
					if (state->aps.ignoreEvents) {
						break;
					}

					// Ignore too big X/Y counts, can happen if column start/end events are lost.
					if ((state->aps.countX[state->aps.currentReadoutType] >= state->aps.expectedCountX)
						|| (state->aps.countY[state->aps.currentReadoutType] >= state->aps.expectedCountY)) {
						break;
					}

					// DAVIS240 has a reduced dynamic range due to external
					// ADC high/low ref resistors not having optimal values.
					// To fix this multiply by 1.95 to 2.15, so we choose to
					// just shift by one (multiply by 2.00) for efficiency.
					if (IS_DAVIS240(handle->info.chipID)) {
						data = U16T(data << 1);
					}

					apsUpdateFrame(handle, data);

					state->aps.countY[state->aps.currentReadoutType]++;

					break;
				}

				case 5: {
					// Misc 8bit data.
					uint8_t misc8Code = U8T((data & 0x0F00) >> 8);
					uint8_t misc8Data = U8T(data & 0x00FF);

					switch (misc8Code) {
						case 0:
							if (state->imu.ignoreEvents) {
								break;
							}

							// IMU data event.
							switch (state->imu.count) {
								case 0:
								case 2:
								case 4:
								case 6:
								case 8:
								case 10:
								case 12:
									state->imu.tmpData = misc8Data;
									break;

								case 1: {
									int16_t accelX = I16T((state->imu.tmpData << 8) | misc8Data);
									if (state->imu.flipX) {
										accelX = I16T(-accelX);
									}
									caerIMU6EventSetAccelX(&state->imu.currentEvent, accelX / state->imu.accelScale);
									break;
								}

								case 3: {
									int16_t accelY = I16T((state->imu.tmpData << 8) | misc8Data);
									if (state->imu.flipY) {
										accelY = I16T(-accelY);
									}
									caerIMU6EventSetAccelY(&state->imu.currentEvent, accelY / state->imu.accelScale);
									break;
								}

								case 5: {
									int16_t accelZ = I16T((state->imu.tmpData << 8) | misc8Data);
									if (state->imu.flipZ) {
										accelZ = I16T(-accelZ);
									}
									caerIMU6EventSetAccelZ(&state->imu.currentEvent, accelZ / state->imu.accelScale);

									// IMU parser count depends on which data is present.
									if (!(state->imu.type & IMU_TYPE_TEMP)) {
										if (state->imu.type & IMU_TYPE_GYRO) {
											// No temperature, but gyro.
											state->imu.count += 2;
										}
										else {
											// No others enabled.
											state->imu.count += 8;
										}
									}
									break;
								}

								case 7: {
									// Temperature is signed. Formula for converting to °C:
									// (SIGNED_VAL / 340) + 36.53
									int16_t temp = I16T((state->imu.tmpData << 8) | misc8Data);
									caerIMU6EventSetTemp(&state->imu.currentEvent, (temp / 340.0F) + 36.53F);

									// IMU parser count depends on which data is present.
									if (!(state->imu.type & IMU_TYPE_GYRO)) {
										// No others enabled.
										state->imu.count += 6;
									}
									break;
								}

								case 9: {
									int16_t gyroX = I16T((state->imu.tmpData << 8) | misc8Data);
									if (state->imu.flipX) {
										gyroX = I16T(-gyroX);
									}
									caerIMU6EventSetGyroX(&state->imu.currentEvent, gyroX / state->imu.gyroScale);
									break;
								}

								case 11: {
									int16_t gyroY = I16T((state->imu.tmpData << 8) | misc8Data);
									if (state->imu.flipY) {
										gyroY = I16T(-gyroY);
									}
									caerIMU6EventSetGyroY(&state->imu.currentEvent, gyroY / state->imu.gyroScale);
									break;
								}

								case 13: {
									int16_t gyroZ = I16T((state->imu.tmpData << 8) | misc8Data);
									if (state->imu.flipZ) {
										gyroZ = I16T(-gyroZ);
									}
									caerIMU6EventSetGyroZ(&state->imu.currentEvent, gyroZ / state->imu.gyroScale);
									break;
								}

								default:
									davisLog(CAER_LOG_ERROR, handle, "Got invalid IMU update sequence.");
									break;
							}

							state->imu.count++;

							break;

						case 1:
							// APS ROI Size Part 1 (bits 15-8).
							// Here we just store the temporary value, and use it again
							// in the next case statement.
							state->aps.roi.tmpData = U16T(misc8Data << 8);

							break;

						case 2: {
							// APS ROI Size Part 2 (bits 7-0).
							// Here we just store the values and re-use the four fields
							// sizeX/Y and positionX/Y to store endCol/Row and startCol/Row.
							// We then recalculate all the right values and set everything
							// up in START_FRAME.
							switch (state->aps.roi.update & 0x03) {
								case 0:
									// START COLUMN
									state->aps.roi.positionX = U16T(state->aps.roi.tmpData | misc8Data);
									break;

								case 1:
									// START ROW
									state->aps.roi.positionY = U16T(state->aps.roi.tmpData | misc8Data);
									break;

								case 2:
									// END COLUMN
									state->aps.roi.sizeX = U16T(state->aps.roi.tmpData | misc8Data);
									break;

								case 3:
									// END ROW
									state->aps.roi.sizeY = U16T(state->aps.roi.tmpData | misc8Data);

									// Got all sizes, now compute correct window.
									apsROIUpdateSizes(state);
									break;

								default:
									davisLog(CAER_LOG_ERROR, handle, "Got invalid ROI update sequence.");
									break;
							}

							// Jump to next type of APS info (col->row, start->end).
							state->aps.roi.update++;

							break;
						}

						case 3: {
							if (state->imu.ignoreEvents) {
								break;
							}
							davisLog(CAER_LOG_DEBUG, handle, "IMU Scale Config event (%" PRIu16 ") received.", data);

							// Set correct IMU accel and gyro scales, used to interpret subsequent
							// IMU samples from the device.
							state->imu.accelScale = calculateIMUAccelScale(U16T(data >> 2) & 0x03);
							state->imu.gyroScale  = calculateIMUGyroScale(data & 0x03);

							// Set expected type of data to come from IMU (accel, gyro, temp).
							state->imu.type = (data >> 5) & 0x07;

							// IMU parser start count depends on which data is present.
							if (state->imu.type & IMU_TYPE_ACCEL) {
								// Accelerometer.
								state->imu.count = 0;
							}
							else if (state->imu.type & IMU_TYPE_TEMP) {
								// Temperature
								state->imu.count = 6;
							}
							else if (state->imu.type & IMU_TYPE_GYRO) {
								// Gyroscope.
								state->imu.count = 8;
							}
							else {
								// Nothing, should never happen.
								state->imu.count = 14;

								davisLog(CAER_LOG_ERROR, handle, "IMU Scale Config: no IMU sensors enabled.");
							}

							break;
						}

						default:
							davisLog(CAER_LOG_ERROR, handle, "Caught Misc8 event that can't be handled.");
							break;
					}

					break;
				}

				case 6: {
					// Misc 10bit data.
					uint8_t misc10Code  = U8T((data & 0x0C00) >> 10);
					uint16_t misc10Data = U16T(data & 0x03FF);

					switch (misc10Code) {
						case 0:
							state->aps.autoExposure.currentFrameExposure
								|= (U32T(misc10Data) << U32T(10 * state->aps.autoExposure.tmpData));
							state->aps.autoExposure.tmpData++;
							break;

						default:
							davisLog(CAER_LOG_ERROR, handle, "Caught Misc10 event that can't be handled.");
							break;
					}

					break;
				}

				case 7: { // Timestamp wrap
					tsBigWrap = handleTimestampWrapNewLogic(
						&state->timestamps, data, TS_WRAP_ADD, handle->info.deviceString, &state->deviceLogLevel);

					if (tsBigWrap) {
						if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
								(size_t) state->currentPackets.specialPosition, 1, handle)) {
							caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(
								state->currentPackets.special, state->currentPackets.specialPosition);
							caerSpecialEventSetTimestamp(currentSpecialEvent, INT32_MAX);
							caerSpecialEventSetType(currentSpecialEvent, TIMESTAMP_WRAP);
							caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
							state->currentPackets.specialPosition++;
						}
					}
					else {
						containerGenerationCommitTimestampInit(&state->container, state->timestamps.current);
					}

					break;
				}

				default:
					davisLog(CAER_LOG_ERROR, handle, "Caught event that can't be handled.");
					break;
			}
		}

		// Thresholds on which to trigger packet container commit.
		// tsReset and tsBigWrap are already defined above.
		// Trigger if any of the global container-wide thresholds are met.
		int32_t currentPacketContainerCommitSize = containerGenerationGetMaxPacketSize(&state->container);
		bool containerSizeCommit                 = (currentPacketContainerCommitSize > 0)
								   && ((state->currentPackets.polarityPosition >= currentPacketContainerCommitSize)
										  || (state->currentPackets.specialPosition >= currentPacketContainerCommitSize)
										  || (state->currentPackets.framePosition >= currentPacketContainerCommitSize)
										  || (state->currentPackets.imu6Position >= currentPacketContainerCommitSize));

		bool containerTimeCommit = containerGenerationIsCommitTimestampElapsed(
			&state->container, state->timestamps.wrapOverflow, state->timestamps.current);

		// Commit packet containers to the ring-buffer, so they can be processed by the
		// main-loop, when any of the required conditions are met.
		if (tsReset || tsBigWrap || containerSizeCommit || containerTimeCommit) {
			// One or more of the commit triggers are hit. Set the packet container up to contain
			// any non-empty packets. Empty packets are not forwarded to save memory.
			bool emptyContainerCommit = true;

			if (state->currentPackets.polarityPosition > 0) {
				containerGenerationSetPacket(
					&state->container, POLARITY_EVENT, (caerEventPacketHeader) state->currentPackets.polarity);

				// Run pixel filter auto-train. Can only be enabled if hw-filter present.
				if (atomic_load_explicit(&state->dvs.pixelFilterAutoTrain.autoTrainRunning, memory_order_relaxed)) {
					if (state->dvs.pixelFilterAutoTrain.noiseFilter == NULL) {
						state->dvs.pixelFilterAutoTrain.noiseFilter
							= caerFilterDVSNoiseInitialize(U16T(handle->info.dvsSizeX), U16T(handle->info.dvsSizeY));
						if (state->dvs.pixelFilterAutoTrain.noiseFilter == NULL) {
							// Failed to initialize, auto-training not possible.
							atomic_store(&state->dvs.pixelFilterAutoTrain.autoTrainRunning, false);
							goto out;
						}

						// Allocate+init success, configure it for hot-pixel learning.
						caerFilterDVSNoiseConfigSet(
							state->dvs.pixelFilterAutoTrain.noiseFilter, CAER_FILTER_DVS_HOTPIXEL_COUNT, 1000);
						caerFilterDVSNoiseConfigSet(
							state->dvs.pixelFilterAutoTrain.noiseFilter, CAER_FILTER_DVS_HOTPIXEL_TIME, 1000000);
						caerFilterDVSNoiseConfigSet(
							state->dvs.pixelFilterAutoTrain.noiseFilter, CAER_FILTER_DVS_HOTPIXEL_LEARN, true);
					}

					// NoiseFilter must be allocated and initialized if we get here.
					caerFilterDVSNoiseApply(
						state->dvs.pixelFilterAutoTrain.noiseFilter, state->currentPackets.polarity);

					uint64_t stillLearning = 1;
					caerFilterDVSNoiseConfigGet(
						state->dvs.pixelFilterAutoTrain.noiseFilter, CAER_FILTER_DVS_HOTPIXEL_LEARN, &stillLearning);

					if (!stillLearning) {
						// Learning done, we can grab the list of hot pixels, and hardware-filter them.
						caerFilterDVSPixel hotPixels;
						ssize_t hotPixelsSize
							= caerFilterDVSNoiseGetHotPixels(state->dvs.pixelFilterAutoTrain.noiseFilter, &hotPixels);
						if (hotPixelsSize < 0) {
							// Failed to get list.
							atomic_store(&state->dvs.pixelFilterAutoTrain.autoTrainRunning, false);
							goto out;
						}

						// Limit to maximum hardware size.
						if (hotPixelsSize > DVS_HOTPIXEL_HW_MAX) {
							hotPixelsSize = DVS_HOTPIXEL_HW_MAX;
						}

						// Go through the found pixels and filter them. Disable not used slots.
						size_t i = 0;

						for (; i < (size_t) hotPixelsSize; i++) {
							spiConfigSendAsync(&handle->usbState, DAVIS_CONFIG_DVS,
								U8T(DAVIS_CONFIG_DVS_FILTER_PIXEL_0_COLUMN + 2 * i),
								(state->dvs.invertXY) ? (hotPixels[i].y) : (hotPixels[i].x), NULL, NULL);
							spiConfigSendAsync(&handle->usbState, DAVIS_CONFIG_DVS,
								U8T(DAVIS_CONFIG_DVS_FILTER_PIXEL_0_ROW + 2 * i),
								(state->dvs.invertXY) ? (hotPixels[i].x) : (hotPixels[i].y), NULL, NULL);
						}

						for (; i < DVS_HOTPIXEL_HW_MAX; i++) {
							spiConfigSendAsync(&handle->usbState, DAVIS_CONFIG_DVS,
								U8T(DAVIS_CONFIG_DVS_FILTER_PIXEL_0_COLUMN + 2 * i), U32T(state->dvs.sizeX), NULL,
								NULL);
							spiConfigSendAsync(&handle->usbState, DAVIS_CONFIG_DVS,
								U8T(DAVIS_CONFIG_DVS_FILTER_PIXEL_0_ROW + 2 * i), U32T(state->dvs.sizeY), NULL, NULL);
						}

						// We're done!
						free(hotPixels);

						atomic_store(&state->dvs.pixelFilterAutoTrain.autoTrainRunning, false);
						goto out;
					}
				}
				else {
				out:
					// Deallocate when turned off, either by user or by having completed.
					if (state->dvs.pixelFilterAutoTrain.noiseFilter != NULL) {
						caerFilterDVSNoiseDestroy(state->dvs.pixelFilterAutoTrain.noiseFilter);
						state->dvs.pixelFilterAutoTrain.noiseFilter = NULL;
					}
				}

				state->currentPackets.polarity         = NULL;
				state->currentPackets.polarityPosition = 0;
				emptyContainerCommit                   = false;
			}

			if (state->currentPackets.specialPosition > 0) {
				containerGenerationSetPacket(
					&state->container, SPECIAL_EVENT, (caerEventPacketHeader) state->currentPackets.special);

				state->currentPackets.special         = NULL;
				state->currentPackets.specialPosition = 0;
				emptyContainerCommit                  = false;
			}

			if (state->currentPackets.framePosition > 0) {
				containerGenerationSetPacket(
					&state->container, FRAME_EVENT, (caerEventPacketHeader) state->currentPackets.frame);

				state->currentPackets.frame         = NULL;
				state->currentPackets.framePosition = 0;
				emptyContainerCommit                = false;
			}

			if (state->currentPackets.imu6Position > 0) {
				containerGenerationSetPacket(
					&state->container, IMU6_EVENT, (caerEventPacketHeader) state->currentPackets.imu6);

				state->currentPackets.imu6         = NULL;
				state->currentPackets.imu6Position = 0;
				emptyContainerCommit               = false;
			}

			if (tsReset || tsBigWrap) {
				// Ignore all APS and IMU6 (composite) events, until a new APS or IMU6
				// Start event comes in, for the next packet.
				// This is to correctly support the forced packet commits that a TS reset,
				// or a TS big wrap, impose. Continuing to parse events would result
				// in a corrupted state of the first event in the new packet, as it would
				// be incomplete, incorrect and miss vital initialization data.
				// See APS and IMU6 END states for more details on a related issue.
				state->aps.ignoreEvents = true;
				state->imu.ignoreEvents = true;
			}

			containerGenerationExecute(&state->container, emptyContainerCommit, tsReset, state->timestamps.wrapOverflow,
				state->timestamps.current, &state->dataExchange, &handle->usbState.dataTransfersRun,
				handle->info.deviceID, handle->info.deviceString, &state->deviceLogLevel);
		}
	}
}

static void davisTSMasterStatusUpdater(void *userDataPtr, int status, uint32_t param) {
	// If any USB error happened, discard.
	if (status != LIBUSB_TRANSFER_COMPLETED) {
		return;
	}

	// Get new Master/Slave information from device. Done here to prevent deadlock
	// inside asynchronous callback.
	struct caer_davis_info *info = userDataPtr;

	atomic_thread_fence(memory_order_seq_cst);
	info->deviceIsMaster = param;
	atomic_thread_fence(memory_order_seq_cst);
}

uint16_t caerBiasVDACGenerate(const struct caer_bias_vdac vdacBias) {
	// Build up bias value from all its components.
	uint16_t biasValue = U16T((vdacBias.voltageValue & 0x3F) << 0);
	biasValue          = U16T(biasValue | ((vdacBias.currentValue & 0x07) << 6));

	return (biasValue);
}

struct caer_bias_vdac caerBiasVDACParse(const uint16_t vdacBias) {
	struct caer_bias_vdac biasValue;

	// Decompose bias integer into its parts.
	biasValue.voltageValue = vdacBias & 0x3F;
	biasValue.currentValue = U16T(vdacBias >> 6) & 0x07;

	return (biasValue);
}

uint16_t caerBiasCoarseFineGenerate(const struct caer_bias_coarsefine coarseFineBias) {
	uint16_t biasValue = 0;

	// Build up bias value from all its components.
	if (coarseFineBias.enabled) {
		biasValue |= 0x01U;
	}
	if (coarseFineBias.sexN) {
		biasValue |= 0x02U;
	}
	if (coarseFineBias.typeNormal) {
		biasValue |= 0x04U;
	}
	if (coarseFineBias.currentLevelNormal) {
		biasValue |= 0x08U;
	}

	biasValue = U16T(biasValue | ((coarseFineBias.fineValue & 0xFF) << 4));
	biasValue = U16T(biasValue | ((coarseFineBias.coarseValue & 0x07) << 12));

	return (biasValue);
}

struct caer_bias_coarsefine caerBiasCoarseFineParse(const uint16_t coarseFineBias) {
	struct caer_bias_coarsefine biasValue;

	// Decompose bias integer into its parts.
	biasValue.enabled            = (coarseFineBias & 0x01);
	biasValue.sexN               = (coarseFineBias & 0x02);
	biasValue.typeNormal         = (coarseFineBias & 0x04);
	biasValue.currentLevelNormal = (coarseFineBias & 0x08);
	biasValue.fineValue          = U8T(coarseFineBias >> 4) & 0xFF;
	biasValue.coarseValue        = U8T(coarseFineBias >> 12) & 0x07;

	return (biasValue);
}

static const uint32_t coarseCurrents[8] = {
	11,
	94,
	756,
	6054,
	48437,
	387500,
	3100000,
	24800000,
};

struct caer_bias_coarsefine caerBiasCoarseFineFromCurrent(uint32_t picoAmps) {
	struct caer_bias_coarsefine biasValue;

	// Disable bias.
	if (picoAmps == 0) {
		biasValue.coarseValue = 0;
		biasValue.fineValue   = 0;

		return (biasValue);
	}

	// We support between 1 pA and 24.8 uA (24.8 mio pA).
	if (picoAmps > 24800000) {
		picoAmps = 24800000; // Limit to 24.8 uA.
	}

	// Select appropriate coarse value from precomputed table.
	uint8_t coarseValue = 0;

	for (uint8_t i = 0; i < 8; i++) {
		if (picoAmps <= coarseCurrents[i]) {
			coarseValue = i;
			break;
		}
	}

	biasValue.coarseValue = U8T(coarseValue);

	// Calculate coarse current value based on value going to device.
	// This is the maximum for the fine divider.
	double coarseCurrent = (double) coarseCurrents[coarseValue];

	int32_t fineValue = I32T(round(((255.0 * (double) picoAmps) / coarseCurrent)));

	// Ensure within range.
	if (fineValue < 1) {
		fineValue = 1;
	}
	else if (fineValue > 255) {
		fineValue = 255;
	}

	biasValue.fineValue = U8T(fineValue);

	return (biasValue);
}

uint32_t caerBiasCoarseFineToCurrent(struct caer_bias_coarsefine coarseFineBias) {
	// Special base: disabled bias.
	if (coarseFineBias.fineValue == 0) {
		return (0);
	}

	double coarseCurrent = (double) coarseCurrents[coarseFineBias.coarseValue];
	double fineCurrent   = (coarseCurrent * (double) coarseFineBias.fineValue) / 255.0;

	double biasCurrent = round(fineCurrent);

	return (U32T(biasCurrent));
}

uint16_t caerBiasShiftedSourceGenerate(const struct caer_bias_shiftedsource shiftedSourceBias) {
	uint16_t biasValue = 0;

	if (shiftedSourceBias.operatingMode == HI_Z) {
		biasValue |= 0x01U;
	}
	else if (shiftedSourceBias.operatingMode == TIED_TO_RAIL) {
		biasValue |= 0x02U;
	}

	if (shiftedSourceBias.voltageLevel == SINGLE_DIODE) {
		biasValue |= (0x01U << 2);
	}
	else if (shiftedSourceBias.voltageLevel == DOUBLE_DIODE) {
		biasValue |= (0x02U << 2);
	}

	biasValue = U16T(biasValue | ((shiftedSourceBias.refValue & 0x3F) << 4));
	biasValue = U16T(biasValue | ((shiftedSourceBias.regValue & 0x3F) << 10));

	return (biasValue);
}

struct caer_bias_shiftedsource caerBiasShiftedSourceParse(const uint16_t shiftedSourceBias) {
	struct caer_bias_shiftedsource biasValue;

	// Decompose bias integer into its parts.
	if (shiftedSourceBias & 0x01) {
		biasValue.operatingMode = HI_Z;
	}
	else if (shiftedSourceBias & 0x02) {
		biasValue.operatingMode = TIED_TO_RAIL;
	}
	else {
		biasValue.operatingMode = SHIFTED_SOURCE;
	}

	if (U16T(shiftedSourceBias >> 2) & 0x01) {
		biasValue.voltageLevel = SINGLE_DIODE;
	}
	else if (U16T(shiftedSourceBias >> 2) & 0x02) {
		biasValue.voltageLevel = DOUBLE_DIODE;
	}
	else {
		biasValue.voltageLevel = SPLIT_GATE;
	}

	biasValue.refValue = U16T(shiftedSourceBias >> 4) & 0x3F;
	biasValue.regValue = U16T(shiftedSourceBias >> 10) & 0x3F;

	return (biasValue);
}

//////////////////////////////////
/// FX3 Debug Transfer Support ///
//////////////////////////////////
static void allocateDebugTransfers(davisHandle handle) {
	// Allocate transfers and set them up.
	for (size_t i = 0; i < DEBUG_TRANSFER_NUM; i++) {
		handle->fx3Support.debugTransfers[i] = libusb_alloc_transfer(0);
		if (handle->fx3Support.debugTransfers[i] == NULL) {
			davisLog(CAER_LOG_CRITICAL, handle,
				"Unable to allocate further libusb transfers (debug channel, %zu of %" PRIu32 ").", i,
				DEBUG_TRANSFER_NUM);
			continue;
		}

		// Create data buffer.
		handle->fx3Support.debugTransfers[i]->length = DEBUG_TRANSFER_SIZE;
		handle->fx3Support.debugTransfers[i]->buffer = malloc(DEBUG_TRANSFER_SIZE);
		if (handle->fx3Support.debugTransfers[i]->buffer == NULL) {
			davisLog(CAER_LOG_CRITICAL, handle,
				"Unable to allocate buffer for libusb transfer %zu (debug channel). Error: %d.", i, errno);

			libusb_free_transfer(handle->fx3Support.debugTransfers[i]);
			handle->fx3Support.debugTransfers[i] = NULL;

			continue;
		}

		// Initialize Transfer.
		handle->fx3Support.debugTransfers[i]->dev_handle = handle->usbState.deviceHandle;
		handle->fx3Support.debugTransfers[i]->endpoint   = DEBUG_ENDPOINT;
		handle->fx3Support.debugTransfers[i]->type       = LIBUSB_TRANSFER_TYPE_INTERRUPT;
		handle->fx3Support.debugTransfers[i]->callback   = &libUsbDebugCallback;
		handle->fx3Support.debugTransfers[i]->user_data  = handle;
		handle->fx3Support.debugTransfers[i]->timeout    = 0;
		handle->fx3Support.debugTransfers[i]->flags      = LIBUSB_TRANSFER_FREE_BUFFER;

		if ((errno = libusb_submit_transfer(handle->fx3Support.debugTransfers[i])) == LIBUSB_SUCCESS) {
			atomic_fetch_add(&handle->fx3Support.activeDebugTransfers, 1);
		}
		else {
			davisLog(CAER_LOG_CRITICAL, handle, "Unable to submit libusb transfer %zu (debug channel). Error: %s (%d).",
				i, libusb_strerror(errno), errno);

			// The transfer buffer is freed automatically here thanks to
			// the LIBUSB_TRANSFER_FREE_BUFFER flag set above.
			libusb_free_transfer(handle->fx3Support.debugTransfers[i]);
			handle->fx3Support.debugTransfers[i] = NULL;
		}
	}

	if (atomic_load(&handle->fx3Support.activeDebugTransfers) == 0) {
		// Didn't manage to allocate any USB transfers, log failure.
		davisLog(CAER_LOG_CRITICAL, handle, "Unable to allocate any libusb transfers (debug channel).");
	}
}

static void cancelAndDeallocateDebugTransfers(davisHandle handle) {
	// Wait for all transfers to go away.
	struct timespec waitForTerminationSleep = {.tv_sec = 0, .tv_nsec = 1000000};

	while (atomic_load(&handle->fx3Support.activeDebugTransfers) > 0) {
		// Continue trying to cancel all transfers until there are none left.
		// It seems like one cancel pass is not enough and some hang around.
		for (size_t i = 0; i < DEBUG_TRANSFER_NUM; i++) {
			if (handle->fx3Support.debugTransfers[i] != NULL) {
				errno = libusb_cancel_transfer(handle->fx3Support.debugTransfers[i]);
				if ((errno != LIBUSB_SUCCESS) && (errno != LIBUSB_ERROR_NOT_FOUND)) {
					davisLog(CAER_LOG_CRITICAL, handle,
						"Unable to cancel libusb transfer %zu (debug channel). Error: %s (%d).", i,
						libusb_strerror(errno), errno);
					// Proceed with trying to cancel all transfers regardless of errors.
				}
			}
		}

		// Sleep for 1ms to avoid busy loop.
		thrd_sleep(&waitForTerminationSleep, NULL);
	}

	// No more transfers in flight, deallocate them all here.
	for (size_t i = 0; i < DEBUG_TRANSFER_NUM; i++) {
		if (handle->fx3Support.debugTransfers[i] != NULL) {
			libusb_free_transfer(handle->fx3Support.debugTransfers[i]);
			handle->fx3Support.debugTransfers[i] = NULL;
		}
	}
}

static void LIBUSB_CALL libUsbDebugCallback(struct libusb_transfer *transfer) {
	davisHandle handle = transfer->user_data;

	// Completed or cancelled transfers are what we expect to handle here, so
	// if they do have data attached, try to parse them.
	if (((transfer->status == LIBUSB_TRANSFER_COMPLETED) || (transfer->status == LIBUSB_TRANSFER_CANCELLED))
		&& (transfer->actual_length > 0)) {
		// Handle debug data.
		debugTranslator(handle, transfer->buffer, (size_t) transfer->actual_length);
	}

	if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
		// Submit transfer again.
		if (libusb_submit_transfer(transfer) == LIBUSB_SUCCESS) {
			return;
		}
	}

	// Cannot recover (cancelled, no device, or other critical error).
	// Signal this by adjusting the counter and exiting.
	// Freeing the transfers is taken care of by cancelAndDeallocateDebugTransfers().
	atomic_fetch_sub(&handle->fx3Support.activeDebugTransfers, 1);
}

static void debugTranslator(davisHandle handle, const uint8_t *buffer, size_t bytesSent) {
	// Check if this is a debug message (length 7-64 bytes).
	if ((bytesSent >= 7) && (buffer[0] == 0x00)) {
		// Debug message, log this.
		davisLog(CAER_LOG_ERROR, handle, "Error message: '%s' (code %u at time %u).", &buffer[6], buffer[1],
			*((const uint32_t *) &buffer[2]));
	}
	else {
		// Unknown/invalid debug message, log this.
		davisLog(CAER_LOG_WARNING, handle, "Unknown/invalid debug message.");
	}
}

// For special Raspberry Pi version usage.
#if defined(OS_LINUX)
bool davisRPiROIConfigure(caerDeviceHandle cdh, uint16_t startX, uint16_t startY, uint16_t endX, uint16_t endY);
#else
bool davisRPiROIConfigure(caerDeviceHandle cdh, uint16_t startX, uint16_t startY, uint16_t endX, uint16_t endY) {
	return (false);
}
#endif

bool caerDavisROIConfigure(caerDeviceHandle cdh, uint16_t startX, uint16_t startY, uint16_t endX, uint16_t endY) {
	davisHandle handle = (davisHandle) cdh;

	// Check if the pointer is valid.
	if (handle == NULL) {
		return (false);
	}

	// Check if device type is supported.
	if ((handle->deviceType != CAER_DEVICE_DAVIS) && (handle->deviceType != CAER_DEVICE_DAVIS_FX2)
		&& (handle->deviceType != CAER_DEVICE_DAVIS_FX3) && (handle->deviceType != CAER_DEVICE_DAVIS_RPI)) {
		return (false);
	}

	// Check that end >= start.
	if ((startX > endX) || (startY > endY)) {
		return (false);
	}

	// Dispatch to Raspberry Pi version if that is the device.
	if (handle->deviceType == CAER_DEVICE_DAVIS_RPI) {
		return (davisRPiROIConfigure(cdh, startX, startY, endX, endY));
	}

	// 5 commands always (disable, four coordinates).
	uint16_t commandsNumber = 5;

	// 6 commands if enable also has to be sent.
	uint32_t isEnabled = 0;
	spiConfigReceive(&handle->usbState, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_RUN, &isEnabled);

	if (isEnabled) {
		commandsNumber++;
	}

	// First disable, then set all four coordinates, then enable again IF requested.
	struct spi_config_params spiMultiConfig[commandsNumber];

	for (size_t i = 0; i < commandsNumber; i++) {
		spiMultiConfig[i].moduleAddr = DAVIS_CONFIG_APS;

		switch (i) {
			case 0: // Disable.
				spiMultiConfig[i].paramAddr = DAVIS_CONFIG_APS_RUN;
				spiMultiConfig[i].param     = false;
				break;

			case 1: // StartX.
				spiMultiConfig[i].paramAddr = DAVIS_CONFIG_APS_START_COLUMN_0;
				spiMultiConfig[i].param     = startX;
				break;

			case 2: // StartY.
				spiMultiConfig[i].paramAddr = DAVIS_CONFIG_APS_START_ROW_0;
				spiMultiConfig[i].param     = startY;
				break;

			case 3: // EndX.
				spiMultiConfig[i].paramAddr = DAVIS_CONFIG_APS_END_COLUMN_0;
				spiMultiConfig[i].param     = endX;
				break;

			case 4: // EndY.
				spiMultiConfig[i].paramAddr = DAVIS_CONFIG_APS_END_ROW_0;
				spiMultiConfig[i].param     = endY;
				break;

			case 5: // Enable.
				spiMultiConfig[i].paramAddr = DAVIS_CONFIG_APS_RUN;
				spiMultiConfig[i].param     = true;
				break;
		}
	}

	return (spiConfigSendMultiple(&handle->usbState, spiMultiConfig, commandsNumber));
}
