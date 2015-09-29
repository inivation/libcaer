#include "davis_common.h"

static bool spiConfigSend(libusb_device_handle *devHandle, uint8_t moduleAddr, uint8_t paramAddr, uint32_t param);
static bool spiConfigReceive(libusb_device_handle *devHandle, uint8_t moduleAddr, uint8_t paramAddr, uint32_t *param);
static libusb_device_handle *davisDeviceOpen(libusb_context *devContext, uint16_t devVID, uint16_t devPID,
	uint8_t devType, uint8_t busNumber, uint8_t devAddress, const char *serialNumber, uint16_t requiredLogicRevision,
	uint16_t requiredFirmwareVersion);
static void davisDeviceClose(libusb_device_handle *devHandle);
static void davisAllocateTransfers(davisHandle handle, uint32_t bufferNum, uint32_t bufferSize);
static void davisDeallocateTransfers(davisHandle handle);
static void LIBUSB_CALL davisLibUsbCallback(struct libusb_transfer *transfer);
static void davisEventTranslator(davisHandle handle, uint8_t *buffer, size_t bytesSent);
static void *davisDataAcquisitionThread(void *inPtr);
static void davisDataAcquisitionThreadConfig(davisHandle handle);

static inline void checkStrictMonotonicTimestamp(davisHandle handle) {
	if (handle->state.currentTimestamp <= handle->state.lastTimestamp) {
		caerLog(CAER_LOG_ALERT, handle->info.deviceString,
			"Timestamps: non strictly-monotonic timestamp detected: lastTimestamp=%" PRIi32 ", currentTimestamp=%" PRIi32 ", difference=%" PRIi32 ".",
			handle->state.lastTimestamp, handle->state.currentTimestamp,
			(handle->state.lastTimestamp - handle->state.currentTimestamp));
	}
}

static inline void initFrame(davisHandle handle) {
	handle->state.apsCurrentReadoutType = APS_READOUT_RESET;
	for (size_t j = 0; j < APS_READOUT_TYPES_NUM; j++) {
		handle->state.apsCountX[j] = 0;
		handle->state.apsCountY[j] = 0;
	}

	memset(&handle->state.currentFrameEvent, 0, sizeof(struct caer_frame_event));

	// Write out start of frame timestamp.
	caerFrameEventSetTSStartOfFrame(&handle->state.currentFrameEvent, handle->state.currentTimestamp);

	// Setup frame.
	caerFrameEventAllocatePixels(&handle->state.currentFrameEvent, handle->state.apsWindow0SizeX,
		handle->state.apsWindow0SizeY, 1);
}

static inline float calculateIMUAccelScale(uint8_t imuAccelScale) {
	// Accelerometer scale is:
	// 0 - +-2 g - 16384 LSB/g
	// 1 - +-4 g - 8192 LSB/g
	// 2 - +-8 g - 4096 LSB/g
	// 3 - +-16 g - 2048 LSB/g
	float accelScale = 65536.0f / (float) U32T(4 * (1 << imuAccelScale));

	return (accelScale);
}

static inline float calculateIMUGyroScale(uint8_t imuGyroScale) {
	// Gyroscope scale is:
	// 0 - +-250 °/s - 131 LSB/°/s
	// 1 - +-500 °/s - 65.5 LSB/°/s
	// 2 - +-1000 °/s - 32.8 LSB/°/s
	// 3 - +-2000 °/s - 16.4 LSB/°/s
	float gyroScale = 65536.0f / (float) U32T(500 * (1 << imuGyroScale));

	return (gyroScale);
}

static inline void freeAllDataMemory(davisState state) {
	if (state->dataExchangeBuffer != NULL) {
		ringBufferFree(state->dataExchangeBuffer);
		state->dataExchangeBuffer = NULL;
	}

	// Since the current event packets aren't necessarily
	// already assigned to the current packet container, we
	// free them separately from it.
	if (state->currentPolarityPacket != NULL) {
		caerEventPacketFree(&state->currentPolarityPacket->packetHeader);
		state->currentPolarityPacket = NULL;

		if (state->currentPacketContainer != NULL) {
			caerEventPacketContainerSetEventPacket(state->currentPacketContainer, POLARITY_EVENT, NULL);
		}
	}

	if (state->currentSpecialPacket != NULL) {
		caerEventPacketFree(&state->currentSpecialPacket->packetHeader);
		state->currentSpecialPacket = NULL;

		if (state->currentPacketContainer != NULL) {
			caerEventPacketContainerSetEventPacket(state->currentPacketContainer, SPECIAL_EVENT, NULL);
		}
	}

	if (state->currentFramePacket != NULL) {
		caerEventPacketFree(&state->currentFramePacket->packetHeader);
		state->currentFramePacket = NULL;

		if (state->currentPacketContainer != NULL) {
			caerEventPacketContainerSetEventPacket(state->currentPacketContainer, FRAME_EVENT, NULL);
		}
	}

	if (state->currentIMU6Packet != NULL) {
		caerEventPacketFree(&state->currentIMU6Packet->packetHeader);
		state->currentIMU6Packet = NULL;

		if (state->currentPacketContainer != NULL) {
			caerEventPacketContainerSetEventPacket(state->currentPacketContainer, IMU6_EVENT, NULL);
		}
	}

	if (state->currentPacketContainer != NULL) {
		caerEventPacketContainerFree(state->currentPacketContainer);
		state->currentPacketContainer = NULL;
	}

	if (state->apsCurrentResetFrame != NULL) {
		free(state->apsCurrentResetFrame);
		state->apsCurrentResetFrame = NULL;
	}

	// Also free possible pixel array for current private frame event.
	free(state->currentFrameEvent.pixels);
}

bool davisCommonOpen(davisHandle handle, uint16_t VID, uint16_t PID, uint8_t DID_TYPE, const char *deviceName,
	uint16_t deviceID, uint8_t busNumberRestrict, uint8_t devAddressRestrict, const char *serialNumberRestrict,
	uint16_t requiredLogicRevision, uint16_t requiredFirmwareVersion) {
	davisState state = &handle->state;

	// Initialize state variables to default values (if not zero, taken care of by calloc above).
	atomic_store(&state->dataExchangeBufferSize, 64);
	atomic_store(&state->dataExchangeBlocking, false);
	atomic_store(&state->usbBufferNumber, 8);
	atomic_store(&state->usbBufferSize, 4096);

	// Packet settings (size (in events) and time interval (in µs)).
	atomic_store(&state->maxPacketContainerSize, 4096 + 128 + 4 + 8);
	atomic_store(&state->maxPacketContainerInterval, 5000);
	atomic_store(&state->maxPolarityPacketSize, 4096);
	atomic_store(&state->maxPolarityPacketInterval, 5000);
	atomic_store(&state->maxSpecialPacketSize, 128);
	atomic_store(&state->maxSpecialPacketInterval, 1000);
	atomic_store(&state->maxFramePacketSize, 4);
	atomic_store(&state->maxFramePacketInterval, 50000);
	atomic_store(&state->maxIMU6PacketSize, 8);
	atomic_store(&state->maxIMU6PacketInterval, 5000);

	// Search for device and open it.
	// Initialize libusb using a separate context for each device.
	// This is to correctly support one thread per device.
	if ((errno = libusb_init(&state->deviceContext)) != LIBUSB_SUCCESS) {
		caerLog(CAER_LOG_CRITICAL, __func__, "Failed to initialize libusb context. Error: %d.", errno);
		return (false);
	}

	// Try to open a DAVIS device on a specific USB port.
	state->deviceHandle = davisDeviceOpen(state->deviceContext, VID, PID, DID_TYPE, busNumberRestrict,
		devAddressRestrict, serialNumberRestrict, requiredLogicRevision, requiredFirmwareVersion);
	if (state->deviceHandle == NULL) {
		libusb_exit(state->deviceContext);

		caerLog(CAER_LOG_CRITICAL, __func__, "Failed to open %s device.", deviceName);
		return (false);
	}

	// At this point we can get some more precise data on the device and update
	// the logging string to reflect that and be more informative.
	uint8_t busNumber = libusb_get_bus_number(libusb_get_device(state->deviceHandle));
	uint8_t devAddress = libusb_get_device_address(libusb_get_device(state->deviceHandle));

	char serialNumber[8 + 1] = { 0 };
	int getStringDescResult = libusb_get_string_descriptor_ascii(state->deviceHandle, 3, (unsigned char *) serialNumber,
		8);

	// Check serial number success and length.
	if (getStringDescResult < 0 || getStringDescResult > 8) {
		davisDeviceClose(state->deviceHandle);
		libusb_exit(state->deviceContext);

		caerLog(CAER_LOG_CRITICAL, __func__, "Unable to get serial number for %s device.", deviceName);
		return (false);
	}

	size_t fullLogStringLength = (size_t) snprintf(NULL, 0, "%s ID-%" PRIu16 " SN-%s [%" PRIu8 ":%" PRIu8 "]",
		deviceName, deviceID, serialNumber, busNumber, devAddress);

	char *fullLogString = malloc(fullLogStringLength + 1);
	if (fullLogString == NULL) {
		davisDeviceClose(state->deviceHandle);
		libusb_exit(state->deviceContext);

		caerLog(CAER_LOG_CRITICAL, __func__, "Unable to allocate memory for %s device info string.", deviceName);
		return (false);
	}

	snprintf(fullLogString, fullLogStringLength + 1, "%s ID-%" PRIu16 " SN-%s [%" PRIu8 ":%" PRIu8 "]", deviceName,
		deviceID, serialNumber, busNumber, devAddress);

	// Populate info variables based on data from device.
	uint32_t param32 = 0;

	handle->info.deviceID = deviceID;
	handle->info.deviceString = fullLogString;
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_LOGIC_VERSION, &param32);
	handle->info.logicVersion = U16T(param32);
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_DEVICE_IS_MASTER, &param32);
	handle->info.deviceIsMaster = param32;
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_LOGIC_CLOCK, &param32);
	handle->info.logicClock = U16T(param32);
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_ADC_CLOCK, &param32);
	handle->info.adcClock = U16T(param32);
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_CHIP_IDENTIFIER, &param32);
	handle->info.chipID = U16T(param32);
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_HAS_PIXEL_FILTER, &param32);
	handle->info.dvsHasPixelFilter = param32;
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_HAS_BACKGROUND_ACTIVITY_FILTER, &param32);
	handle->info.dvsHasBackgroundActivityFilter = param32;
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_HAS_TEST_EVENT_GENERATOR, &param32);
	handle->info.dvsHasTestEventGenerator = param32;

	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_COLOR_FILTER, &param32);
	handle->info.apsColorFilter = U8T(param32);
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_HAS_GLOBAL_SHUTTER, &param32);
	handle->info.apsHasGlobalShutter = param32;
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_HAS_QUAD_ROI, &param32);
	handle->info.apsHasQuadROI = param32;
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_HAS_EXTERNAL_ADC, &param32);
	handle->info.apsHasExternalADC = param32;
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_HAS_INTERNAL_ADC, &param32);
	handle->info.apsHasInternalADC = param32;

	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_HAS_GENERATOR, &param32);
	handle->info.extInputHasGenerator = param32;

	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_SIZE_COLUMNS, &param32);
	state->dvsSizeX = U16T(param32);
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_SIZE_ROWS, &param32);
	state->dvsSizeY = U16T(param32);

	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_ORIENTATION_INFO, &param32);
	state->dvsInvertXY = U16T(param32) & 0x04;

	if (state->dvsInvertXY) {
		handle->info.dvsSizeX = state->dvsSizeY;
		handle->info.dvsSizeY = state->dvsSizeX;
	}
	else {
		handle->info.dvsSizeX = state->dvsSizeX;
		handle->info.dvsSizeY = state->dvsSizeY;
	}

	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_SIZE_COLUMNS, &param32);
	state->apsSizeX = U16T(param32);
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_SIZE_ROWS, &param32);
	state->apsSizeY = U16T(param32);

	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_ORIENTATION_INFO, &param32);
	uint16_t apsOrientationInfo = U16T(param32);
	state->apsInvertXY = apsOrientationInfo & 0x04;
	state->apsFlipX = apsOrientationInfo & 0x02;
	state->apsFlipY = apsOrientationInfo & 0x01;

	if (state->apsInvertXY) {
		handle->info.apsSizeX = state->apsSizeY;
		handle->info.apsSizeY = state->apsSizeX;
	}
	else {
		handle->info.apsSizeX = state->apsSizeX;
		handle->info.apsSizeY = state->apsSizeY;
	}

	caerLog(CAER_LOG_DEBUG, fullLogString, "Initialized device successfully with USB Bus=%" PRIu8 ":Addr=%" PRIu8 ".",
		busNumber, devAddress);

	return (true);
}

bool davisCommonClose(davisHandle handle) {
	davisState state = &handle->state;

	// Finally, close the device fully.
	davisDeviceClose(state->deviceHandle);

	// Destroy libusb context.
	libusb_exit(state->deviceContext);

	caerLog(CAER_LOG_DEBUG, handle->info.deviceString, "Shutdown successful.");

	// Free memory.
	free(handle->info.deviceString);
	free(handle);

	return (true);
}

caerDavisInfo caerDavisInfoGet(caerDeviceHandle cdh) {
	davisHandle handle = (davisHandle) cdh;

	// Return a link to the device information.
	return (&handle->info);
}

bool davisCommonSendDefaultFPGAConfig(caerDeviceHandle cdh,
bool (*configSet)(caerDeviceHandle cdh, int8_t modAddr, uint8_t paramAddr, uint32_t param)) {
	davisHandle handle = (davisHandle) cdh;
	davisState state = &handle->state;

	(*configSet)(cdh, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_TIMESTAMP_RESET, false);
	(*configSet)(cdh, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_FORCE_CHIP_BIAS_ENABLE, false);
	(*configSet)(cdh, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_DROP_DVS_ON_TRANSFER_STALL, true);
	(*configSet)(cdh, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_DROP_APS_ON_TRANSFER_STALL, false);
	(*configSet)(cdh, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_DROP_IMU_ON_TRANSFER_STALL, false);
	(*configSet)(cdh, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_DROP_EXTINPUT_ON_TRANSFER_STALL, true);

	(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_ACK_DELAY_ROW, 4); // in cycles @ LogicClock
	(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_ACK_DELAY_COLUMN, 0); // in cycles @ LogicClock
	(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_ACK_EXTENSION_ROW, 1); // in cycles @ LogicClock
	(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_ACK_EXTENSION_COLUMN, 0); // in cycles @ LogicClock
	(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_WAIT_ON_TRANSFER_STALL, false);
	(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_ROW_ONLY_EVENTS, true);
	(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_EXTERNAL_AER_CONTROL, false);
	if (handle->info.dvsHasPixelFilter) {
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_0_ROW, state->dvsSizeY);
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_0_COLUMN, state->dvsSizeX);
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_1_ROW, state->dvsSizeY);
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_1_COLUMN, state->dvsSizeX);
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_2_ROW, state->dvsSizeY);
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_2_COLUMN, state->dvsSizeX);
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_3_ROW, state->dvsSizeY);
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_3_COLUMN, state->dvsSizeX);
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_4_ROW, state->dvsSizeY);
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_4_COLUMN, state->dvsSizeX);
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_5_ROW, state->dvsSizeY);
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_5_COLUMN, state->dvsSizeX);
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_6_ROW, state->dvsSizeY);
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_6_COLUMN, state->dvsSizeX);
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_7_ROW, state->dvsSizeY);
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_PIXEL_7_COLUMN, state->dvsSizeX);
	}
	if (handle->info.dvsHasBackgroundActivityFilter) {
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_BACKGROUND_ACTIVITY, true);
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_FILTER_BACKGROUND_ACTIVITY_DELTAT, 20000);	// in µs
	}
	if (handle->info.dvsHasTestEventGenerator) {
		(*configSet)(cdh, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_TEST_EVENT_GENERATOR_ENABLE, false);
	}

	(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_RESET_READ, true);
	(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_WAIT_ON_TRANSFER_STALL, true);
	(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_GLOBAL_SHUTTER, handle->info.apsHasGlobalShutter);
	(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_START_COLUMN_0, 0);
	(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_START_ROW_0, 0);
	(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_END_COLUMN_0, U16T(state->apsSizeX - 1));
	(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_END_ROW_0, U16T(state->apsSizeY - 1));
	(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_EXPOSURE, 4000); // in µs, converted to cycles @ ADCClock later
	(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_FRAME_DELAY, 1000); // in µs, converted to cycles @ ADCClock later
	(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_RESET_SETTLE, 10); // in cycles @ ADCClock
	(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_COLUMN_SETTLE, 30); // in cycles @ ADCClock
	(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_ROW_SETTLE, 8); // in cycles @ ADCClock
	(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_NULL_SETTLE, 3); // in cycles @ ADCClock
	if (handle->info.apsHasQuadROI) {
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_START_COLUMN_1, state->apsSizeX);
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_START_ROW_1, state->apsSizeY);
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_END_COLUMN_1, state->apsSizeX);
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_END_ROW_1, state->apsSizeY);
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_START_COLUMN_2, state->apsSizeX);
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_START_ROW_2, state->apsSizeY);
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_END_COLUMN_2, state->apsSizeX);
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_END_ROW_2, state->apsSizeY);
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_START_COLUMN_3, state->apsSizeX);
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_START_ROW_3, state->apsSizeY);
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_END_COLUMN_3, state->apsSizeX);
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_END_ROW_3, state->apsSizeY);
	}
	if (handle->info.apsHasInternalADC) {
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_USE_INTERNAL_ADC, true);
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_SAMPLE_ENABLE, true);
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_SAMPLE_SETTLE, 60); // in cycles @ ADCClock
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_RAMP_RESET, 10); // in cycles @ ADCClock
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_RAMP_SHORT_RESET, true);
	}
	if (IS_RGB(handle->info.chipID)) {
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVISRGB_CONFIG_APS_TRANSFER, 3000); // in cycles @ ADCClock
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVISRGB_CONFIG_APS_RSFDSETTLE, 3000); // in cycles @ ADCClock
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVISRGB_CONFIG_APS_GSPDRESET, 3000); // in cycles @ ADCClock
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVISRGB_CONFIG_APS_GSRESETFALL, 3000); // in cycles @ ADCClock
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVISRGB_CONFIG_APS_GSTXFALL, 3000); // in cycles @ ADCClock
		(*configSet)(cdh, DAVIS_CONFIG_APS, DAVISRGB_CONFIG_APS_GSFDRESET, 3000); // in cycles @ ADCClock
	}

	(*configSet)(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_TEMP_STANDBY, false);
	(*configSet)(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_ACCEL_STANDBY, false);
	(*configSet)(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_GYRO_STANDBY, false);
	(*configSet)(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_LP_CYCLE, false);
	(*configSet)(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_LP_WAKEUP, 1);
	(*configSet)(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_SAMPLE_RATE_DIVIDER, 0);
	(*configSet)(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_DIGITAL_LOW_PASS_FILTER, 1);
	(*configSet)(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_ACCEL_FULL_SCALE, 1);
	(*configSet)(cdh, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_GYRO_FULL_SCALE, 1);

	(*configSet)(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_DETECT_RISING_EDGES, false);
	(*configSet)(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_DETECT_FALLING_EDGES, false);
	(*configSet)(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_DETECT_PULSES, true);
	(*configSet)(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_DETECT_PULSE_POLARITY, true);
	(*configSet)(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_DETECT_PULSE_LENGTH, 10); // in cycles @ LogicClock

	if (handle->info.extInputHasGenerator) {
		// Disable generator by default. Has to be enabled manually after sendDefaultConfig() by user!
		(*configSet)(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_RUN_GENERATOR, false);
		(*configSet)(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_GENERATE_USE_CUSTOM_SIGNAL, false);
		(*configSet)(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_POLARITY, true);
		(*configSet)(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_INTERVAL, 10); // in cycles @ LogicClock
		(*configSet)(cdh, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_LENGTH, 5); // in cycles @ LogicClock
	}

	(*configSet)(cdh, DAVIS_CONFIG_USB, DAVIS_CONFIG_USB_EARLY_PACKET_DELAY, 8); // in 125µs time-slices (defaults to 1ms)

	return (true);
}

bool davisCommonSendDefaultChipConfig(caerDeviceHandle cdh,
bool (*configSet)(caerDeviceHandle cdh, int8_t modAddr, uint8_t paramAddr, uint32_t param)) {
	davisHandle handle = (davisHandle) cdh;

	// Default bias configuration.
	if (IS_240(handle->info.chipID)) {
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_DIFFBN,
			caerBiasGenerateCoarseFine(4, 39, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_ONBN,
			caerBiasGenerateCoarseFine(5, 255, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_OFFBN,
			caerBiasGenerateCoarseFine(4, 0, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_APSCASEPC,
			caerBiasGenerateCoarseFine(5, 185, true, true, false, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_DIFFCASBNC,
			caerBiasGenerateCoarseFine(5, 115, true, true, false, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_APSROSFBN,
			caerBiasGenerateCoarseFine(6, 219, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_LOCALBUFBN,
			caerBiasGenerateCoarseFine(5, 164, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_PIXINVBN,
			caerBiasGenerateCoarseFine(5, 129, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_PRBP,
			caerBiasGenerateCoarseFine(2, 58, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_PRSFBP,
			caerBiasGenerateCoarseFine(1, 16, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_REFRBP,
			caerBiasGenerateCoarseFine(4, 25, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_AEPDBN,
			caerBiasGenerateCoarseFine(6, 91, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_LCOLTIMEOUTBN,
			caerBiasGenerateCoarseFine(5, 49, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_AEPUXBP,
			caerBiasGenerateCoarseFine(4, 80, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_AEPUYBP,
			caerBiasGenerateCoarseFine(7, 152, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_IFTHRBN,
			caerBiasGenerateCoarseFine(5, 255, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_IFREFRBN,
			caerBiasGenerateCoarseFine(5, 255, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_PADFOLLBN,
			caerBiasGenerateCoarseFine(7, 215, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_APSOVERFLOWLEVEL,
			caerBiasGenerateCoarseFine(6, 253, true, true, true, true));

		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_BIASBUFFER,
			caerBiasGenerateCoarseFine(5, 254, true, true, true, true));

		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_SSP,
			caerBiasGenerateShiftedSource(33, 20, SHIFTED_SOURCE, SPLIT_GATE));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS240_CONFIG_BIAS_SSN,
			caerBiasGenerateShiftedSource(33, 21, SHIFTED_SOURCE, SPLIT_GATE));

	}

	if (IS_128(handle->info.chipID) || IS_208(handle->info.chipID)
	|| IS_346(handle->info.chipID) || IS_640(handle->info.chipID)) {
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_APSOVERFLOWLEVEL, caerBiasGenerateVDAC(27, 6));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_APSCAS, caerBiasGenerateVDAC(21, 6));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_ADCREFHIGH, caerBiasGenerateVDAC(30, 7));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_ADCREFLOW, caerBiasGenerateVDAC(1, 7));

		if (IS_346(handle->info.chipID) || IS_640(handle->info.chipID)) {
			// Only DAVIS346 and 640 have ADC testing.
			(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS346_CONFIG_BIAS_ADCTESTVOLTAGE, caerBiasGenerateVDAC(21, 7));
		}

		if (IS_208(handle->info.chipID)) {
			(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS208_CONFIG_BIAS_RESETHIGHPASS, caerBiasGenerateVDAC(63, 7));
			(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS208_CONFIG_BIAS_REFSS, caerBiasGenerateVDAC(11, 5));

			(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS208_CONFIG_BIAS_REGBIASBP,
				caerBiasGenerateCoarseFine(5, 20, true, false, true, true));
			(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS208_CONFIG_BIAS_REFSSBN,
				caerBiasGenerateCoarseFine(5, 20, true, true, true, true));
		}

		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_LOCALBUFBN,
			caerBiasGenerateCoarseFine(5, 164, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_PADFOLLBN,
			caerBiasGenerateCoarseFine(7, 215, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_DIFFBN,
			caerBiasGenerateCoarseFine(4, 39, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_ONBN,
			caerBiasGenerateCoarseFine(5, 255, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_OFFBN,
			caerBiasGenerateCoarseFine(4, 1, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_PIXINVBN,
			caerBiasGenerateCoarseFine(5, 129, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_PRBP,
			caerBiasGenerateCoarseFine(2, 58, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_PRSFBP,
			caerBiasGenerateCoarseFine(1, 16, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_REFRBP,
			caerBiasGenerateCoarseFine(4, 25, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_READOUTBUFBP,
			caerBiasGenerateCoarseFine(6, 20, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_APSROSFBN,
			caerBiasGenerateCoarseFine(6, 219, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_ADCCOMPBP,
			caerBiasGenerateCoarseFine(5, 20, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_COLSELLOWBN,
			caerBiasGenerateCoarseFine(0, 1, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_DACBUFBP,
			caerBiasGenerateCoarseFine(6, 60, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_LCOLTIMEOUTBN,
			caerBiasGenerateCoarseFine(5, 49, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_AEPDBN,
			caerBiasGenerateCoarseFine(6, 91, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_AEPUXBP,
			caerBiasGenerateCoarseFine(4, 80, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_AEPUYBP,
			caerBiasGenerateCoarseFine(7, 152, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_IFREFRBN,
			caerBiasGenerateCoarseFine(5, 255, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_IFTHRBN,
			caerBiasGenerateCoarseFine(5, 255, true, true, true, true));

		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_BIASBUFFER,
			caerBiasGenerateCoarseFine(5, 254, true, true, true, true));

		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_SSP,
			caerBiasGenerateShiftedSource(1, 33, SHIFTED_SOURCE, SPLIT_GATE));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS128_CONFIG_BIAS_SSN,
			caerBiasGenerateShiftedSource(1, 33, SHIFTED_SOURCE, SPLIT_GATE));

		if (IS_640(handle->info.chipID)) {
			// Slow down pixels for big 640x480 array, to avoid overwhelming the AER bus.
			(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS640_CONFIG_BIAS_PRBP,
				caerBiasGenerateCoarseFine(2, 3, true, false, true, true));
			(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVIS640_CONFIG_BIAS_PRSFBP,
				caerBiasGenerateCoarseFine(1, 1, true, false, true, true));
		}
	}

	if (IS_RGB(handle->info.chipID)) {
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_APSCAS, caerBiasGenerateVDAC(21, 4));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_OVG1LO, caerBiasGenerateVDAC(21, 4));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_OVG2LO, caerBiasGenerateVDAC(0, 0));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_TX2OVG2HI, caerBiasGenerateVDAC(63, 0));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_GND07, caerBiasGenerateVDAC(13, 4));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_ADCTESTVOLTAGE, caerBiasGenerateVDAC(21, 0));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_ADCREFHIGH, caerBiasGenerateVDAC(63, 7));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_ADCREFLOW, caerBiasGenerateVDAC(0, 7));

		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_IFREFRBN,
			caerBiasGenerateCoarseFine(5, 255, false, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_IFTHRBN,
			caerBiasGenerateCoarseFine(5, 255, false, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_LOCALBUFBN,
			caerBiasGenerateCoarseFine(5, 164, false, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_PADFOLLBN,
			caerBiasGenerateCoarseFine(7, 209, false, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_PIXINVBN,
			caerBiasGenerateCoarseFine(4, 164, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_DIFFBN,
			caerBiasGenerateCoarseFine(4, 54, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_ONBN,
			caerBiasGenerateCoarseFine(6, 63, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_OFFBN,
			caerBiasGenerateCoarseFine(2, 138, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_PRBP,
			caerBiasGenerateCoarseFine(1, 108, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_PRSFBP,
			caerBiasGenerateCoarseFine(1, 108, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_REFRBP,
			caerBiasGenerateCoarseFine(4, 28, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_ARRAYBIASBUFFERBN,
			caerBiasGenerateCoarseFine(6, 128, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_ARRAYLOGICBUFFERBN,
			caerBiasGenerateCoarseFine(5, 255, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_FALLTIMEBN,
			caerBiasGenerateCoarseFine(7, 41, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_RISETIMEBP,
			caerBiasGenerateCoarseFine(6, 162, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_READOUTBUFBP,
			caerBiasGenerateCoarseFine(6, 20, false, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_APSROSFBN,
			caerBiasGenerateCoarseFine(6, 255, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_ADCCOMPBP,
			caerBiasGenerateCoarseFine(4, 159, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_DACBUFBP,
			caerBiasGenerateCoarseFine(6, 194, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_LCOLTIMEOUTBN,
			caerBiasGenerateCoarseFine(5, 49, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_AEPDBN,
			caerBiasGenerateCoarseFine(6, 91, true, true, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_AEPUXBP,
			caerBiasGenerateCoarseFine(4, 80, true, false, true, true));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_AEPUYBP,
			caerBiasGenerateCoarseFine(7, 152, true, false, true, true));

		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_BIASBUFFER,
			caerBiasGenerateCoarseFine(6, 251, true, true, true, true));

		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_SSP,
			caerBiasGenerateShiftedSource(1, 33, TIED_TO_RAIL, SPLIT_GATE));
		(*configSet)(cdh, DAVIS_CONFIG_BIAS, DAVISRGB_CONFIG_BIAS_SSN,
			caerBiasGenerateShiftedSource(2, 33, SHIFTED_SOURCE, SPLIT_GATE));
	}

	// Default chip configuration.
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_DIGITALMUX0, 0);
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_DIGITALMUX1, 0);
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_DIGITALMUX2, 0);
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_DIGITALMUX3, 0);
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_ANALOGMUX0, 0);
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_ANALOGMUX1, 0);
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_ANALOGMUX2, 0);
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_BIASMUX0, 0);
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_RESETCALIBNEURON, true);
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_TYPENCALIBNEURON, false);
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_RESETTESTPIXEL, true);
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_AERNAROW, false);  // Use nArow in the AER state machine.
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_USEAOUT, false); // Enable analog pads for aMUX output (testing).
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_GLOBAL_SHUTTER, handle->info.apsHasGlobalShutter);

	// Special extra pixels control for DAVIS240 A/B.
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS240_CONFIG_CHIP_SPECIALPIXELCONTROL, false);

	// Select which grey counter to use with the internal ADC: '0' means the external grey counter is used, which
	// has to be supplied off-chip. '1' means the on-chip grey counter is used instead.
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_SELECTGRAYCOUNTER, 1);

	// Test ADC functionality: if true, the ADC takes its input voltage not from the pixel, but from the
	// VDAC 'AdcTestVoltage'. If false, the voltage comes from the pixels.
	(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS346_CONFIG_CHIP_TESTADC, false);

	if (IS_208(handle->info.chipID)) {
		(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS208_CONFIG_CHIP_SELECTPREAMPAVG, false);
		(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS208_CONFIG_CHIP_SELECTBIASREFSS, false);
		(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS208_CONFIG_CHIP_SELECTSENSE, true);
		(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS208_CONFIG_CHIP_SELECTPOSFB, false);
		(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVIS208_CONFIG_CHIP_SELECTHIGHPASS, false);
	}

	if (IS_RGB(handle->info.chipID)) {
		(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVISRGB_CONFIG_CHIP_ADJUSTOVG1LO, true);
		(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVISRGB_CONFIG_CHIP_ADJUSTOVG2LO, false);
		(*configSet)(cdh, DAVIS_CONFIG_CHIP, DAVISRGB_CONFIG_CHIP_ADJUSTTX2OVG2HI, false);
	}

	return (true);
}

bool davisCommonConfigSet(davisHandle handle, int8_t modAddr, uint8_t paramAddr, uint32_t param) {
	davisState state = &handle->state;

	switch (modAddr) {
		case CAER_HOST_CONFIG_USB:
			switch (paramAddr) {
				case CAER_HOST_CONFIG_USB_BUFFER_NUMBER:
					atomic_store(&state->usbBufferNumber, param);

					// Notify data acquisition thread to change buffers.
					atomic_fetch_or(&state->dataAcquisitionThreadConfigUpdate, 1 << 0);
					break;

				case CAER_HOST_CONFIG_USB_BUFFER_SIZE:
					atomic_store(&state->usbBufferSize, param);

					// Notify data acquisition thread to change buffers.
					atomic_fetch_or(&state->dataAcquisitionThreadConfigUpdate, 1 << 0);
					break;

				default:
					return (false);
					break;
			}
			break;

		case CAER_HOST_CONFIG_DATAEXCHANGE:
			switch (paramAddr) {
				case CAER_HOST_CONFIG_DATAEXCHANGE_BUFFER_SIZE:
					atomic_store(&state->dataExchangeBufferSize, param);
					break;

				case CAER_HOST_CONFIG_DATAEXCHANGE_BLOCKING:
					atomic_store(&state->dataExchangeBlocking, param);
					break;

				default:
					return (false);
					break;
			}
			break;

		case CAER_HOST_CONFIG_PACKETS:
			switch (paramAddr) {
				case CAER_HOST_CONFIG_PACKETS_MAX_CONTAINER_SIZE:
					atomic_store(&state->maxPacketContainerSize, param);
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_CONTAINER_INTERVAL:
					atomic_store(&state->maxPacketContainerInterval, param);
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_POLARITY_SIZE:
					atomic_store(&state->maxPolarityPacketSize, param);
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_POLARITY_INTERVAL:
					atomic_store(&state->maxPolarityPacketInterval, param);
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_SPECIAL_SIZE:
					atomic_store(&state->maxSpecialPacketSize, param);
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_SPECIAL_INTERVAL:
					atomic_store(&state->maxSpecialPacketInterval, param);
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_FRAME_SIZE:
					atomic_store(&state->maxFramePacketSize, param);
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_FRAME_INTERVAL:
					atomic_store(&state->maxFramePacketInterval, param);
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_IMU6_SIZE:
					atomic_store(&state->maxIMU6PacketSize, param);
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_IMU6_INTERVAL:
					atomic_store(&state->maxIMU6PacketInterval, param);
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
				case DAVIS_CONFIG_MUX_TIMESTAMP_RESET:
				case DAVIS_CONFIG_MUX_FORCE_CHIP_BIAS_ENABLE:
				case DAVIS_CONFIG_MUX_DROP_DVS_ON_TRANSFER_STALL:
				case DAVIS_CONFIG_MUX_DROP_APS_ON_TRANSFER_STALL:
				case DAVIS_CONFIG_MUX_DROP_IMU_ON_TRANSFER_STALL:
				case DAVIS_CONFIG_MUX_DROP_EXTINPUT_ON_TRANSFER_STALL:
					return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_MUX, paramAddr, param));
					break;

				default:
					return (false);
					break;
			}
			break;

		case DAVIS_CONFIG_DVS:
			switch (paramAddr) {
				case DAVIS_CONFIG_DVS_RUN:
				case DAVIS_CONFIG_DVS_ACK_DELAY_ROW:
				case DAVIS_CONFIG_DVS_ACK_DELAY_COLUMN:
				case DAVIS_CONFIG_DVS_ACK_EXTENSION_ROW:
				case DAVIS_CONFIG_DVS_ACK_EXTENSION_COLUMN:
				case DAVIS_CONFIG_DVS_WAIT_ON_TRANSFER_STALL:
				case DAVIS_CONFIG_DVS_FILTER_ROW_ONLY_EVENTS:
				case DAVIS_CONFIG_DVS_EXTERNAL_AER_CONTROL:
					return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_DVS, paramAddr, param));
					break;

				case DAVIS_CONFIG_DVS_FILTER_PIXEL_0_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_0_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_1_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_1_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_2_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_2_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_3_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_3_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_4_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_4_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_5_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_5_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_6_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_6_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_7_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_7_COLUMN:
					if (handle->info.dvsHasPixelFilter) {
						return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_FILTER_BACKGROUND_ACTIVITY:
				case DAVIS_CONFIG_DVS_FILTER_BACKGROUND_ACTIVITY_DELTAT:
					if (handle->info.dvsHasBackgroundActivityFilter) {
						return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_TEST_EVENT_GENERATOR_ENABLE:
					if (handle->info.dvsHasTestEventGenerator) {
						return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_DVS, paramAddr, param));
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
				case DAVIS_CONFIG_APS_RESET_READ:
				case DAVIS_CONFIG_APS_WAIT_ON_TRANSFER_STALL:
				case DAVIS_CONFIG_APS_START_COLUMN_0:
				case DAVIS_CONFIG_APS_START_ROW_0:
				case DAVIS_CONFIG_APS_END_COLUMN_0:
				case DAVIS_CONFIG_APS_END_ROW_0:
				case DAVIS_CONFIG_APS_RESET_SETTLE:
				case DAVIS_CONFIG_APS_COLUMN_SETTLE:
				case DAVIS_CONFIG_APS_ROW_SETTLE:
				case DAVIS_CONFIG_APS_NULL_SETTLE:
					return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_APS, paramAddr, param));
					break;

				case DAVIS_CONFIG_APS_EXPOSURE:
				case DAVIS_CONFIG_APS_FRAME_DELAY:
					// Exposure and Frame Delay are in µs, must be converted to native FPGA cycles
					// by multiplying with ADC clock value.
					return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_APS, paramAddr,
						param * handle->info.adcClock));
					break;

				case DAVIS_CONFIG_APS_GLOBAL_SHUTTER:
					if (handle->info.apsHasGlobalShutter) {
						// Keep in sync with chip config module GlobalShutter parameter.
						if (!spiConfigSend(state->deviceHandle, DAVIS_CONFIG_CHIP, DAVIS128_CONFIG_CHIP_GLOBAL_SHUTTER,
							param)) {
							return (false);
						}

						return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_APS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_APS_START_COLUMN_1:
				case DAVIS_CONFIG_APS_START_ROW_1:
				case DAVIS_CONFIG_APS_END_COLUMN_1:
				case DAVIS_CONFIG_APS_END_ROW_1:
				case DAVIS_CONFIG_APS_START_COLUMN_2:
				case DAVIS_CONFIG_APS_START_ROW_2:
				case DAVIS_CONFIG_APS_END_COLUMN_2:
				case DAVIS_CONFIG_APS_END_ROW_2:
				case DAVIS_CONFIG_APS_START_COLUMN_3:
				case DAVIS_CONFIG_APS_START_ROW_3:
				case DAVIS_CONFIG_APS_END_COLUMN_3:
				case DAVIS_CONFIG_APS_END_ROW_3:
					if (handle->info.apsHasQuadROI) {
						return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_APS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_APS_USE_INTERNAL_ADC:
				case DAVIS_CONFIG_APS_SAMPLE_ENABLE:
				case DAVIS_CONFIG_APS_SAMPLE_SETTLE:
				case DAVIS_CONFIG_APS_RAMP_RESET:
				case DAVIS_CONFIG_APS_RAMP_SHORT_RESET:
					if (handle->info.apsHasInternalADC) {
						return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_APS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVISRGB_CONFIG_APS_TRANSFER:
				case DAVISRGB_CONFIG_APS_RSFDSETTLE:
				case DAVISRGB_CONFIG_APS_GSPDRESET:
				case DAVISRGB_CONFIG_APS_GSRESETFALL:
				case DAVISRGB_CONFIG_APS_GSTXFALL:
				case DAVISRGB_CONFIG_APS_GSFDRESET:
					// Support for DAVISRGB extra timing parameters.
					if (IS_RGB(handle->info.chipID)) {
						return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_APS, paramAddr, param));
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

		case DAVIS_CONFIG_IMU:
			switch (paramAddr) {
				case DAVIS_CONFIG_IMU_RUN:
				case DAVIS_CONFIG_IMU_TEMP_STANDBY:
				case DAVIS_CONFIG_IMU_ACCEL_STANDBY:
				case DAVIS_CONFIG_IMU_GYRO_STANDBY:
				case DAVIS_CONFIG_IMU_LP_CYCLE:
				case DAVIS_CONFIG_IMU_LP_WAKEUP:
				case DAVIS_CONFIG_IMU_SAMPLE_RATE_DIVIDER:
				case DAVIS_CONFIG_IMU_DIGITAL_LOW_PASS_FILTER:
				case DAVIS_CONFIG_IMU_ACCEL_FULL_SCALE:
				case DAVIS_CONFIG_IMU_GYRO_FULL_SCALE:
					return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_IMU, paramAddr, param));
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
					return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_EXTINPUT, paramAddr, param));
					break;

				case DAVIS_CONFIG_EXTINPUT_RUN_GENERATOR:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_USE_CUSTOM_SIGNAL:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_POLARITY:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_INTERVAL:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_LENGTH:
					if (handle->info.extInputHasGenerator) {
						return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_EXTINPUT, paramAddr, param));
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
				if (IS_240(handle->info.chipID)) {
					// DAVIS240 uses the old bias generator with 22 branches, and uses all of them.
					if (paramAddr < 22) {
						return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_BIAS, paramAddr, param));
					}
				}
				else if (IS_128(handle->info.chipID) || IS_208(handle->info.chipID)
				|| IS_346(handle->info.chipID) || IS_640(handle->info.chipID)) {
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
							return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_BIAS, paramAddr, param));
							break;

						case DAVIS346_CONFIG_BIAS_ADCTESTVOLTAGE:
							// Only supported by DAVIS346 and DAVIS640 chips.
							if (IS_346(handle->info.chipID) || IS_640(handle->info.chipID)) {
								return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_BIAS, paramAddr, param));
							}
							break;

						case DAVIS208_CONFIG_BIAS_RESETHIGHPASS:
						case DAVIS208_CONFIG_BIAS_REFSS:
						case DAVIS208_CONFIG_BIAS_REGBIASBP:
						case DAVIS208_CONFIG_BIAS_REFSSBN:
							// Only supported by DAVIS208 chips.
							if (IS_208(handle->info.chipID)) {
								return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_BIAS, paramAddr, param));
							}
							break;

						default:
							return (false);
							break;
					}
				}
				else if (IS_RGB(handle->info.chipID)) {
					// DAVISRGB also uses the 37 branches bias generator, with different values.
					switch (paramAddr) {
						case DAVISRGB_CONFIG_BIAS_APSCAS:
						case DAVISRGB_CONFIG_BIAS_OVG1LO:
						case DAVISRGB_CONFIG_BIAS_OVG2LO:
						case DAVISRGB_CONFIG_BIAS_TX2OVG2HI:
						case DAVISRGB_CONFIG_BIAS_GND07:
						case DAVISRGB_CONFIG_BIAS_ADCTESTVOLTAGE:
						case DAVISRGB_CONFIG_BIAS_ADCREFHIGH:
						case DAVISRGB_CONFIG_BIAS_ADCREFLOW:
						case DAVISRGB_CONFIG_BIAS_IFREFRBN:
						case DAVISRGB_CONFIG_BIAS_IFTHRBN:
						case DAVISRGB_CONFIG_BIAS_LOCALBUFBN:
						case DAVISRGB_CONFIG_BIAS_PADFOLLBN:
						case DAVISRGB_CONFIG_BIAS_PIXINVBN:
						case DAVISRGB_CONFIG_BIAS_DIFFBN:
						case DAVISRGB_CONFIG_BIAS_ONBN:
						case DAVISRGB_CONFIG_BIAS_OFFBN:
						case DAVISRGB_CONFIG_BIAS_PRBP:
						case DAVISRGB_CONFIG_BIAS_PRSFBP:
						case DAVISRGB_CONFIG_BIAS_REFRBP:
						case DAVISRGB_CONFIG_BIAS_ARRAYBIASBUFFERBN:
						case DAVISRGB_CONFIG_BIAS_ARRAYLOGICBUFFERBN:
						case DAVISRGB_CONFIG_BIAS_FALLTIMEBN:
						case DAVISRGB_CONFIG_BIAS_RISETIMEBP:
						case DAVISRGB_CONFIG_BIAS_READOUTBUFBP:
						case DAVISRGB_CONFIG_BIAS_APSROSFBN:
						case DAVISRGB_CONFIG_BIAS_ADCCOMPBP:
						case DAVISRGB_CONFIG_BIAS_DACBUFBP:
						case DAVISRGB_CONFIG_BIAS_LCOLTIMEOUTBN:
						case DAVISRGB_CONFIG_BIAS_AEPDBN:
						case DAVISRGB_CONFIG_BIAS_AEPUXBP:
						case DAVISRGB_CONFIG_BIAS_AEPUYBP:
						case DAVISRGB_CONFIG_BIAS_BIASBUFFER:
						case DAVISRGB_CONFIG_BIAS_SSP:
						case DAVISRGB_CONFIG_BIAS_SSN:
							return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_BIAS, paramAddr, param));
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
						return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_CHIP, paramAddr, param));
						break;

					case DAVIS240_CONFIG_CHIP_SPECIALPIXELCONTROL:
						// Only supported by DAVIS240 A/B chips.
						if (IS_240A(handle->info.chipID) || IS_240B(handle->info.chipID)) {
							return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS128_CONFIG_CHIP_GLOBAL_SHUTTER:
						// Only supported by some chips.
						if (handle->info.apsHasGlobalShutter) {
							// Keep in sync with APS module GlobalShutter parameter.
							if (!spiConfigSend(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_GLOBAL_SHUTTER,
								param)) {
								return (false);
							}

							return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS128_CONFIG_CHIP_SELECTGRAYCOUNTER:
						// Only supported by the new DAVIS chips.
						if (IS_128(handle->info.chipID) || IS_208(handle->info.chipID) || IS_346(handle->info.chipID)
						|| IS_640(handle->info.chipID) || IS_RGB(handle->info.chipID)) {
							return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS346_CONFIG_CHIP_TESTADC:
						// Only supported by some of the new DAVIS chips.
						if (IS_346(handle->info.chipID) || IS_640(handle->info.chipID) || IS_RGB(handle->info.chipID)) {
							return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVISRGB_CONFIG_CHIP_ADJUSTOVG1LO: // Also DAVIS208_CONFIG_CHIP_SELECTPREAMPAVG.
					case DAVISRGB_CONFIG_CHIP_ADJUSTOVG2LO: // Also DAVIS208_CONFIG_CHIP_SELECTBIASREFSS.
					case DAVISRGB_CONFIG_CHIP_ADJUSTTX2OVG2HI: // Also DAVIS208_CONFIG_CHIP_SELECTSENSE.
						// Only supported by DAVIS208 and DAVISRGB.
						if (IS_208(handle->info.chipID) || IS_RGB(handle->info.chipID)) {
							return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS208_CONFIG_CHIP_SELECTPOSFB:
					case DAVIS208_CONFIG_CHIP_SELECTHIGHPASS:
						// Only supported by DAVIS208.
						if (IS_208(handle->info.chipID)) {
							return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_CHIP, paramAddr, param));
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
				case DAVIS_CONFIG_USB_EARLY_PACKET_DELAY:
					return (spiConfigSend(state->deviceHandle, DAVIS_CONFIG_USB, paramAddr, param));
					break;

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

bool davisCommonConfigGet(davisHandle handle, int8_t modAddr, uint8_t paramAddr, uint32_t *param) {
	davisState state = &handle->state;

	switch (modAddr) {
		case CAER_HOST_CONFIG_USB:
			switch (paramAddr) {
				case CAER_HOST_CONFIG_USB_BUFFER_NUMBER:
					*param = U32T(atomic_load(&state->usbBufferNumber));
					break;

				case CAER_HOST_CONFIG_USB_BUFFER_SIZE:
					*param = U32T(atomic_load(&state->usbBufferSize));
					break;

				default:
					return (false);
					break;
			}
			break;

		case CAER_HOST_CONFIG_DATAEXCHANGE:
			switch (paramAddr) {
				case CAER_HOST_CONFIG_DATAEXCHANGE_BUFFER_SIZE:
					*param = U32T(atomic_load(&state->dataExchangeBufferSize));
					break;

				case CAER_HOST_CONFIG_DATAEXCHANGE_BLOCKING:
					*param = atomic_load(&state->dataExchangeBlocking);
					break;

				default:
					return (false);
					break;
			}
			break;

		case CAER_HOST_CONFIG_PACKETS:
			switch (paramAddr) {
				case CAER_HOST_CONFIG_PACKETS_MAX_CONTAINER_SIZE:
					*param = U32T(atomic_load(&state->maxPacketContainerSize));
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_CONTAINER_INTERVAL:
					*param = U32T(atomic_load(&state->maxPacketContainerInterval));
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_POLARITY_SIZE:
					*param = U32T(atomic_load(&state->maxPolarityPacketSize));
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_POLARITY_INTERVAL:
					*param = U32T(atomic_load(&state->maxPolarityPacketInterval));
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_SPECIAL_SIZE:
					*param = U32T(atomic_load(&state->maxSpecialPacketSize));
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_SPECIAL_INTERVAL:
					*param = U32T(atomic_load(&state->maxSpecialPacketInterval));
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_FRAME_SIZE:
					*param = U32T(atomic_load(&state->maxFramePacketSize));
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_FRAME_INTERVAL:
					*param = U32T(atomic_load(&state->maxFramePacketInterval));
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_IMU6_SIZE:
					*param = U32T(atomic_load(&state->maxIMU6PacketSize));
					break;

				case CAER_HOST_CONFIG_PACKETS_MAX_IMU6_INTERVAL:
					*param = U32T(atomic_load(&state->maxIMU6PacketInterval));
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
				case DAVIS_CONFIG_MUX_TIMESTAMP_RESET:
				case DAVIS_CONFIG_MUX_FORCE_CHIP_BIAS_ENABLE:
				case DAVIS_CONFIG_MUX_DROP_DVS_ON_TRANSFER_STALL:
				case DAVIS_CONFIG_MUX_DROP_APS_ON_TRANSFER_STALL:
				case DAVIS_CONFIG_MUX_DROP_IMU_ON_TRANSFER_STALL:
				case DAVIS_CONFIG_MUX_DROP_EXTINPUT_ON_TRANSFER_STALL:
					return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_MUX, paramAddr, param));
					break;

				default:
					return (false);
					break;
			}
			break;

		case DAVIS_CONFIG_DVS:
			switch (paramAddr) {
				case DAVIS_CONFIG_DVS_SIZE_COLUMNS:
				case DAVIS_CONFIG_DVS_SIZE_ROWS:
				case DAVIS_CONFIG_DVS_ORIENTATION_INFO:
				case DAVIS_CONFIG_DVS_RUN:
				case DAVIS_CONFIG_DVS_ACK_DELAY_ROW:
				case DAVIS_CONFIG_DVS_ACK_DELAY_COLUMN:
				case DAVIS_CONFIG_DVS_ACK_EXTENSION_ROW:
				case DAVIS_CONFIG_DVS_ACK_EXTENSION_COLUMN:
				case DAVIS_CONFIG_DVS_WAIT_ON_TRANSFER_STALL:
				case DAVIS_CONFIG_DVS_FILTER_ROW_ONLY_EVENTS:
				case DAVIS_CONFIG_DVS_EXTERNAL_AER_CONTROL:
				case DAVIS_CONFIG_DVS_HAS_PIXEL_FILTER:
				case DAVIS_CONFIG_DVS_HAS_BACKGROUND_ACTIVITY_FILTER:
				case DAVIS_CONFIG_DVS_HAS_TEST_EVENT_GENERATOR:
					return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_DVS, paramAddr, param));
					break;

				case DAVIS_CONFIG_DVS_FILTER_PIXEL_0_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_0_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_1_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_1_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_2_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_2_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_3_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_3_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_4_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_4_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_5_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_5_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_6_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_6_COLUMN:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_7_ROW:
				case DAVIS_CONFIG_DVS_FILTER_PIXEL_7_COLUMN:
					if (handle->info.dvsHasPixelFilter) {
						return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_FILTER_BACKGROUND_ACTIVITY:
				case DAVIS_CONFIG_DVS_FILTER_BACKGROUND_ACTIVITY_DELTAT:
					if (handle->info.dvsHasBackgroundActivityFilter) {
						return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_DVS_TEST_EVENT_GENERATOR_ENABLE:
					if (handle->info.dvsHasTestEventGenerator) {
						return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_DVS, paramAddr, param));
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
				case DAVIS_CONFIG_APS_SIZE_COLUMNS:
				case DAVIS_CONFIG_APS_SIZE_ROWS:
				case DAVIS_CONFIG_APS_ORIENTATION_INFO:
				case DAVIS_CONFIG_APS_COLOR_FILTER:
				case DAVIS_CONFIG_APS_RUN:
				case DAVIS_CONFIG_APS_RESET_READ:
				case DAVIS_CONFIG_APS_WAIT_ON_TRANSFER_STALL:
				case DAVIS_CONFIG_APS_START_COLUMN_0:
				case DAVIS_CONFIG_APS_START_ROW_0:
				case DAVIS_CONFIG_APS_END_COLUMN_0:
				case DAVIS_CONFIG_APS_END_ROW_0:
				case DAVIS_CONFIG_APS_RESET_SETTLE:
				case DAVIS_CONFIG_APS_COLUMN_SETTLE:
				case DAVIS_CONFIG_APS_ROW_SETTLE:
				case DAVIS_CONFIG_APS_NULL_SETTLE:
				case DAVIS_CONFIG_APS_HAS_GLOBAL_SHUTTER:
				case DAVIS_CONFIG_APS_HAS_QUAD_ROI:
				case DAVIS_CONFIG_APS_HAS_EXTERNAL_ADC:
				case DAVIS_CONFIG_APS_HAS_INTERNAL_ADC:
					return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, paramAddr, param));
					break;

				case DAVIS_CONFIG_APS_EXPOSURE:
				case DAVIS_CONFIG_APS_FRAME_DELAY: {
					// Exposure and Frame Delay are in µs, must be converted from native FPGA cycles
					// by dividing with ADC clock value.
					uint32_t cyclesValue = 0;
					if (!spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, paramAddr, &cyclesValue)) {
						return (false);
					}

					*param = cyclesValue / handle->info.adcClock;

					return (true);
					break;
				}

				case DAVIS_CONFIG_APS_GLOBAL_SHUTTER:
					if (handle->info.apsHasGlobalShutter) {
						return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_APS_START_COLUMN_1:
				case DAVIS_CONFIG_APS_START_ROW_1:
				case DAVIS_CONFIG_APS_END_COLUMN_1:
				case DAVIS_CONFIG_APS_END_ROW_1:
				case DAVIS_CONFIG_APS_START_COLUMN_2:
				case DAVIS_CONFIG_APS_START_ROW_2:
				case DAVIS_CONFIG_APS_END_COLUMN_2:
				case DAVIS_CONFIG_APS_END_ROW_2:
				case DAVIS_CONFIG_APS_START_COLUMN_3:
				case DAVIS_CONFIG_APS_START_ROW_3:
				case DAVIS_CONFIG_APS_END_COLUMN_3:
				case DAVIS_CONFIG_APS_END_ROW_3:
					if (handle->info.apsHasQuadROI) {
						return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVIS_CONFIG_APS_USE_INTERNAL_ADC:
				case DAVIS_CONFIG_APS_SAMPLE_ENABLE:
				case DAVIS_CONFIG_APS_SAMPLE_SETTLE:
				case DAVIS_CONFIG_APS_RAMP_RESET:
				case DAVIS_CONFIG_APS_RAMP_SHORT_RESET:
					if (handle->info.apsHasInternalADC) {
						return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DAVISRGB_CONFIG_APS_TRANSFER:
				case DAVISRGB_CONFIG_APS_RSFDSETTLE:
				case DAVISRGB_CONFIG_APS_GSPDRESET:
				case DAVISRGB_CONFIG_APS_GSRESETFALL:
				case DAVISRGB_CONFIG_APS_GSTXFALL:
				case DAVISRGB_CONFIG_APS_GSFDRESET:
					// Support for DAVISRGB extra timing parameters.
					if (IS_RGB(handle->info.chipID)) {
						return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, paramAddr, param));
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

		case DAVIS_CONFIG_IMU:
			switch (paramAddr) {
				case DAVIS_CONFIG_IMU_RUN:
				case DAVIS_CONFIG_IMU_TEMP_STANDBY:
				case DAVIS_CONFIG_IMU_ACCEL_STANDBY:
				case DAVIS_CONFIG_IMU_GYRO_STANDBY:
				case DAVIS_CONFIG_IMU_LP_CYCLE:
				case DAVIS_CONFIG_IMU_LP_WAKEUP:
				case DAVIS_CONFIG_IMU_SAMPLE_RATE_DIVIDER:
				case DAVIS_CONFIG_IMU_DIGITAL_LOW_PASS_FILTER:
				case DAVIS_CONFIG_IMU_ACCEL_FULL_SCALE:
				case DAVIS_CONFIG_IMU_GYRO_FULL_SCALE:
					return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_IMU, paramAddr, param));
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
				case DAVIS_CONFIG_EXTINPUT_HAS_GENERATOR:
					return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_EXTINPUT, paramAddr, param));
					break;

				case DAVIS_CONFIG_EXTINPUT_RUN_GENERATOR:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_USE_CUSTOM_SIGNAL:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_POLARITY:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_INTERVAL:
				case DAVIS_CONFIG_EXTINPUT_GENERATE_PULSE_LENGTH:
					if (handle->info.extInputHasGenerator) {
						return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_EXTINPUT, paramAddr, param));
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
				if (IS_240(handle->info.chipID)) {
					// DAVIS240 uses the old bias generator with 22 branches, and uses all of them.
					if (paramAddr < 22) {
						return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_BIAS, paramAddr, param));
					}
				}
				else if (IS_128(handle->info.chipID) || IS_208(handle->info.chipID)
				|| IS_346(handle->info.chipID) || IS_640(handle->info.chipID)) {
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
							return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_BIAS, paramAddr, param));
							break;

						case DAVIS346_CONFIG_BIAS_ADCTESTVOLTAGE:
							// Only supported by DAVIS346 and DAVIS640 chips.
							if (IS_346(handle->info.chipID) || IS_640(handle->info.chipID)) {
								return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_BIAS, paramAddr, param));
							}
							break;

						case DAVIS208_CONFIG_BIAS_RESETHIGHPASS:
						case DAVIS208_CONFIG_BIAS_REFSS:
						case DAVIS208_CONFIG_BIAS_REGBIASBP:
						case DAVIS208_CONFIG_BIAS_REFSSBN:
							// Only supported by DAVIS208 chips.
							if (IS_208(handle->info.chipID)) {
								return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_BIAS, paramAddr, param));
							}
							break;

						default:
							return (false);
							break;
					}
				}
				else if (IS_RGB(handle->info.chipID)) {
					// DAVISRGB also uses the 37 branches bias generator, with different values.
					switch (paramAddr) {
						case DAVISRGB_CONFIG_BIAS_APSCAS:
						case DAVISRGB_CONFIG_BIAS_OVG1LO:
						case DAVISRGB_CONFIG_BIAS_OVG2LO:
						case DAVISRGB_CONFIG_BIAS_TX2OVG2HI:
						case DAVISRGB_CONFIG_BIAS_GND07:
						case DAVISRGB_CONFIG_BIAS_ADCTESTVOLTAGE:
						case DAVISRGB_CONFIG_BIAS_ADCREFHIGH:
						case DAVISRGB_CONFIG_BIAS_ADCREFLOW:
						case DAVISRGB_CONFIG_BIAS_IFREFRBN:
						case DAVISRGB_CONFIG_BIAS_IFTHRBN:
						case DAVISRGB_CONFIG_BIAS_LOCALBUFBN:
						case DAVISRGB_CONFIG_BIAS_PADFOLLBN:
						case DAVISRGB_CONFIG_BIAS_PIXINVBN:
						case DAVISRGB_CONFIG_BIAS_DIFFBN:
						case DAVISRGB_CONFIG_BIAS_ONBN:
						case DAVISRGB_CONFIG_BIAS_OFFBN:
						case DAVISRGB_CONFIG_BIAS_PRBP:
						case DAVISRGB_CONFIG_BIAS_PRSFBP:
						case DAVISRGB_CONFIG_BIAS_REFRBP:
						case DAVISRGB_CONFIG_BIAS_ARRAYBIASBUFFERBN:
						case DAVISRGB_CONFIG_BIAS_ARRAYLOGICBUFFERBN:
						case DAVISRGB_CONFIG_BIAS_FALLTIMEBN:
						case DAVISRGB_CONFIG_BIAS_RISETIMEBP:
						case DAVISRGB_CONFIG_BIAS_READOUTBUFBP:
						case DAVISRGB_CONFIG_BIAS_APSROSFBN:
						case DAVISRGB_CONFIG_BIAS_ADCCOMPBP:
						case DAVISRGB_CONFIG_BIAS_DACBUFBP:
						case DAVISRGB_CONFIG_BIAS_LCOLTIMEOUTBN:
						case DAVISRGB_CONFIG_BIAS_AEPDBN:
						case DAVISRGB_CONFIG_BIAS_AEPUXBP:
						case DAVISRGB_CONFIG_BIAS_AEPUYBP:
						case DAVISRGB_CONFIG_BIAS_BIASBUFFER:
						case DAVISRGB_CONFIG_BIAS_SSP:
						case DAVISRGB_CONFIG_BIAS_SSN:
							return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_BIAS, paramAddr, param));
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
						return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_CHIP, paramAddr, param));
						break;

					case DAVIS240_CONFIG_CHIP_SPECIALPIXELCONTROL:
						// Only supported by DAVIS240 A/B chips.
						if (IS_240A(handle->info.chipID) || IS_240B(handle->info.chipID)) {
							return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS128_CONFIG_CHIP_GLOBAL_SHUTTER:
						// Only supported by some chips.
						if (handle->info.apsHasGlobalShutter) {
							return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS128_CONFIG_CHIP_SELECTGRAYCOUNTER:
						// Only supported by the new DAVIS chips.
						if (IS_128(handle->info.chipID) || IS_208(handle->info.chipID) || IS_346(handle->info.chipID)
						|| IS_640(handle->info.chipID) || IS_RGB(handle->info.chipID)) {
							return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS346_CONFIG_CHIP_TESTADC:
						// Only supported by some of the new DAVIS chips.
						if (IS_346(handle->info.chipID) || IS_640(handle->info.chipID) || IS_RGB(handle->info.chipID)) {
							return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVISRGB_CONFIG_CHIP_ADJUSTOVG1LO: // Also DAVIS208_CONFIG_CHIP_SELECTPREAMPAVG.
					case DAVISRGB_CONFIG_CHIP_ADJUSTOVG2LO: // Also DAVIS208_CONFIG_CHIP_SELECTBIASREFSS.
					case DAVISRGB_CONFIG_CHIP_ADJUSTTX2OVG2HI: // Also DAVIS208_CONFIG_CHIP_SELECTSENSE.
						// Only supported by DAVIS208 and DAVISRGB.
						if (IS_208(handle->info.chipID) || IS_RGB(handle->info.chipID)) {
							return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_CHIP, paramAddr, param));
						}
						break;

					case DAVIS208_CONFIG_CHIP_SELECTPOSFB:
					case DAVIS208_CONFIG_CHIP_SELECTHIGHPASS:
						// Only supported by DAVIS208.
						if (IS_208(handle->info.chipID)) {
							return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_CHIP, paramAddr, param));
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
			switch (paramAddr) {
				case DAVIS_CONFIG_SYSINFO_LOGIC_VERSION:
				case DAVIS_CONFIG_SYSINFO_CHIP_IDENTIFIER:
				case DAVIS_CONFIG_SYSINFO_DEVICE_IS_MASTER:
				case DAVIS_CONFIG_SYSINFO_LOGIC_CLOCK:
				case DAVIS_CONFIG_SYSINFO_ADC_CLOCK:
					return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_SYSINFO, paramAddr, param));
					break;

				default:
					return (false);
					break;
			}
			break;

		case DAVIS_CONFIG_USB:
			switch (paramAddr) {
				case DAVIS_CONFIG_USB_RUN:
				case DAVIS_CONFIG_USB_EARLY_PACKET_DELAY:
					return (spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_USB, paramAddr, param));
					break;

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

bool davisCommonDataStart(caerDeviceHandle cdh, void (*dataNotifyIncrease)(void *ptr),
	void (*dataNotifyDecrease)(void *ptr), void *dataNotifyUserPtr) {
	davisHandle handle = (davisHandle) cdh;
	davisState state = &handle->state;

	// Store new data available/not available anymore call-backs.
	state->dataNotifyIncrease = dataNotifyIncrease;
	state->dataNotifyDecrease = dataNotifyDecrease;
	state->dataNotifyUserPtr = dataNotifyUserPtr;

	// Initialize RingBuffer.
	state->dataExchangeBuffer = ringBufferInit(atomic_load(&state->dataExchangeBufferSize));
	if (state->dataExchangeBuffer == NULL) {
		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to initialize data exchange buffer.");
		return (false);
	}

	// Allocate packets.
	state->currentPacketContainer = caerEventPacketContainerAllocate(DAVIS_EVENT_TYPES);
	if (state->currentPacketContainer == NULL) {
		freeAllDataMemory(state);

		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate event packet container.");
		return (false);
	}

	state->currentPolarityPacket = caerPolarityEventPacketAllocate(I32T(atomic_load(&state->maxPolarityPacketSize)),
		I16T(handle->info.deviceID), 0);
	if (state->currentPolarityPacket == NULL) {
		freeAllDataMemory(state);

		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate polarity event packet.");
		return (false);
	}

	state->currentSpecialPacket = caerSpecialEventPacketAllocate(I32T(atomic_load(&state->maxSpecialPacketSize)),
		I16T(handle->info.deviceID), 0);
	if (state->currentSpecialPacket == NULL) {
		freeAllDataMemory(state);

		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate special event packet.");
		return (false);
	}

	state->currentFramePacket = caerFrameEventPacketAllocate(I32T(atomic_load(&state->maxFramePacketSize)),
		I16T(handle->info.deviceID), 0);
	if (state->currentFramePacket == NULL) {
		freeAllDataMemory(state);

		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate frame event packet.");
		return (false);
	}

	state->currentIMU6Packet = caerIMU6EventPacketAllocate(I32T(atomic_load(&state->maxIMU6PacketSize)),
		I16T(handle->info.deviceID), 0);
	if (state->currentIMU6Packet == NULL) {
		freeAllDataMemory(state);

		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate IMU6 event packet.");
		return (false);
	}

	state->apsCurrentResetFrame = calloc((size_t) state->apsSizeX * state->apsSizeY * 1, sizeof(uint16_t));
	if (state->currentIMU6Packet == NULL) {
		freeAllDataMemory(state);

		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate IMU6 event packet.");
		return (false);
	}

	// Default IMU settings (for event parsing).
	uint32_t param32 = 0;

	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_ACCEL_FULL_SCALE, &param32);
	state->imuAccelScale = calculateIMUAccelScale(U8T(param32));
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_GYRO_FULL_SCALE, &param32);
	state->imuGyroScale = calculateIMUGyroScale(U8T(param32));

	// Default APS settings (for event parsing).
	uint32_t param32start = 0;

	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_END_COLUMN_0, &param32);
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_START_COLUMN_0, &param32start);
	state->apsWindow0SizeX = U16T(param32 + 1 - param32start);
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_END_ROW_0, &param32);
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_START_ROW_0, &param32start);
	state->apsWindow0SizeY = U16T(param32 + 1 - param32start);
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_GLOBAL_SHUTTER, &param32);
	state->apsGlobalShutter = param32;
	spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_RESET_READ, &param32);
	state->apsResetRead = param32;

	// Start data acquisition thread.
	atomic_store(&state->dataAcquisitionThreadRun, true);

	if ((errno = pthread_create(&state->dataAcquisitionThread, NULL, &davisDataAcquisitionThread, handle)) != 0) {
		freeAllDataMemory(state);

		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to start data acquisition thread. Error: %d.",
		errno);
		return (false);
	}

	return (true);
}

bool davisCommonDataStop(caerDeviceHandle cdh) {
	davisHandle handle = (davisHandle) cdh;
	davisState state = &handle->state;

	// Stop data acquisition thread.
	atomic_store(&state->dataAcquisitionThreadRun, false);

	// Wait for data acquisition thread to terminate...
	if ((errno = pthread_join(state->dataAcquisitionThread, NULL)) != 0) {
		// This should never happen!
		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to join data acquisition thread. Error: %d.",
		errno);
		return (false);
	}

	// Empty ringbuffer.
	caerEventPacketContainer container;
	while ((container = ringBufferGet(state->dataExchangeBuffer)) != NULL) {
		// Notify data-not-available call-back.
		state->dataNotifyDecrease(state->dataNotifyUserPtr);

		// Free container, which will free its subordinate packets too.
		caerEventPacketContainerFree(container);
	}

	// Free current, uncommitted packets and ringbuffer.
	freeAllDataMemory(state);

	// Reset packet positions.
	state->currentPolarityPacketPosition = 0;
	state->currentSpecialPacketPosition = 0;
	state->currentFramePacketPosition = 0;
	state->currentIMU6PacketPosition = 0;

	// Reset private composite events.
	memset(&state->currentFrameEvent, 0, sizeof(struct caer_frame_event));
	memset(&state->currentIMU6Event, 0, sizeof(struct caer_imu6_event));

	return (true);
}

caerEventPacketContainer davisCommonDataGet(caerDeviceHandle cdh) {
	davisHandle handle = (davisHandle) cdh;
	davisState state = &handle->state;
	caerEventPacketContainer container = NULL;

	retry: container = ringBufferGet(state->dataExchangeBuffer);

	if (container != NULL) {
		// Found an event container, return it and signal this piece of data
		// is no longer available for later acquisition.
		state->dataNotifyDecrease(state->dataNotifyUserPtr);

		return (container);
	}

	// Didn't find any event container, either report this or retry, depending
	// on blocking setting.
	if (atomic_load(&state->dataExchangeBlocking)) {
		goto retry;
	}

	// Nothing.
	return (NULL);
}

static bool spiConfigSend(libusb_device_handle *devHandle, uint8_t moduleAddr, uint8_t paramAddr, uint32_t param) {
	uint8_t spiConfig[4] = { 0 };

	spiConfig[0] = U8T(param >> 24);
	spiConfig[1] = U8T(param >> 16);
	spiConfig[2] = U8T(param >> 8);
	spiConfig[3] = U8T(param >> 0);

	return (libusb_control_transfer(devHandle,
		LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
		VENDOR_REQUEST_FPGA_CONFIG, moduleAddr, paramAddr, spiConfig, sizeof(spiConfig), 0) == LIBUSB_SUCCESS);
}

static bool spiConfigReceive(libusb_device_handle *devHandle, uint8_t moduleAddr, uint8_t paramAddr, uint32_t *param) {
	uint8_t spiConfig[4] = { 0 };

	if (libusb_control_transfer(devHandle, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
	VENDOR_REQUEST_FPGA_CONFIG, moduleAddr, paramAddr, spiConfig, sizeof(spiConfig), 0) != LIBUSB_SUCCESS) {
		return (false);
	}

	*param = 0;
	*param |= U32T(spiConfig[0] << 24);
	*param |= U32T(spiConfig[1] << 16);
	*param |= U32T(spiConfig[2] << 8);
	*param |= U32T(spiConfig[3] << 0);

	return (true);
}

static libusb_device_handle *davisDeviceOpen(libusb_context *devContext, uint16_t devVID, uint16_t devPID,
	uint8_t devType, uint8_t busNumber, uint8_t devAddress, const char *serialNumber, uint16_t requiredLogicRevision,
	uint16_t requiredFirmwareVersion) {
	libusb_device_handle *devHandle = NULL;
	libusb_device **devicesList;

	ssize_t result = libusb_get_device_list(devContext, &devicesList);

	if (result >= 0) {
		// Cycle thorough all discovered devices and find a match.
		for (size_t i = 0; i < (size_t) result; i++) {
			struct libusb_device_descriptor devDesc;

			if (libusb_get_device_descriptor(devicesList[i], &devDesc) != LIBUSB_SUCCESS) {
				continue;
			}

			// Check if this is the device we want (VID/PID).
			if (devDesc.idVendor == devVID && devDesc.idProduct == devPID
				&& U8T((devDesc.bcdDevice & 0xFF00) >> 8) == devType
				&& U8T(devDesc.bcdDevice & 0x00FF) >= requiredFirmwareVersion) {
				// If a USB port restriction is given, honor it.
				if (busNumber > 0 && libusb_get_bus_number(devicesList[i]) != busNumber) {
					continue;
				}

				if (devAddress > 0 && libusb_get_device_address(devicesList[i]) != devAddress) {
					continue;
				}

				if (libusb_open(devicesList[i], &devHandle) != LIBUSB_SUCCESS) {
					devHandle = NULL;

					continue;
				}

				// Check the serial number restriction, if any is present.
				if (serialNumber != NULL && !caerStrEquals(serialNumber, "")) {
					char deviceSerialNumber[8 + 1] = { 0 };
					int getStringDescResult = libusb_get_string_descriptor_ascii(devHandle, devDesc.iSerialNumber,
						(unsigned char *) deviceSerialNumber, 8);

					// Check serial number success and length.
					if (getStringDescResult < 0 || getStringDescResult > 8) {
						libusb_close(devHandle);
						devHandle = NULL;

						continue;
					}

					// Now check if the Serial Number matches.
					if (!caerStrEquals(serialNumber, deviceSerialNumber)) {
						libusb_close(devHandle);
						devHandle = NULL;

						continue;
					}
				}

				// Check that the active configuration is set to number 1. If not, do so.
				int activeConfiguration;
				if (libusb_get_configuration(devHandle, &activeConfiguration) != LIBUSB_SUCCESS) {
					libusb_close(devHandle);
					devHandle = NULL;

					continue;
				}

				if (activeConfiguration != 1) {
					if (libusb_set_configuration(devHandle, 1) != LIBUSB_SUCCESS) {
						libusb_close(devHandle);
						devHandle = NULL;

						continue;
					}
				}

				// Claim interface 0 (default).
				if (libusb_claim_interface(devHandle, 0) != LIBUSB_SUCCESS) {
					libusb_close(devHandle);
					devHandle = NULL;

					continue;
				}

				// Communication with device open, get logic version information.
				uint32_t param32 = 0;

				spiConfigReceive(devHandle, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_LOGIC_VERSION, &param32);
				uint16_t logicVersion = U16T(param32);

				// Verify device logic version.
				if (logicVersion < requiredLogicRevision) {
					libusb_release_interface(devHandle, 0);
					libusb_close(devHandle);
					devHandle = NULL;

					continue;
				}

				// Found and configured it!
				break;
			}
		}

		libusb_free_device_list(devicesList, true);
	}

	return (devHandle);
}

static void davisDeviceClose(libusb_device_handle *devHandle) {
	// Release interface 0 (default).
	libusb_release_interface(devHandle, 0);

	libusb_close(devHandle);
}

static void davisAllocateTransfers(davisHandle handle, uint32_t bufferNum, uint32_t bufferSize) {
	davisState state = &handle->state;

	// Set number of transfers and allocate memory for the main transfer array.
	state->dataTransfers = calloc(bufferNum, sizeof(struct libusb_transfer *));
	if (state->dataTransfers == NULL) {
		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString,
			"Failed to allocate memory for %" PRIu32 " libusb transfers. Error: %d.", bufferNum, errno);
		return;
	}
	state->dataTransfersLength = bufferNum;

	// Allocate transfers and set them up.
	for (size_t i = 0; i < bufferNum; i++) {
		state->dataTransfers[i] = libusb_alloc_transfer(0);
		if (state->dataTransfers[i] == NULL) {
			caerLog(CAER_LOG_CRITICAL, handle->info.deviceString,
				"Unable to allocate further libusb transfers (%zu of %" PRIu32 ").", i, bufferNum);
			continue;
		}

		// Create data buffer.
		state->dataTransfers[i]->length = (int) bufferSize;
		state->dataTransfers[i]->buffer = malloc(bufferSize);
		if (state->dataTransfers[i]->buffer == NULL) {
			caerLog(CAER_LOG_CRITICAL, handle->info.deviceString,
				"Unable to allocate buffer for libusb transfer %zu. Error: %d.", i, errno);

			libusb_free_transfer(state->dataTransfers[i]);
			state->dataTransfers[i] = NULL;

			continue;
		}

		// Initialize Transfer.
		state->dataTransfers[i]->dev_handle = state->deviceHandle;
		state->dataTransfers[i]->endpoint = DAVIS_DATA_ENDPOINT;
		state->dataTransfers[i]->type = LIBUSB_TRANSFER_TYPE_BULK;
		state->dataTransfers[i]->callback = &davisLibUsbCallback;
		state->dataTransfers[i]->user_data = handle;
		state->dataTransfers[i]->timeout = 0;
		state->dataTransfers[i]->flags = LIBUSB_TRANSFER_FREE_BUFFER;

		if ((errno = libusb_submit_transfer(state->dataTransfers[i])) == LIBUSB_SUCCESS) {
			state->activeDataTransfers++;
		}
		else {
			caerLog(CAER_LOG_CRITICAL, handle->info.deviceString,
				"Unable to submit libusb transfer %zu. Error: %s (%d).", i, libusb_strerror(errno), errno);

			// The transfer buffer is freed automatically here thanks to
			// the LIBUSB_TRANSFER_FREE_BUFFER flag set above.
			libusb_free_transfer(state->dataTransfers[i]);
			state->dataTransfers[i] = NULL;

			continue;
		}
	}

	if (state->activeDataTransfers == 0) {
		// Didn't manage to allocate any USB transfers, free array memory and log failure.
		free(state->dataTransfers);
		state->dataTransfers = NULL;
		state->dataTransfersLength = 0;

		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Unable to allocate any libusb transfers.");
	}
}

static void davisDeallocateTransfers(davisHandle handle) {
	davisState state = &handle->state;

	// Cancel all current transfers first.
	for (size_t i = 0; i < state->dataTransfersLength; i++) {
		if (state->dataTransfers[i] != NULL) {
			errno = libusb_cancel_transfer(state->dataTransfers[i]);
			if (errno != LIBUSB_SUCCESS && errno != LIBUSB_ERROR_NOT_FOUND) {
				caerLog(CAER_LOG_CRITICAL, handle->info.deviceString,
					"Unable to cancel libusb transfer %zu. Error: %s (%d).", i, libusb_strerror(errno), errno);
				// Proceed with trying to cancel all transfers regardless of errors.
			}
		}
	}

	// Wait for all transfers to go away (0.1 seconds timeout).
	struct timeval te = { .tv_sec = 0, .tv_usec = 100000 };

	while (state->activeDataTransfers > 0) {
		libusb_handle_events_timeout(state->deviceContext, &te);
	}

	// The buffers and transfers have been deallocated in the callback.
	// Only the transfers array remains, which we free here.
	free(state->dataTransfers);
	state->dataTransfers = NULL;
	state->dataTransfersLength = 0;
}

static void LIBUSB_CALL davisLibUsbCallback(struct libusb_transfer *transfer) {
	davisHandle handle = transfer->user_data;
	davisState state = &handle->state;

	if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
		// Handle data.
		davisEventTranslator(handle, transfer->buffer, (size_t) transfer->actual_length);
	}

	if (transfer->status != LIBUSB_TRANSFER_CANCELLED && transfer->status != LIBUSB_TRANSFER_NO_DEVICE) {
		// Submit transfer again.
		if (libusb_submit_transfer(transfer) == LIBUSB_SUCCESS) {
			return;
		}
	}

	// Cannot recover (cancelled, no device, or other critical error).
	// Signal this by adjusting the counter, free and exit.
	state->activeDataTransfers--;
	for (size_t i = 0; i < state->dataTransfersLength; i++) {
		// Remove from list, so we don't try to cancel it later on.
		if (state->dataTransfers[i] == transfer) {
			state->dataTransfers[i] = NULL;
		}
	}
	libusb_free_transfer(transfer);
}

#define TS_WRAP_ADD 0x8000

static void davisEventTranslator(davisHandle handle, uint8_t *buffer, size_t bytesSent) {
	davisState state = &handle->state;

	// Truncate off any extra partial event.
	if ((bytesSent & 0x01) != 0) {
		caerLog(CAER_LOG_ALERT, handle->info.deviceString,
			"%zu bytes received via USB, which is not a multiple of two.", bytesSent);
		bytesSent &= (size_t) ~0x01;
	}

	for (size_t i = 0; i < bytesSent; i += 2) {
		// Allocate new packets for next iteration as needed.
		if (state->currentPacketContainer == NULL) {
			state->currentPacketContainer = caerEventPacketContainerAllocate(DAVIS_EVENT_TYPES);
			if (state->currentPacketContainer == NULL) {
				caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate event packet container.");
				return;
			}
		}

		if (state->currentPolarityPacket == NULL) {
			state->currentPolarityPacket = caerPolarityEventPacketAllocate(
				I32T(atomic_load(&state->maxPolarityPacketSize)), I16T(handle->info.deviceID), state->wrapOverflow);
			if (state->currentPolarityPacket == NULL) {
				caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate polarity event packet.");
				return;
			}
		}

		if (state->currentSpecialPacket == NULL) {
			state->currentSpecialPacket = caerSpecialEventPacketAllocate(
				I32T(atomic_load(&state->maxSpecialPacketSize)), I16T(handle->info.deviceID), state->wrapOverflow);
			if (state->currentSpecialPacket == NULL) {
				caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate special event packet.");
				return;
			}
		}

		if (state->currentFramePacket == NULL) {
			state->currentFramePacket = caerFrameEventPacketAllocate(I32T(atomic_load(&state->maxFramePacketSize)),
				I16T(handle->info.deviceID), state->wrapOverflow);
			if (state->currentFramePacket == NULL) {
				caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate frame event packet.");
				return;
			}
		}

		if (state->currentIMU6Packet == NULL) {
			state->currentIMU6Packet = caerIMU6EventPacketAllocate(I32T(atomic_load(&state->maxIMU6PacketSize)),
				I16T(handle->info.deviceID), state->wrapOverflow);
			if (state->currentIMU6Packet == NULL) {
				caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate IMU6 event packet.");
				return;
			}
		}

		bool forceCommit = false;

		uint16_t event = le16toh(*((uint16_t *) (&buffer[i])));

		// Check if timestamp.
		if ((event & 0x8000) != 0) {
			// Is a timestamp! Expand to 32 bits. (Tick is 1µs already.)
			state->lastTimestamp = state->currentTimestamp;
			state->currentTimestamp = state->wrapAdd + (event & 0x7FFF);

			// Check monotonicity of timestamps.
			checkStrictMonotonicTimestamp(handle);
		}
		else {
			// Get all current events, so we don't have to duplicate code in every branch.
			caerPolarityEvent currentPolarityEvent = caerPolarityEventPacketGetEvent(state->currentPolarityPacket,
				state->currentPolarityPacketPosition);
			caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(state->currentSpecialPacket,
				state->currentSpecialPacketPosition);

			// Look at the code, to determine event and data type.
			uint8_t code = U8T((event & 0x7000) >> 12);
			uint16_t data = (event & 0x0FFF);

			switch (code) {
				case 0: // Special event
					switch (data) {
						case 0: // Ignore this, but log it.
							caerLog(CAER_LOG_ERROR, handle->info.deviceString, "Caught special reserved event!");
							break;

						case 1: { // Timetamp reset
							state->wrapOverflow = 0;
							state->wrapAdd = 0;
							state->lastTimestamp = 0;
							state->currentTimestamp = 0;
							state->dvsTimestamp = 0;

							caerLog(CAER_LOG_INFO, handle->info.deviceString, "Timestamp reset event received.");

							// Create timestamp reset event.
							caerSpecialEventSetTimestamp(currentSpecialEvent, INT32_MAX);
							caerSpecialEventSetType(currentSpecialEvent, TIMESTAMP_RESET);
							caerSpecialEventValidate(currentSpecialEvent, state->currentSpecialPacket);
							state->currentSpecialPacketPosition++;

							// Commit packets when doing a reset to clearly separate them.
							forceCommit = true;

							// Update Master/Slave status on incoming TS resets. Done in main thread
							// to avoid deadlock inside callback.
							atomic_fetch_or(&state->dataAcquisitionThreadConfigUpdate, 1 << 1);

							break;
						}

						case 2: { // External input (falling edge)
							caerLog(CAER_LOG_DEBUG, handle->info.deviceString,
								"External input (falling edge) event received.");

							caerSpecialEventSetTimestamp(currentSpecialEvent, state->currentTimestamp);
							caerSpecialEventSetType(currentSpecialEvent, EXTERNAL_INPUT_FALLING_EDGE);
							caerSpecialEventValidate(currentSpecialEvent, state->currentSpecialPacket);
							state->currentSpecialPacketPosition++;
							break;
						}

						case 3: { // External input (rising edge)
							caerLog(CAER_LOG_DEBUG, handle->info.deviceString,
								"External input (rising edge) event received.");

							caerSpecialEventSetTimestamp(currentSpecialEvent, state->currentTimestamp);
							caerSpecialEventSetType(currentSpecialEvent, EXTERNAL_INPUT_RISING_EDGE);
							caerSpecialEventValidate(currentSpecialEvent, state->currentSpecialPacket);
							state->currentSpecialPacketPosition++;
							break;
						}

						case 4: { // External input (pulse)
							caerLog(CAER_LOG_DEBUG, handle->info.deviceString,
								"External input (pulse) event received.");

							caerSpecialEventSetTimestamp(currentSpecialEvent, state->currentTimestamp);
							caerSpecialEventSetType(currentSpecialEvent, EXTERNAL_INPUT_PULSE);
							caerSpecialEventValidate(currentSpecialEvent, state->currentSpecialPacket);
							state->currentSpecialPacketPosition++;
							break;
						}

						case 5: { // IMU Start (6 axes)
							caerLog(CAER_LOG_DEBUG, handle->info.deviceString, "IMU6 Start event received.");

							state->imuIgnoreEvents = false;
							state->imuCount = 0;

							memset(&state->currentIMU6Event, 0, sizeof(struct caer_imu6_event));

							caerIMU6EventSetTimestamp(&state->currentIMU6Event, state->currentTimestamp);
							break;
						}

						case 7: // IMU End
							caerLog(CAER_LOG_DEBUG, handle->info.deviceString, "IMU End event received.");
							if (state->imuIgnoreEvents) {
								break;
							}

							if (state->imuCount == IMU6_COUNT) {
								caerIMU6EventValidate(&state->currentIMU6Event, state->currentIMU6Packet);

								caerIMU6Event currentIMU6Event = caerIMU6EventPacketGetEvent(state->currentIMU6Packet,
									state->currentIMU6PacketPosition);
								memcpy(currentIMU6Event, &state->currentIMU6Event, sizeof(struct caer_imu6_event));
								state->currentIMU6PacketPosition++;
							}
							else {
								caerLog(CAER_LOG_INFO, handle->info.deviceString,
									"IMU End: failed to validate IMU sample count (%" PRIu8 "), discarding samples.",
									state->imuCount);
							}
							break;

						case 8: { // APS Global Shutter Frame Start
							caerLog(CAER_LOG_DEBUG, handle->info.deviceString, "APS GS Frame Start event received.");
							state->apsIgnoreEvents = false;
							state->apsGlobalShutter = true;
							state->apsResetRead = true;

							initFrame(handle);

							break;
						}

						case 9: { // APS Rolling Shutter Frame Start
							caerLog(CAER_LOG_DEBUG, handle->info.deviceString, "APS RS Frame Start event received.");
							state->apsIgnoreEvents = false;
							state->apsGlobalShutter = false;
							state->apsResetRead = true;

							initFrame(handle);

							break;
						}

						case 10: { // APS Frame End
							caerLog(CAER_LOG_DEBUG, handle->info.deviceString, "APS Frame End event received.");
							if (state->apsIgnoreEvents) {
								break;
							}

							bool validFrame = true;

							for (size_t j = 0; j < APS_READOUT_TYPES_NUM; j++) {
								int32_t checkValue = caerFrameEventGetLengthX(&state->currentFrameEvent);

								// Check main reset read against zero if disabled.
								if (j == APS_READOUT_RESET && !state->apsResetRead) {
									checkValue = 0;
								}

								caerLog(CAER_LOG_DEBUG, handle->info.deviceString, "APS Frame End: CountX[%zu] is %d.",
									j, state->apsCountX[j]);

								if (state->apsCountX[j] != checkValue) {
									caerLog(CAER_LOG_ERROR, handle->info.deviceString,
										"APS Frame End: wrong column count [%zu - %d] detected.", j,
										state->apsCountX[j]);
									validFrame = false;
								}
							}

							// Write out end of frame timestamp.
							caerFrameEventSetTSEndOfFrame(&state->currentFrameEvent, state->currentTimestamp);

							// Validate event and advance frame packet position.
							if (validFrame) {
								caerFrameEventValidate(&state->currentFrameEvent, state->currentFramePacket);

								caerFrameEvent currentFrameEvent = caerFrameEventPacketGetEvent(
									state->currentFramePacket, state->currentFramePacketPosition);
								memcpy(currentFrameEvent, &state->currentFrameEvent, sizeof(struct caer_frame_event));
								state->currentFramePacketPosition++;

								// Ensure pixel array for current frame is freed together with the packet,
								// and not with the device state.
								state->currentFrameEvent.pixels = NULL;
							}
							else {
								// Avoid memory leak on failure.
								free(state->currentFrameEvent.pixels);
								state->currentFrameEvent.pixels = NULL;
							}

							break;
						}

						case 11: { // APS Reset Column Start
							caerLog(CAER_LOG_DEBUG, handle->info.deviceString,
								"APS Reset Column Start event received.");
							if (state->apsIgnoreEvents) {
								break;
							}

							state->apsCurrentReadoutType = APS_READOUT_RESET;
							state->apsCountY[state->apsCurrentReadoutType] = 0;

							state->apsRGBPixelOffsetDirection = 0;
							state->apsRGBPixelOffset = 1; // RGB support, first pixel of row always even.

							// The first Reset Column Read Start is also the start
							// of the exposure for the RS.
							if (!state->apsGlobalShutter && state->apsCountX[APS_READOUT_RESET] == 0) {
								caerFrameEventSetTSStartOfExposure(&state->currentFrameEvent, state->currentTimestamp);
							}

							break;
						}

						case 12: { // APS Signal Column Start
							caerLog(CAER_LOG_DEBUG, handle->info.deviceString,
								"APS Signal Column Start event received.");
							if (state->apsIgnoreEvents) {
								break;
							}

							state->apsCurrentReadoutType = APS_READOUT_SIGNAL;
							state->apsCountY[state->apsCurrentReadoutType] = 0;

							state->apsRGBPixelOffsetDirection = 0;
							state->apsRGBPixelOffset = 1; // RGB support, first pixel of row always even.

							// The first Signal Column Read Start is also always the end
							// of the exposure time, for both RS and GS.
							if (state->apsCountX[APS_READOUT_SIGNAL] == 0) {
								caerFrameEventSetTSEndOfExposure(&state->currentFrameEvent, state->currentTimestamp);
							}

							break;
						}

						case 13: { // APS Column End
							caerLog(CAER_LOG_DEBUG, handle->info.deviceString, "APS Column End event received.");
							if (state->apsIgnoreEvents) {
								break;
							}

							caerLog(CAER_LOG_DEBUG, handle->info.deviceString, "APS Column End: CountX[%d] is %d.",
								state->apsCurrentReadoutType, state->apsCountX[state->apsCurrentReadoutType]);
							caerLog(CAER_LOG_DEBUG, handle->info.deviceString, "APS Column End: CountY[%d] is %d.",
								state->apsCurrentReadoutType, state->apsCountY[state->apsCurrentReadoutType]);

							if (state->apsCountY[state->apsCurrentReadoutType]
								!= caerFrameEventGetLengthY(&state->currentFrameEvent)) {
								caerLog(CAER_LOG_ERROR, handle->info.deviceString,
									"APS Column End: wrong row count [%d - %d] detected.", state->apsCurrentReadoutType,
									state->apsCountY[state->apsCurrentReadoutType]);
							}

							state->apsCountX[state->apsCurrentReadoutType]++;

							// The last Reset Column Read End is also the start
							// of the exposure for the GS.
							if (state->apsGlobalShutter && state->apsCurrentReadoutType == APS_READOUT_RESET
								&& state->apsCountX[APS_READOUT_RESET]
									== caerFrameEventGetLengthX(&state->currentFrameEvent)) {
								caerFrameEventSetTSStartOfExposure(&state->currentFrameEvent, state->currentTimestamp);
							}

							break;
						}

						case 14: { // APS Global Shutter Frame Start with no Reset Read
							caerLog(CAER_LOG_DEBUG, handle->info.deviceString,
								"APS GS NORST Frame Start event received.");
							state->apsIgnoreEvents = false;
							state->apsGlobalShutter = true;
							state->apsResetRead = false;

							initFrame(handle);

							// If reset reads are disabled, the start of exposure is closest to
							// the start of frame.
							caerFrameEventSetTSStartOfExposure(&state->currentFrameEvent, state->currentTimestamp);

							break;
						}

						case 15: { // APS Rolling Shutter Frame Start with no Reset Read
							caerLog(CAER_LOG_DEBUG, handle->info.deviceString,
								"APS RS NORST Frame Start event received.");
							state->apsIgnoreEvents = false;
							state->apsGlobalShutter = false;
							state->apsResetRead = false;

							initFrame(handle);

							// If reset reads are disabled, the start of exposure is closest to
							// the start of frame.
							caerFrameEventSetTSStartOfExposure(&state->currentFrameEvent, state->currentTimestamp);

							break;
						}

						case 16:
						case 17:
						case 18:
						case 19:
						case 20:
						case 21:
						case 22:
						case 23:
						case 24:
						case 25:
						case 26:
						case 27:
						case 28:
						case 29:
						case 30:
						case 31: {
							caerLog(CAER_LOG_DEBUG, handle->info.deviceString,
								"IMU Scale Config event (%" PRIu16 ") received.", data);
							if (state->imuIgnoreEvents) {
								break;
							}

							// Set correct IMU accel and gyro scales, used to interpret subsequent
							// IMU samples from the device.
							state->imuAccelScale = calculateIMUAccelScale((data >> 2) & 0x03);
							state->imuGyroScale = calculateIMUGyroScale(data & 0x03);

							// At this point the IMU event count should be zero (reset by start).
							if (state->imuCount != 0) {
								caerLog(CAER_LOG_INFO, handle->info.deviceString,
									"IMU Scale Config: previous IMU start event missed, attempting recovery.");
							}

							// Increase IMU count by one, to a total of one (0+1=1).
							// This way we can recover from the above error of missing start, and we can
							// later discover if the IMU Scale Config event actually arrived itself.
							state->imuCount = 1;

							break;
						}

						default:
							caerLog(CAER_LOG_ERROR, handle->info.deviceString,
								"Caught special event that can't be handled: %d.", data);
							break;
					}
					break;

				case 1: // Y address
					// Check range conformity.
					if (data >= state->dvsSizeY) {
						caerLog(CAER_LOG_ALERT, handle->info.deviceString,
							"DVS: Y address out of range (0-%d): %" PRIu16 ".", state->dvsSizeY - 1, data);
						break; // Skip invalid Y address (don't update lastY).
					}

					if (state->dvsGotY) {
						// Use the previous timestamp here, since this refers to the previous Y.
						caerSpecialEventSetTimestamp(currentSpecialEvent, state->dvsTimestamp);
						caerSpecialEventSetType(currentSpecialEvent, DVS_ROW_ONLY);
						caerSpecialEventSetData(currentSpecialEvent, state->dvsLastY);
						caerSpecialEventValidate(currentSpecialEvent, state->currentSpecialPacket);
						state->currentSpecialPacketPosition++;

						caerLog(CAER_LOG_DEBUG, handle->info.deviceString,
							"DVS: row-only event received for address Y=%" PRIu16 ".", state->dvsLastY);
					}

					state->dvsLastY = data;
					state->dvsGotY = true;
					state->dvsTimestamp = state->currentTimestamp;

					break;

				case 2: // X address, Polarity OFF
				case 3: { // X address, Polarity ON
					// Check range conformity.
					if (data >= state->dvsSizeX) {
						caerLog(CAER_LOG_ALERT, handle->info.deviceString,
							"DVS: X address out of range (0-%d): %" PRIu16 ".", state->dvsSizeX - 1, data);
						break; // Skip invalid event.
					}

					// Invert polarity for PixelParade high gain pixels (DavisSense), because of
					// negative gain from pre-amplifier.
					uint8_t polarity = ((IS_208(handle->info.chipID)) && (data < 192)) ? U8T(~code) : (code);

					caerPolarityEventSetTimestamp(currentPolarityEvent, state->dvsTimestamp);
					caerPolarityEventSetPolarity(currentPolarityEvent, (polarity & 0x01));
					if (state->dvsInvertXY) {
						caerPolarityEventSetY(currentPolarityEvent, data);
						caerPolarityEventSetX(currentPolarityEvent, state->dvsLastY);
					}
					else {
						caerPolarityEventSetY(currentPolarityEvent, state->dvsLastY);
						caerPolarityEventSetX(currentPolarityEvent, data);
					}
					caerPolarityEventValidate(currentPolarityEvent, state->currentPolarityPacket);
					state->currentPolarityPacketPosition++;

					state->dvsGotY = false;

					break;
				}

				case 4: {
					if (state->apsIgnoreEvents) {
						break;
					}

					// Let's check that apsCountY is not above the maximum. This could happen
					// if start/end of column events are discarded (no wait on transfer stall).
					if (state->apsCountY[state->apsCurrentReadoutType]
						>= caerFrameEventGetLengthY(&state->currentFrameEvent)) {
						caerLog(CAER_LOG_DEBUG, handle->info.deviceString,
							"APS ADC sample: row count is at maximum, discarding further samples.");
						break;
					}

					// If reset read, we store the values in a local array. If signal read, we
					// store the final pixel value directly in the output frame event. We already
					// do the subtraction between reset and signal here, to avoid carrying that
					// around all the time and consuming memory. This way we can also only take
					// infrequent reset reads and re-use them for multiple frames, which can heavily
					// reduce traffic, and should not impact image quality heavily, at least in GS.
					uint16_t xPos =
						(state->apsFlipX) ?
							(U16T(
								caerFrameEventGetLengthX(&state->currentFrameEvent) - 1
									- state->apsCountX[state->apsCurrentReadoutType])) :
							(U16T(state->apsCountX[state->apsCurrentReadoutType]));
					uint16_t yPos =
						(state->apsFlipY) ?
							(U16T(
								caerFrameEventGetLengthY(&state->currentFrameEvent) - 1
									- state->apsCountY[state->apsCurrentReadoutType])) :
							(U16T(state->apsCountY[state->apsCurrentReadoutType]));

					if (IS_RGB(handle->info.chipID)) {
						yPos = U16T(yPos + state->apsRGBPixelOffset);
					}

					if (state->apsInvertXY) {
						SWAP_VAR(uint16_t, xPos, yPos);
					}

					size_t pixelPosition = (size_t) (yPos * caerFrameEventGetLengthX(&state->currentFrameEvent)) + xPos;

					if ((state->apsCurrentReadoutType == APS_READOUT_RESET
						&& !(IS_RGB(handle->info.chipID) && state->apsGlobalShutter))
						|| (state->apsCurrentReadoutType == APS_READOUT_SIGNAL
							&& (IS_RGB(handle->info.chipID) && state->apsGlobalShutter))) {
						state->apsCurrentResetFrame[pixelPosition] = data;
					}
					else {
						int32_t pixelValue = 0;

						if (IS_RGB(handle->info.chipID) && state->apsGlobalShutter) {
							// DAVIS RGB GS has inverted samples, signal read comes first
							// and was stored above inside state->apsCurrentResetFrame.
							pixelValue = (data - state->apsCurrentResetFrame[pixelPosition]);
						}
						else {
							pixelValue = (state->apsCurrentResetFrame[pixelPosition] - data);
						}

						// Normalize the ADC value to 16bit generic depth and check for underflow.
						pixelValue = (pixelValue < 0) ? (0) : (pixelValue);
						pixelValue = pixelValue << (16 - APS_ADC_DEPTH);

						caerFrameEventGetPixelArrayUnsafe(&state->currentFrameEvent)[pixelPosition] = htole16(
							U16T(pixelValue));
					}

					caerLog(CAER_LOG_DEBUG, handle->info.deviceString,
						"APS ADC Sample: column=%" PRIu16 ", row=%" PRIu16 ", xPos=%" PRIu16 ", yPos=%" PRIu16 ", data=%" PRIu16 ".",
						state->apsCountX[state->apsCurrentReadoutType], state->apsCountY[state->apsCurrentReadoutType],
						xPos, yPos, data);

					state->apsCountY[state->apsCurrentReadoutType]++;

					// RGB support: first 320 pixels are even, then odd.
					if (IS_RGB(handle->info.chipID)) {
						if (state->apsRGBPixelOffsetDirection == 0) { // Increasing
							state->apsRGBPixelOffset++;

							if (state->apsRGBPixelOffset == 321) {
								// Switch to decreasing after last even pixel.
								state->apsRGBPixelOffsetDirection = 1;
								state->apsRGBPixelOffset = 318;
							}
						}
						else { // Decreasing
							state->apsRGBPixelOffset = I16T(state->apsRGBPixelOffset - 3);
						}
					}

					break;
				}

				case 5: {
					// Misc 8bit data, used currently only
					// for IMU events in DAVIS FX3 boards.
					uint8_t misc8Code = U8T((data & 0x0F00) >> 8);
					uint8_t misc8Data = U8T(data & 0x00FF);

					switch (misc8Code) {
						case 0:
							if (state->imuIgnoreEvents) {
								break;
							}

							// Detect missing IMU end events.
							if (state->imuCount >= IMU6_COUNT) {
								caerLog(CAER_LOG_INFO, handle->info.deviceString,
									"IMU data: IMU samples count is at maximum, discarding further samples.");
								break;
							}

							// IMU data event.
							switch (state->imuCount) {
								case 0:
									caerLog(CAER_LOG_ERROR, handle->info.deviceString,
										"IMU data: missing IMU Scale Config event. Parsing of IMU events will still be attempted, but be aware that Accel/Gyro scale conversions may be inaccurate.");
									state->imuCount = 1;
									// Fall through to next case, as if imuCount was equal to 1.

								case 1:
								case 3:
								case 5:
								case 7:
								case 9:
								case 11:
								case 13:
									state->imuTmpData = misc8Data;
									break;

								case 2: {
									int16_t accelX = I16T((state->imuTmpData << 8) | misc8Data);
									caerIMU6EventSetAccelX(&state->currentIMU6Event, accelX / state->imuAccelScale);
									break;
								}

								case 4: {
									int16_t accelY = I16T((state->imuTmpData << 8) | misc8Data);
									caerIMU6EventSetAccelY(&state->currentIMU6Event, accelY / state->imuAccelScale);
									break;
								}

								case 6: {
									int16_t accelZ = I16T((state->imuTmpData << 8) | misc8Data);
									caerIMU6EventSetAccelZ(&state->currentIMU6Event, accelZ / state->imuAccelScale);
									break;
								}

									// Temperature is signed. Formula for converting to °C:
									// (SIGNED_VAL / 340) + 36.53
								case 8: {
									int16_t temp = I16T((state->imuTmpData << 8) | misc8Data);
									caerIMU6EventSetTemp(&state->currentIMU6Event, (temp / 340.0f) + 36.53f);
									break;
								}

								case 10: {
									int16_t gyroX = I16T((state->imuTmpData << 8) | misc8Data);
									caerIMU6EventSetGyroX(&state->currentIMU6Event, gyroX / state->imuGyroScale);
									break;
								}

								case 12: {
									int16_t gyroY = I16T((state->imuTmpData << 8) | misc8Data);
									caerIMU6EventSetGyroY(&state->currentIMU6Event, gyroY / state->imuGyroScale);
									break;
								}

								case 14: {
									int16_t gyroZ = I16T((state->imuTmpData << 8) | misc8Data);
									caerIMU6EventSetGyroZ(&state->currentIMU6Event, gyroZ / state->imuGyroScale);
									break;
								}
							}

							state->imuCount++;

							break;

						default:
							caerLog(CAER_LOG_ERROR, handle->info.deviceString,
								"Caught Misc8 event that can't be handled.");
							break;
					}

					break;
				}

				case 7: { // Timestamp wrap
					// Detect big timestamp wrap-around.
					int64_t wrapJump = (TS_WRAP_ADD * data);
					int64_t wrapSum = I64T(state->wrapAdd) + wrapJump;

					if (wrapSum > I64T(INT32_MAX)) {
						// Reset wrapAdd at this point, so we can again
						// start detecting overruns of the 32bit value.
						// We reset not to zero, but to the remaining value after
						// multiple wrap-jumps are taken into account.
						int64_t wrapRemainder = wrapSum - I64T(INT32_MAX) - 1LL;
						state->wrapAdd = I32T(wrapRemainder);

						state->lastTimestamp = 0;
						state->currentTimestamp = state->wrapAdd;
						state->dvsTimestamp = 0; // Closest to previous value for column addresses.

						// Increment TSOverflow counter.
						state->wrapOverflow++;

						caerSpecialEventSetTimestamp(currentSpecialEvent, INT32_MAX);
						caerSpecialEventSetType(currentSpecialEvent, TIMESTAMP_WRAP);
						caerSpecialEventValidate(currentSpecialEvent, state->currentSpecialPacket);
						state->currentSpecialPacketPosition++;

						// Commit packets to separate before wrap from after cleanly.
						forceCommit = true;
					}
					else {
						// Each wrap is 2^15 µs (~32ms), and we have
						// to multiply it with the wrap counter,
						// which is located in the data part of this
						// event.
						state->wrapAdd = I32T(wrapSum);

						state->lastTimestamp = state->currentTimestamp;
						state->currentTimestamp = state->wrapAdd;

						// Check monotonicity of timestamps.
						checkStrictMonotonicTimestamp(handle);

						caerLog(CAER_LOG_DEBUG, handle->info.deviceString,
							"Timestamp wrap event received with multiplier of %" PRIu16 ".", data);
					}

					break;
				}

				default:
					caerLog(CAER_LOG_ERROR, handle->info.deviceString, "Caught event that can't be handled.");
					break;
			}
		}

		// Thresholds on which to trigger packet container commit.
		// forceCommit is already defined above.
		int32_t polaritySize = state->currentPolarityPacketPosition;
		int32_t polarityInterval =
			(polaritySize > 1) ?
				(caerPolarityEventGetTimestamp(
					caerPolarityEventPacketGetEvent(state->currentPolarityPacket, polaritySize - 1))
					- caerPolarityEventGetTimestamp(caerPolarityEventPacketGetEvent(state->currentPolarityPacket, 0))) :
				(0);

		int32_t specialSize = state->currentSpecialPacketPosition;
		int32_t specialInterval =
			(specialSize > 1) ?
				(caerSpecialEventGetTimestamp(
					caerSpecialEventPacketGetEvent(state->currentSpecialPacket, specialSize - 1))
					- caerSpecialEventGetTimestamp(caerSpecialEventPacketGetEvent(state->currentSpecialPacket, 0))) :
				(0);

		int32_t frameSize = state->currentFramePacketPosition;
		int32_t frameInterval =
			(frameSize > 1) ?
				(caerFrameEventGetTSStartOfExposure(
					caerFrameEventPacketGetEvent(state->currentFramePacket, frameSize - 1))
					- caerFrameEventGetTSStartOfExposure(caerFrameEventPacketGetEvent(state->currentFramePacket, 0))) :
				(0);

		int32_t imu6Size = state->currentIMU6PacketPosition;
		int32_t imu6Interval =
			(imu6Size > 1) ?
				(caerIMU6EventGetTimestamp(caerIMU6EventPacketGetEvent(state->currentIMU6Packet, imu6Size - 1))
					- caerIMU6EventGetTimestamp(caerIMU6EventPacketGetEvent(state->currentIMU6Packet, 0))) :
				(0);

		// Trigger if any of the global container-wide thresholds are met.
		bool containerCommit = (((polaritySize + specialSize + frameSize + imu6Size)
			>= atomic_load(&state->maxPacketContainerSize))
			|| (polarityInterval >= atomic_load(&state->maxPacketContainerInterval))
			|| (specialInterval >= atomic_load(&state->maxPacketContainerInterval))
			|| (frameInterval >= atomic_load(&state->maxPacketContainerInterval))
			|| (imu6Interval >= atomic_load(&state->maxPacketContainerInterval)));

		// Trigger if any of the packet-specific thresholds are met.
		bool polarityPacketCommit = ((polaritySize
			>= caerEventPacketHeaderGetEventCapacity(&state->currentPolarityPacket->packetHeader))
			|| (polarityInterval >= atomic_load(&state->maxPolarityPacketInterval)));

		// Trigger if any of the packet-specific thresholds are met.
		bool specialPacketCommit = ((specialSize
			>= caerEventPacketHeaderGetEventCapacity(&state->currentSpecialPacket->packetHeader))
			|| (specialInterval >= atomic_load(&state->maxSpecialPacketInterval)));

		// Trigger if any of the packet-specific thresholds are met.
		bool framePacketCommit = ((frameSize
			>= caerEventPacketHeaderGetEventCapacity(&state->currentFramePacket->packetHeader))
			|| (frameInterval >= atomic_load(&state->maxFramePacketInterval)));

		// Trigger if any of the packet-specific thresholds are met.
		bool imu6PacketCommit = ((imu6Size
			>= caerEventPacketHeaderGetEventCapacity(&state->currentIMU6Packet->packetHeader))
			|| (imu6Interval >= atomic_load(&state->maxIMU6PacketInterval)));

		// Commit packet containers to the ring-buffer, so they can be processed by the
		// main-loop, when any of the required conditions are met.
		if (forceCommit || containerCommit || polarityPacketCommit || specialPacketCommit || framePacketCommit
			|| imu6PacketCommit) {
			// One or more of the commit triggers are hit. Set the packet container up to contain
			// any non-empty packets. Empty packets are not forwarded to save memory.
			if (polaritySize > 0) {
				caerEventPacketContainerSetEventPacket(state->currentPacketContainer, POLARITY_EVENT,
					(caerEventPacketHeader) state->currentPolarityPacket);

				state->currentPolarityPacket = NULL;
				state->currentPolarityPacketPosition = 0;
			}

			if (specialSize > 0) {
				caerEventPacketContainerSetEventPacket(state->currentPacketContainer, SPECIAL_EVENT,
					(caerEventPacketHeader) state->currentSpecialPacket);

				state->currentSpecialPacket = NULL;
				state->currentSpecialPacketPosition = 0;
			}

			if (frameSize > 0) {
				caerEventPacketContainerSetEventPacket(state->currentPacketContainer, FRAME_EVENT,
					(caerEventPacketHeader) state->currentFramePacket);

				state->currentFramePacket = NULL;
				state->currentFramePacketPosition = 0;
			}

			if (imu6Size > 0) {
				caerEventPacketContainerSetEventPacket(state->currentPacketContainer, IMU6_EVENT,
					(caerEventPacketHeader) state->currentIMU6Packet);

				state->currentIMU6Packet = NULL;
				state->currentIMU6PacketPosition = 0;
			}

			if (forceCommit) {
				// Ignore all APS and IMU6 (composite) events, until a new APS or IMU6
				// Start event comes in, for the next packet.
				// This is to correctly support the forced packet commits that a TS reset,
				// or a TS big wrap, impose. Continuing to parse events would result
				// in a corrupted state of the first event in the new packet, as it would
				// be incomplete, incorrect and miss vital initialization data.
				state->apsIgnoreEvents = true;
				state->imuIgnoreEvents = true;
			}

			retry_important: if (!ringBufferPut(state->dataExchangeBuffer, state->currentPacketContainer)) {
				// Failed to forward packet container, drop it, unless it contains a timestamp
				// related change, those are critical, so we just spin until we can
				// deliver that one. (Easily detected by forceCommit!)
				if (forceCommit) {
					goto retry_important;
				}
				else {
					// Failed to forward packet container, just drop it, it doesn't contain
					// any critical information anyway.
					caerLog(CAER_LOG_INFO, handle->info.deviceString,
						"Dropped EventPacket Container because ring-buffer full!");

					// Re-use the event-packet container to avoid having to reallocate it.
					// The contained event packets do have to be dropped first!
					caerEventPacketFree(
						caerEventPacketContainerGetEventPacket(state->currentPacketContainer, POLARITY_EVENT));
					caerEventPacketFree(
						caerEventPacketContainerGetEventPacket(state->currentPacketContainer, SPECIAL_EVENT));
					caerEventPacketFree(
						caerEventPacketContainerGetEventPacket(state->currentPacketContainer, FRAME_EVENT));
					caerEventPacketFree(
						caerEventPacketContainerGetEventPacket(state->currentPacketContainer, IMU6_EVENT));

					caerEventPacketContainerSetEventPacket(state->currentPacketContainer, POLARITY_EVENT, NULL);
					caerEventPacketContainerSetEventPacket(state->currentPacketContainer, SPECIAL_EVENT, NULL);
					caerEventPacketContainerSetEventPacket(state->currentPacketContainer, FRAME_EVENT, NULL);
					caerEventPacketContainerSetEventPacket(state->currentPacketContainer, IMU6_EVENT, NULL);
				}
			}
			else {
				state->dataNotifyIncrease(state->dataNotifyUserPtr);

				state->currentPacketContainer = NULL;
			}
		}
	}
}

static void *davisDataAcquisitionThread(void *inPtr) {
	// inPtr is a pointer to device handle.
	davisHandle handle = inPtr;
	davisState state = &handle->state;

	caerLog(CAER_LOG_DEBUG, handle->info.deviceString, "Initializing data acquisition thread ...");

	// Reset configuration update, so as to not re-do work afterwards.
	atomic_store(&state->dataAcquisitionThreadConfigUpdate, 0);

	// Enable data transfer on USB end-point 2.
	davisCommonConfigSet(handle, DAVIS_CONFIG_USB, DAVIS_CONFIG_USB_RUN, true);
	davisCommonConfigSet(handle, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_RUN, true);
	davisCommonConfigSet(handle, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_TIMESTAMP_RUN, true);
	davisCommonConfigSet(handle, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_RUN, true);
	davisCommonConfigSet(handle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_RUN, true);
	davisCommonConfigSet(handle, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_RUN, true);
	davisCommonConfigSet(handle, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_RUN_DETECTOR, true);

	// Create buffers as specified in config file.
	davisAllocateTransfers(handle, U32T(atomic_load(&state->usbBufferNumber)),
		U32T(atomic_load(&state->usbBufferSize)));

	// Handle USB events (1 second timeout).
	struct timeval te = { .tv_sec = 0, .tv_usec = 1000000 };

	caerLog(CAER_LOG_DEBUG, handle->info.deviceString, "data acquisition thread ready to process events.");

	while (atomic_load(&state->dataAcquisitionThreadRun) != 0 && state->activeDataTransfers > 0) {
		// Check config refresh, in this case to adjust buffer sizes.
		if (atomic_load(&state->dataAcquisitionThreadConfigUpdate) != 0) {
			davisDataAcquisitionThreadConfig(handle);
		}

		libusb_handle_events_timeout(state->deviceContext, &te);
	}

	caerLog(CAER_LOG_DEBUG, handle->info.deviceString, "shutting down data acquisition thread ...");

	// Disable data transfer on USB end-point 2. Reverse order of enabling above.
	// TODO: check if this really needs to be in close() due to libusb_handle_events_timeout() not returning under high load.
	davisCommonConfigSet(handle, DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_RUN_DETECTOR, false);
	davisCommonConfigSet(handle, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_RUN, false);
	davisCommonConfigSet(handle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_RUN, false);
	davisCommonConfigSet(handle, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_RUN, false);
	davisCommonConfigSet(handle, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_FORCE_CHIP_BIAS_ENABLE, false); // Ensure chip turns off.
	davisCommonConfigSet(handle, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_TIMESTAMP_RUN, false); // Turn off timestamping too.
	davisCommonConfigSet(handle, DAVIS_CONFIG_MUX, DAVIS_CONFIG_MUX_RUN, false);
	davisCommonConfigSet(handle, DAVIS_CONFIG_USB, DAVIS_CONFIG_USB_RUN, false);

	// Cancel all transfers and handle them.
	davisDeallocateTransfers(handle);

	caerLog(CAER_LOG_DEBUG, handle->info.deviceString, "data acquisition thread shut down.");

	return (NULL);
}

static void davisDataAcquisitionThreadConfig(davisHandle handle) {
	davisState state = &handle->state;

	// Get the current value to examine by atomic exchange, since we don't
	// want there to be any possible store between a load/store pair.
	uint32_t configUpdate = U32T(atomic_exchange(&state->dataAcquisitionThreadConfigUpdate, 0));

	if ((configUpdate >> 0) & 0x01) {
		// Do buffer size change: cancel all and recreate them.
		davisDeallocateTransfers(handle);
		davisAllocateTransfers(handle, U32T(atomic_load(&state->usbBufferNumber)),
			U32T(atomic_load(&state->usbBufferSize)));
	}

	if ((configUpdate >> 1) & 0x01) {
		// Get new Master/Slave information from device. Done here to prevent deadlock
		// inside asynchronous callback.
		uint32_t param32 = 0;

		spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_DEVICE_IS_MASTER, &param32);
		handle->info.deviceIsMaster = param32;
	}
}
