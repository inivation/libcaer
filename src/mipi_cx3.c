#include "mipi_cx3.h"

static void mipiCx3Log(enum caer_log_level logLevel, mipiCx3Handle handle, const char *format, ...) ATTRIBUTE_FORMAT(3);
static void mipiCx3EventTranslator(void *vhd, const uint8_t *buffer, size_t bytesSent);

static bool i2cConfigSend(usbState state, uint16_t deviceAddr, uint16_t byteAddr, uint8_t param);
static bool i2cConfigReceive(usbState state, uint16_t deviceAddr, uint16_t byteAddr, uint8_t *param);

static void mipiCx3Log(enum caer_log_level logLevel, mipiCx3Handle handle, const char *format, ...) {
	va_list argumentList;
	va_start(argumentList, format);
	caerLogVAFull(atomic_load_explicit(&handle->state.deviceLogLevel, memory_order_relaxed), logLevel,
		handle->info.deviceString, format, argumentList);
	va_end(argumentList);
}

ssize_t mipiCx3Find(caerDeviceDiscoveryResult *discoveredDevices) {
	// Set to NULL initially (for error return).
	*discoveredDevices = NULL;

	struct usb_info *foundMipiCx3 = NULL;

	ssize_t result = usbDeviceFind(MIPI_CX3_DEVICE_VID, MIPI_CX3_DEVICE_PID, -1, -1, -1, &foundMipiCx3);

	if (result <= 0) {
		// Error or nothing found, return right away.
		return (result);
	}

	// Allocate memory for discovered devices in expected format.
	*discoveredDevices = calloc((size_t) result, sizeof(struct caer_device_discovery_result));
	if (*discoveredDevices == NULL) {
		free(foundMipiCx3);
		return (-1);
	}

	// Transform from generic USB format into device discovery one.
	caerLogDisable(true);
	for (size_t i = 0; i < (size_t) result; i++) {
		// This is a MIPI_CX3.
		(*discoveredDevices)[i].deviceType         = CAER_DEVICE_MIPI_CX3;
		(*discoveredDevices)[i].deviceErrorOpen    = foundMipiCx3[i].errorOpen;
		(*discoveredDevices)[i].deviceErrorVersion = foundMipiCx3[i].errorVersion;
		struct caer_mipi_cx3_info *mipiCx3InfoPtr  = &((*discoveredDevices)[i].deviceInfo.mipiCx3Info);

		mipiCx3InfoPtr->deviceUSBBusNumber     = foundMipiCx3[i].busNumber;
		mipiCx3InfoPtr->deviceUSBDeviceAddress = foundMipiCx3[i].devAddress;
		strncpy(mipiCx3InfoPtr->deviceSerialNumber, foundMipiCx3[i].serialNumber, MAX_SERIAL_NUMBER_LENGTH + 1);

		// Reopen MIPI_CX3 device to get additional info, if possible at all.
		if (!foundMipiCx3[i].errorOpen && !foundMipiCx3[i].errorVersion) {
			caerDeviceHandle dvs
				= mipiCx3Open(0, mipiCx3InfoPtr->deviceUSBBusNumber, mipiCx3InfoPtr->deviceUSBDeviceAddress, NULL);
			if (dvs != NULL) {
				*mipiCx3InfoPtr = caerMipiCx3InfoGet(dvs);

				mipiCx3Close(dvs);
			}
		}

		// Set/Reset to invalid values, not part of discovery.
		mipiCx3InfoPtr->deviceID     = -1;
		mipiCx3InfoPtr->deviceString = NULL;
	}
	caerLogDisable(false);

	free(foundMipiCx3);
	return (result);
}

static inline void freeAllDataMemory(mipiCx3State state) {
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

	containerGenerationDestroy(&state->container);
}

caerDeviceHandle mipiCx3Open(
	uint16_t deviceID, uint8_t busNumberRestrict, uint8_t devAddressRestrict, const char *serialNumberRestrict) {
	errno = 0;

	caerLog(CAER_LOG_DEBUG, __func__, "Initializing %s.", MIPI_CX3_DEVICE_NAME);

	mipiCx3Handle handle = calloc(1, sizeof(*handle));
	if (handle == NULL) {
		// Failed to allocate memory for device handle!
		caerLog(CAER_LOG_CRITICAL, __func__, "Failed to allocate memory for device handle.");
		errno = CAER_ERROR_MEMORY_ALLOCATION;
		return (NULL);
	}

	// Set main deviceType correctly right away.
	handle->deviceType = CAER_DEVICE_MIPI_CX3;

	mipiCx3State state = &handle->state;

	// Initialize state variables to default values (if not zero, taken care of by calloc above).
	dataExchangeSettingsInit(&state->dataExchange);

	// Packet settings (size (in events) and time interval (in µs)).
	containerGenerationSettingsInit(&state->container);

	// Logging settings (initialize to global log-level).
	enum caer_log_level globalLogLevel = caerLogLevelGet();
	atomic_store(&state->deviceLogLevel, globalLogLevel);
	usbSetLogLevel(&state->usbState, globalLogLevel);

	// Set device thread name. Maximum length of 15 chars due to Linux limitations.
	char usbThreadName[MAX_THREAD_NAME_LENGTH + 1];
	snprintf(usbThreadName, MAX_THREAD_NAME_LENGTH + 1, "%s %" PRIu16, MIPI_CX3_DEVICE_NAME, deviceID);
	usbThreadName[MAX_THREAD_NAME_LENGTH] = '\0';

	usbSetThreadName(&state->usbState, usbThreadName);
	handle->info.deviceString = usbThreadName; // Temporary, until replaced by full string.

	// Try to open a MIPI_CX3 device on a specific USB port.
	struct usb_info usbInfo;

	if (!usbDeviceOpen(&state->usbState, MIPI_CX3_DEVICE_VID, MIPI_CX3_DEVICE_PID, busNumberRestrict,
			devAddressRestrict, serialNumberRestrict, -1, -1, -1, &usbInfo)) {
		if (errno == CAER_ERROR_OPEN_ACCESS) {
			mipiCx3Log(
				CAER_LOG_CRITICAL, handle, "Failed to open device, no matching device could be found or opened.");
		}
		else {
			mipiCx3Log(CAER_LOG_CRITICAL, handle,
				"Failed to open device, see above log message for more information (errno=%d).", errno);
		}

		free(handle);

		// errno set by usbDeviceOpen().
		return (NULL);
	}

	char *usbInfoString = usbGenerateDeviceString(usbInfo, MIPI_CX3_DEVICE_NAME, deviceID);
	if (usbInfoString == NULL) {
		mipiCx3Log(CAER_LOG_CRITICAL, handle, "Failed to generate USB information string.");

		usbDeviceClose(&state->usbState);
		free(handle);

		errno = CAER_ERROR_MEMORY_ALLOCATION;
		return (NULL);
	}

	// Setup USB.
	usbSetDataCallback(&state->usbState, &mipiCx3EventTranslator, handle);
	usbSetDataEndpoint(&state->usbState, MIPI_CX3_DATA_ENDPOINT);
	usbSetTransfersNumber(&state->usbState, 8);
	usbSetTransfersSize(&state->usbState, 8192);

	// Start USB handling thread.
	if (!usbThreadStart(&state->usbState)) {
		usbDeviceClose(&state->usbState);
		free(usbInfoString);
		free(handle);

		errno = CAER_ERROR_COMMUNICATION;
		return (NULL);
	}

	handle->info.deviceID = I16T(deviceID);
	strncpy(handle->info.deviceSerialNumber, usbInfo.serialNumber, MAX_SERIAL_NUMBER_LENGTH + 1);
	handle->info.deviceUSBBusNumber     = usbInfo.busNumber;
	handle->info.deviceUSBDeviceAddress = usbInfo.devAddress;
	handle->info.deviceString           = usbInfoString;

	// Get USB firmware version.
	uint8_t firmwareVersion = 0;
	i2cConfigReceive(&state->usbState, DEVICE_FPGA, 0xFF00, &firmwareVersion);

	handle->info.firmwareVersion = firmwareVersion;
	handle->info.chipID          = MIPI_CX3_CHIP_ID;
	handle->info.dvsSizeX        = 640;
	handle->info.dvsSizeY        = 480;

	// Send initialization commands.
	usbControlTransferOut(&state->usbState, VENDOR_REQUEST_RESET, 0, 0, NULL, 0); // Reset FPGA.
	usbControlTransferOut(&state->usbState, VENDOR_REQUEST_RESET, 1, 0, NULL, 0); // Reset FX3 FIFO SM.

	// Wait 10ms for FPGA / FX3 to reset.
	struct timespec resetSleep = {.tv_sec = 0, .tv_nsec = 10000000};
	thrd_sleep(&resetSleep, NULL);

	// FPGA settings.
	i2cConfigSend(&state->usbState, DEVICE_FPGA, 0x020C, 0x3F); // MI2C
	i2cConfigSend(&state->usbState, DEVICE_FPGA, 0x020D, 0x04); // MI2C
	i2cConfigSend(&state->usbState, DEVICE_FPGA, 0x0200, 0x00); // Turn all IMU features off.

	i2cConfigSend(&state->usbState, DEVICE_FPGA, 0x0000, 0x11); // Big endian transfer, enable FX3 transfer.
	i2cConfigSend(&state->usbState, DEVICE_FPGA, 0x0004, 0x01); // Take DVS out of reset.

	// Wait 10ms for DVS to start.
	struct timespec dvsSleep = {.tv_sec = 0, .tv_nsec = 10000000};
	thrd_sleep(&dvsSleep, NULL);

	// Bias reset.
	i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_OTP_TRIM, 0x24);

	// Bias enable.
	i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_PINS_DBGP, 0x07);
	i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_PINS_DBGN, 0xFF);
	i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_PINS_BUFP, 0x03);
	i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_PINS_BUFN, 0x7F);
	i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_PINS_DOB, 0x00);

	mipiCx3ConfigSet(
		(caerDeviceHandle) handle, MIPI_CX3_DVS_BIAS, MIPI_CX3_DVS_BIAS_SIMPLE, MIPI_CX3_DVS_BIAS_SIMPLE_DEFAULT);

	// System settings.
	i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_CONTROL_CLOCK_DIVIDER_SYS, 0xA0); // Divide freq by 10.
	i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_CONTROL_PARALLEL_OUT_CONTROL, 0x00);
	i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_CONTROL_PARALLEL_OUT_ENABLE, 0x01);
	i2cConfigSend(
		&state->usbState, DEVICE_DVS, REGISTER_CONTROL_PACKET_FORMAT, 0x00); // TODO: 0x80 to enable MGROUP compression.

	// Digital settings.
	i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_TIMESTAMP_SUBUNIT, 0x31);
	i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_MODE_CONTROL, 0x0C); // R/AY signals enable.
	i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_BOOT_SEQUENCE, 0x08);

	// Fine clock counts based on clock frequency.
	i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GH_COUNT_FINE, 50);
	i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GRS_COUNT_FINE, 50);
	i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GRS_END_FINE, 50);

	// Disable histogram, not currently used/mapped.
	i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_SPATIAL_HISTOGRAM_OFF, 0x01);

	// Commands in firmware but not documented, unused.
	// i2cConfigSend(&state->usbState, DEVICE_DVS, 0x3043, 0x01); // Bypass ESP.
	// i2cConfigSend(&state->usbState, DEVICE_DVS, 0x3249, 0x00);
	// i2cConfigSend(&state->usbState, DEVICE_DVS, 0x324A, 0x01);
	// i2cConfigSend(&state->usbState, DEVICE_DVS, 0x325A, 0x00);
	// i2cConfigSend(&state->usbState, DEVICE_DVS, 0x325B, 0x01);

	mipiCx3Log(CAER_LOG_DEBUG, handle, "Initialized device successfully with USB Bus=%" PRIu8 ":Addr=%" PRIu8 ".",
		usbInfo.busNumber, usbInfo.devAddress);

	return ((caerDeviceHandle) handle);
}

bool mipiCx3Close(caerDeviceHandle cdh) {
	mipiCx3Handle handle = (mipiCx3Handle) cdh;
	mipiCx3State state   = &handle->state;

	mipiCx3Log(CAER_LOG_DEBUG, handle, "Shutting down ...");

	i2cConfigSend(&state->usbState, DEVICE_FPGA, 0x0004, 0x00); // Put DVS in reset.
	i2cConfigSend(&state->usbState, DEVICE_FPGA, 0x0000, 0x10); // Disable FX3 transfer.

	// Shut down USB handling thread.
	usbThreadStop(&state->usbState);

	// Finally, close the device fully.
	usbDeviceClose(&state->usbState);

	mipiCx3Log(CAER_LOG_DEBUG, handle, "Shutdown successful.");

	// Free memory.
	free(handle->info.deviceString);
	free(handle);

	return (true);
}

struct caer_mipi_cx3_info caerMipiCx3InfoGet(caerDeviceHandle cdh) {
	mipiCx3Handle handle = (mipiCx3Handle) cdh;

	// Check if the pointer is valid.
	if (handle == NULL) {
		struct caer_mipi_cx3_info emptyInfo = {0, .deviceString = NULL};
		return (emptyInfo);
	}

	// Check if device type is supported.
	if (handle->deviceType != CAER_DEVICE_MIPI_CX3) {
		struct caer_mipi_cx3_info emptyInfo = {0, .deviceString = NULL};
		return (emptyInfo);
	}

	// Return a copy of the device information.
	return (handle->info);
}

bool mipiCx3SendDefaultConfig(caerDeviceHandle cdh) {
	mipiCx3Handle handle = (mipiCx3Handle) cdh;

	// Set default biases.
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS_BIAS, MIPI_CX3_DVS_BIAS_SIMPLE, MIPI_CX3_DVS_BIAS_SIMPLE_DEFAULT);

	// External trigger.
	mipiCx3ConfigSet(
		cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_EXTERNAL_TRIGGER_MODE, MIPI_CX3_DVS_EXTERNAL_TRIGGER_MODE_TIMESTAMP_RESET);

	// Digital readout configuration.
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_GLOBAL_HOLD_ENABLE, true);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_GLOBAL_RESET_ENABLE, false);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_GLOBAL_RESET_DURING_READOUT, false);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_FIXED_READ_TIME_ENABLE, false);

	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_EVENT_FLATTEN, false);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_EVENT_ON_ONLY, false);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_EVENT_OFF_ONLY, false);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_SUBSAMPLE_ENABLE, false);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_ENABLE, false);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_DUAL_BINNING_ENABLE, false);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_SUBSAMPLE_VERTICAL, MIPI_CX3_DVS_SUBSAMPLE_VERTICAL_NONE);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_SUBSAMPLE_HORIZONTAL, MIPI_CX3_DVS_SUBSAMPLE_HORIZONTAL_NONE);

	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_0, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_1, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_2, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_3, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_4, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_5, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_6, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_7, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_8, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_9, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_10, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_11, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_12, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_13, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_14, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_15, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_16, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_17, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_18, 0x7FFF);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_AREA_BLOCKING_19, 0x7FFF);

	// Timing settings.
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_TIMING_ED, 2);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_TIMING_GH2GRS, 0);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_TIMING_GRS, 1);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_TIMING_GH2SEL, 4);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_TIMING_SELW, 6);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_TIMING_SEL2AY_R, 4);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_TIMING_SEL2AY_F, 6);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_TIMING_SEL2R_R, 8);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_TIMING_SEL2R_F, 10);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_TIMING_NEXT_SEL, 15);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_TIMING_NEXT_GH, 10);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_TIMING_READ_FIXED, 48000);

	// Crop block.
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS_CROPPER, MIPI_CX3_DVS_CROPPER_ENABLE, false);

	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS_CROPPER, MIPI_CX3_DVS_CROPPER_X_START_ADDRESS, 0);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS_CROPPER, MIPI_CX3_DVS_CROPPER_Y_START_ADDRESS, 0);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS_CROPPER, MIPI_CX3_DVS_CROPPER_X_END_ADDRESS, U32T(handle->info.dvsSizeX - 1));
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS_CROPPER, MIPI_CX3_DVS_CROPPER_Y_END_ADDRESS, U32T(handle->info.dvsSizeY - 1));

	// Activity decision block.
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS_ACTIVITY_DECISION, MIPI_CX3_DVS_ACTIVITY_DECISION_ENABLE, false);

	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS_ACTIVITY_DECISION, MIPI_CX3_DVS_ACTIVITY_DECISION_POS_THRESHOLD, 300);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS_ACTIVITY_DECISION, MIPI_CX3_DVS_ACTIVITY_DECISION_NEG_THRESHOLD, 20);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS_ACTIVITY_DECISION, MIPI_CX3_DVS_ACTIVITY_DECISION_DEC_RATE, 1);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS_ACTIVITY_DECISION, MIPI_CX3_DVS_ACTIVITY_DECISION_DEC_TIME, 3);
	mipiCx3ConfigSet(cdh, MIPI_CX3_DVS_ACTIVITY_DECISION, MIPI_CX3_DVS_ACTIVITY_DECISION_POS_MAX_COUNT, 300);

	// DTAG restart after config.
	i2cConfigSend(&handle->state.usbState, DEVICE_DVS, REGISTER_DIGITAL_RESTART, 0x02);

	return (true);
}

bool mipiCx3ConfigSet(caerDeviceHandle cdh, int8_t modAddr, uint8_t paramAddr, uint32_t param) {
	mipiCx3Handle handle = (mipiCx3Handle) cdh;
	mipiCx3State state   = &handle->state;

	switch (modAddr) {
		case CAER_HOST_CONFIG_USB:
			return (usbConfigSet(&state->usbState, U8T(paramAddr), param));
			break;

		case CAER_HOST_CONFIG_DATAEXCHANGE:
			return (dataExchangeConfigSet(&state->dataExchange, U8T(paramAddr), param));
			break;

		case CAER_HOST_CONFIG_PACKETS:
			return (containerGenerationConfigSet(&state->container, U8T(paramAddr), param));
			break;

		case CAER_HOST_CONFIG_LOG:
			switch (paramAddr) {
				case CAER_HOST_CONFIG_LOG_LEVEL:
					atomic_store(&state->deviceLogLevel, U8T(param));

					// Set USB log-level to this value too.
					usbSetLogLevel(&state->usbState, param);
					break;

				default:
					return (false);
					break;
			}
			break;

		case MIPI_CX3_DVS:
			switch (paramAddr) {
				case MIPI_CX3_DVS_MODE: {
					if (param >= 3) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_CONTROL_MODE, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_EVENT_FLATTEN: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_CONTROL_PACKET_FORMAT, &currVal)) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_CONTROL_PACKET_FORMAT,
						(param) ? U8T(currVal | 0x40) : U8T(currVal & ~0x40)));
					break;
				}

				case MIPI_CX3_DVS_EVENT_ON_ONLY: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_CONTROL_PACKET_FORMAT, &currVal)) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_CONTROL_PACKET_FORMAT,
						(param) ? U8T(currVal | 0x20) : U8T(currVal & ~0x20)));
					break;
				}

				case MIPI_CX3_DVS_EVENT_OFF_ONLY: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_CONTROL_PACKET_FORMAT, &currVal)) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_CONTROL_PACKET_FORMAT,
						(param) ? U8T(currVal | 0x10) : U8T(currVal & ~0x10)));
					break;
				}

				case MIPI_CX3_DVS_SUBSAMPLE_ENABLE: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_ENABLE, &currVal)) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_ENABLE,
						(param) ? U8T(currVal & ~0x04) : U8T(currVal | 0x04)));
					break;
				}

				case MIPI_CX3_DVS_AREA_BLOCKING_ENABLE: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_ENABLE, &currVal)) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_ENABLE,
						(param) ? U8T(currVal & ~0x02) : U8T(currVal | 0x02)));
					break;
				}

				case MIPI_CX3_DVS_DUAL_BINNING_ENABLE: {
					return (i2cConfigSend(
						&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_DUAL_BINNING, (param) ? (0x01) : (0x00)));
					break;
				}

				case MIPI_CX3_DVS_SUBSAMPLE_VERTICAL: {
					if (param >= 8) {
						return (false);
					}

					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_SUBSAMPLE_RATIO, &currVal)) {
						return (false);
					}

					currVal = U8T(U8T(currVal) & ~0x38) | U8T(param << 3);

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_SUBSAMPLE_RATIO, currVal));
					break;
				}

				case MIPI_CX3_DVS_SUBSAMPLE_HORIZONTAL: {
					if (param >= 8) {
						return (false);
					}

					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_SUBSAMPLE_RATIO, &currVal)) {
						return (false);
					}

					currVal = U8T(U8T(currVal) & ~0x07) | U8T(param);

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_SUBSAMPLE_RATIO, currVal));
					break;
				}

				case MIPI_CX3_DVS_AREA_BLOCKING_0:
				case MIPI_CX3_DVS_AREA_BLOCKING_1:
				case MIPI_CX3_DVS_AREA_BLOCKING_2:
				case MIPI_CX3_DVS_AREA_BLOCKING_3:
				case MIPI_CX3_DVS_AREA_BLOCKING_4:
				case MIPI_CX3_DVS_AREA_BLOCKING_5:
				case MIPI_CX3_DVS_AREA_BLOCKING_6:
				case MIPI_CX3_DVS_AREA_BLOCKING_7:
				case MIPI_CX3_DVS_AREA_BLOCKING_8:
				case MIPI_CX3_DVS_AREA_BLOCKING_9:
				case MIPI_CX3_DVS_AREA_BLOCKING_10:
				case MIPI_CX3_DVS_AREA_BLOCKING_11:
				case MIPI_CX3_DVS_AREA_BLOCKING_12:
				case MIPI_CX3_DVS_AREA_BLOCKING_13:
				case MIPI_CX3_DVS_AREA_BLOCKING_14:
				case MIPI_CX3_DVS_AREA_BLOCKING_15:
				case MIPI_CX3_DVS_AREA_BLOCKING_16:
				case MIPI_CX3_DVS_AREA_BLOCKING_17:
				case MIPI_CX3_DVS_AREA_BLOCKING_18:
				case MIPI_CX3_DVS_AREA_BLOCKING_19: {
					uint16_t regAddr = REGISTER_DIGITAL_AREA_BLOCK + (2 * (paramAddr - MIPI_CX3_DVS_AREA_BLOCKING_0));

					if (!i2cConfigSend(&state->usbState, DEVICE_DVS, regAddr, U8T(param >> 8))) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, U16T(regAddr + 1), U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_TIMESTAMP_RESET: {
					if (param) {
						i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_TIMESTAMP_RESET, 0x01);
						return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_TIMESTAMP_RESET, 0x00));
					}
					break;
				}

				case MIPI_CX3_DVS_GLOBAL_RESET_ENABLE: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_MODE_CONTROL, &currVal)) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_MODE_CONTROL,
						(param) ? U8T(currVal | 0x02) : U8T(currVal & ~0x02)));
					break;
				}

				case MIPI_CX3_DVS_GLOBAL_RESET_DURING_READOUT: {
					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_GLOBAL_RESET_READOUT,
						(param) ? (0x01) : (0x00)));
					break;
				}

				case MIPI_CX3_DVS_GLOBAL_HOLD_ENABLE: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_MODE_CONTROL, &currVal)) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_MODE_CONTROL,
						(param) ? U8T(currVal | 0x01) : U8T(currVal & ~0x01)));
					break;
				}

				case MIPI_CX3_DVS_FIXED_READ_TIME_ENABLE: {
					return (i2cConfigSend(
						&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_FIXED_READ_TIME, (param) ? (0x01) : (0x00)));
					break;
				}

				case MIPI_CX3_DVS_EXTERNAL_TRIGGER_MODE: {
					if (param >= 3) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_EXTERNAL_TRIGGER, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_TIMING_ED: {
					// TODO: figure this out.
					if (param >= 128000) {
						return (false);
					}

					i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GH_COUNT, 0x00);
					i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GH_COUNT + 1, 0x00);
					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GH_COUNT + 2, 0x02));
					break;
				}

				case MIPI_CX3_DVS_TIMING_GH2GRS: {
					// TODO: figure this out.
					if (param >= 128000) {
						return (false);
					}

					i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GRS_COUNT, 0x00);
					i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GRS_COUNT + 1, 0x00);
					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GRS_COUNT + 2, 0x00));
					break;
				}

				case MIPI_CX3_DVS_TIMING_GRS: {
					// TODO: figure this out.
					if (param >= 128000) {
						return (false);
					}

					i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GRS_END, 0x00);
					i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GRS_END + 1, 0x00);
					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GRS_END + 2, 0x01));
					break;
				}

				case MIPI_CX3_DVS_TIMING_GH2SEL: {
					if (param >= 256) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_FIRST_SELX_START, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_TIMING_SELW: {
					if (param >= 256) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_SELX_WIDTH, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_TIMING_SEL2AY_R: {
					if (param >= 256) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_AY_START, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_TIMING_SEL2AY_F: {
					if (param >= 256) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_AY_END, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_TIMING_SEL2R_R: {
					if (param >= 256) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_R_START, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_TIMING_SEL2R_F: {
					if (param >= 256) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_R_END, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_TIMING_NEXT_SEL: {
					if ((param >= 65536) || (param < 5)) {
						return (false);
					}

					i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_NEXT_SELX_START, U8T(param >> 8));
					i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_NEXT_SELX_START + 1, U8T(param));

					// Also set MAX_EVENT_NUM, which is defined as NEXT_SEL-5, up to a maximum of 60.
					uint8_t maxEventNum = (param < 65) ? U8T(param - 5) : U8T(60);

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_MAX_EVENT_NUM, maxEventNum));
					break;
				}

				case MIPI_CX3_DVS_TIMING_NEXT_GH: {
					if (param >= 128) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_NEXT_GH_CNT, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_TIMING_READ_FIXED: {
					if (param >= 65536) {
						return (false);
					}

					i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_TIMING_READ_TIME_INTERVAL, U8T(param >> 8));
					return (i2cConfigSend(
						&state->usbState, DEVICE_DVS, REGISTER_TIMING_READ_TIME_INTERVAL + 1, U8T(param)));
					break;
				}

				default:
					return (false);
					break;
			}
			break;

		case MIPI_CX3_DVS_CROPPER:
			switch (paramAddr) {
				case MIPI_CX3_DVS_CROPPER_ENABLE: {
					return (i2cConfigSend(
						&state->usbState, DEVICE_DVS, REGISTER_CROPPER_BYPASS, (param) ? (0x00) : (0x01)));
					break;
				}

				case MIPI_CX3_DVS_CROPPER_Y_START_ADDRESS:
				case MIPI_CX3_DVS_CROPPER_Y_END_ADDRESS: {
					if (param >= U32T(handle->info.dvsSizeY)) {
						return (false);
					}

					// Cropper has a special corner case:
					// if both start and end pixels are in the same group,
					// only the mask in the END register is actually applied.
					// We must track the addresses, detect this, and properly
					// update all the masks as needed.
					if (paramAddr == MIPI_CX3_DVS_CROPPER_Y_START_ADDRESS) {
						state->dvs.cropperYStart = U16T(param);
					}

					if (paramAddr == MIPI_CX3_DVS_CROPPER_Y_END_ADDRESS) {
						state->dvs.cropperYEnd = U16T(param);
					}

					uint8_t startGroup = U8T(state->dvs.cropperYStart / 8);
					uint8_t startMask  = U8T(0x00FF << (state->dvs.cropperYStart % 8));

					uint8_t endGroup = U8T(state->dvs.cropperYEnd / 8);
					uint8_t endMask  = U8T(0x00FF >> (7 - (state->dvs.cropperYEnd % 8)));

					if (startGroup == endGroup) {
						// EndMask must let pass only the unblocked pixels (set to 1).
						endMask = startMask & endMask;

						// StartMask doesn't matter in this case, reset to 0xFF.
						startMask = 0xFF;
					}

					i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_CROPPER_Y_START_GROUP, startGroup);
					i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_CROPPER_Y_START_MASK, startMask);

					i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_CROPPER_Y_END_GROUP, endGroup);
					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_CROPPER_Y_END_MASK, endMask));
					break;
				}

				case MIPI_CX3_DVS_CROPPER_X_START_ADDRESS: {
					if (param >= U32T(handle->info.dvsSizeX)) {
						return (false);
					}

					i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_CROPPER_X_START_ADDRESS, U8T(param >> 8));
					return (
						i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_CROPPER_X_START_ADDRESS + 1, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_CROPPER_X_END_ADDRESS: {
					if (param >= U32T(handle->info.dvsSizeX)) {
						return (false);
					}

					i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_CROPPER_X_END_ADDRESS, U8T(param >> 8));
					return (
						i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_CROPPER_X_END_ADDRESS + 1, U8T(param)));
					break;
				}

				default:
					return (false);
					break;
			}
			break;

		case MIPI_CX3_DVS_ACTIVITY_DECISION:
			switch (paramAddr) {
				case MIPI_CX3_DVS_ACTIVITY_DECISION_ENABLE: {
					return (i2cConfigSend(
						&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_BYPASS, (param) ? (0x00) : (0x01)));
					break;
				}

				case MIPI_CX3_DVS_ACTIVITY_DECISION_POS_THRESHOLD: {
					if (param >= 65536) {
						return (false);
					}

					i2cConfigSend(
						&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_POS_THRESHOLD, U8T(param >> 8));
					return (i2cConfigSend(
						&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_POS_THRESHOLD + 1, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_ACTIVITY_DECISION_NEG_THRESHOLD: {
					if (param >= 65536) {
						return (false);
					}

					i2cConfigSend(
						&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_NEG_THRESHOLD, U8T(param >> 8));
					return (i2cConfigSend(
						&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_NEG_THRESHOLD + 1, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_ACTIVITY_DECISION_DEC_RATE: {
					if (param >= 16) {
						return (false);
					}

					return (
						i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_DEC_RATE, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_ACTIVITY_DECISION_DEC_TIME: {
					if (param >= 32) {
						return (false);
					}

					return (
						i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_DEC_TIME, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_ACTIVITY_DECISION_POS_MAX_COUNT: {
					if (param >= 65536) {
						return (false);
					}

					i2cConfigSend(
						&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_POS_MAX_COUNT, U8T(param >> 8));
					return (i2cConfigSend(
						&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_POS_MAX_COUNT + 1, U8T(param)));
					break;
				}

				default:
					return (false);
					break;
			}
			break;

		case MIPI_CX3_DVS_BIAS:
			switch (paramAddr) {
				case MIPI_CX3_DVS_BIAS_CURRENT_RANGE_LOG: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST, &currVal)) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST,
						(param) ? U8T(currVal | 0x08) : U8T(currVal & ~0x08)));
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_RANGE_SF: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST, &currVal)) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST,
						(param) ? U8T(currVal | 0x04) : U8T(currVal & ~0x04)));
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_RANGE_ON: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST, &currVal)) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST,
						(param) ? U8T(currVal | 0x02) : U8T(currVal & ~0x02)));
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_RANGE_nRST: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST, &currVal)) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST,
						(param) ? U8T(currVal | 0x01) : U8T(currVal & ~0x01)));
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_RANGE_LOGA: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS,
							REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGALOGD_MONITOR, &currVal)) {
						return (false);
					}

					return (
						i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGALOGD_MONITOR,
							(param) ? U8T(currVal | 0x10) : U8T(currVal & ~0x10)));
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_RANGE_LOGD: {
					if (param >= 4) {
						return (false);
					}

					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS,
							REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGALOGD_MONITOR, &currVal)) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS,
						REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGALOGD_MONITOR, U8T((currVal & ~0x0C) | U8T(param << 2))));
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_LEVEL_SF: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_LEVEL_SFOFF, &currVal)) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_LEVEL_SFOFF,
						(param) ? U8T(currVal | 0x10) : U8T(currVal & ~0x10)));
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_LEVEL_nOFF: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_LEVEL_SFOFF, &currVal)) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_LEVEL_SFOFF,
						(param) ? U8T(currVal | 0x02) : U8T(currVal & ~0x02)));
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_AMP: {
					if (param >= 9) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_AMP, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_ON: {
					if (param >= 9) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_ON, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_OFF: {
					if (param >= 9) {
						return (false);
					}

					return (i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_OFF, U8T(param)));
					break;
				}

				case MIPI_CX3_DVS_BIAS_SIMPLE:
					i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_AMP, 0x04);
					i2cConfigSend(
						&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGALOGD_MONITOR, 0x14);

					switch (param) {
						case MIPI_CX3_DVS_BIAS_SIMPLE_VERY_LOW: {
							i2cConfigSend(
								&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST, 0x06);
							i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_LEVEL_SFOFF, 0x7D);

							i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_ON, 0x06);
							i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_OFF, 0x02);
							break;
						}

						case MIPI_CX3_DVS_BIAS_SIMPLE_LOW: {
							i2cConfigSend(
								&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST, 0x06);
							i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_LEVEL_SFOFF, 0x7D);

							i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_ON, 0x03);
							i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_OFF, 0x05);
							break;
						}

						case MIPI_CX3_DVS_BIAS_SIMPLE_HIGH: {
							i2cConfigSend(
								&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST, 0x04);
							i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_LEVEL_SFOFF, 0x7F);

							i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_ON, 0x05);
							i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_OFF, 0x03);
							break;
						}

						case MIPI_CX3_DVS_BIAS_SIMPLE_VERY_HIGH: {
							i2cConfigSend(
								&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST, 0x04);
							i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_LEVEL_SFOFF, 0x7F);

							i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_ON, 0x02);
							i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_OFF, 0x06);
							break;
						}

						case MIPI_CX3_DVS_BIAS_SIMPLE_DEFAULT:
						default: {
							i2cConfigSend(
								&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST, 0x06);
							i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_LEVEL_SFOFF, 0x7D);

							i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_ON, 0x00);
							i2cConfigSend(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_OFF, 0x08);
							break;
						}
					}
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

bool mipiCx3ConfigGet(caerDeviceHandle cdh, int8_t modAddr, uint8_t paramAddr, uint32_t *param) {
	mipiCx3Handle handle = (mipiCx3Handle) cdh;
	mipiCx3State state   = &handle->state;

	switch (modAddr) {
		case CAER_HOST_CONFIG_USB:
			return (usbConfigGet(&state->usbState, paramAddr, param));
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

		case MIPI_CX3_DVS:
			switch (paramAddr) {
				case MIPI_CX3_DVS_MODE: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_CONTROL_MODE, &currVal)) {
						return (false);
					}

					*param = currVal;
					break;
				}

				case MIPI_CX3_DVS_EVENT_FLATTEN: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_CONTROL_PACKET_FORMAT, &currVal)) {
						return (false);
					}

					*param = ((currVal & 0x40) == true);
					break;
				}

				case MIPI_CX3_DVS_EVENT_ON_ONLY: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_CONTROL_PACKET_FORMAT, &currVal)) {
						return (false);
					}

					*param = ((currVal & 0x20) == true);
					break;
				}

				case MIPI_CX3_DVS_EVENT_OFF_ONLY: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_CONTROL_PACKET_FORMAT, &currVal)) {
						return (false);
					}

					*param = ((currVal & 0x10) == true);
					break;
				}

				case MIPI_CX3_DVS_SUBSAMPLE_ENABLE: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_ENABLE, &currVal)) {
						return (false);
					}

					*param = ((currVal & 0x04) == true);
					break;
				}

				case MIPI_CX3_DVS_AREA_BLOCKING_ENABLE: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_ENABLE, &currVal)) {
						return (false);
					}

					*param = ((currVal & 0x02) == true);
					break;
				}

				case MIPI_CX3_DVS_DUAL_BINNING_ENABLE: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_ENABLE, &currVal)) {
						return (false);
					}

					*param = currVal;
					break;
				}

				case MIPI_CX3_DVS_SUBSAMPLE_VERTICAL: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_SUBSAMPLE_RATIO, &currVal)) {
						return (false);
					}

					*param = ((currVal & 0x38) >> 3);
					break;
				}

				case MIPI_CX3_DVS_SUBSAMPLE_HORIZONTAL: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_SUBSAMPLE_RATIO, &currVal)) {
						return (false);
					}

					*param = (currVal & 0x07);
					break;
				}

				case MIPI_CX3_DVS_AREA_BLOCKING_0:
				case MIPI_CX3_DVS_AREA_BLOCKING_1:
				case MIPI_CX3_DVS_AREA_BLOCKING_2:
				case MIPI_CX3_DVS_AREA_BLOCKING_3:
				case MIPI_CX3_DVS_AREA_BLOCKING_4:
				case MIPI_CX3_DVS_AREA_BLOCKING_5:
				case MIPI_CX3_DVS_AREA_BLOCKING_6:
				case MIPI_CX3_DVS_AREA_BLOCKING_7:
				case MIPI_CX3_DVS_AREA_BLOCKING_8:
				case MIPI_CX3_DVS_AREA_BLOCKING_9:
				case MIPI_CX3_DVS_AREA_BLOCKING_10:
				case MIPI_CX3_DVS_AREA_BLOCKING_11:
				case MIPI_CX3_DVS_AREA_BLOCKING_12:
				case MIPI_CX3_DVS_AREA_BLOCKING_13:
				case MIPI_CX3_DVS_AREA_BLOCKING_14:
				case MIPI_CX3_DVS_AREA_BLOCKING_15:
				case MIPI_CX3_DVS_AREA_BLOCKING_16:
				case MIPI_CX3_DVS_AREA_BLOCKING_17:
				case MIPI_CX3_DVS_AREA_BLOCKING_18:
				case MIPI_CX3_DVS_AREA_BLOCKING_19: {
					uint16_t regAddr = REGISTER_DIGITAL_AREA_BLOCK + (2 * (paramAddr - MIPI_CX3_DVS_AREA_BLOCKING_0));

					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, regAddr, &currVal)) {
						return (false);
					}

					*param = U32T(currVal << 8);

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, U16T(regAddr + 1), &currVal)) {
						return (false);
					}

					*param |= currVal;
					break;
				}

				case MIPI_CX3_DVS_TIMESTAMP_RESET: {
					*param = false;
					break;
				}

				case MIPI_CX3_DVS_GLOBAL_RESET_ENABLE: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_MODE_CONTROL, &currVal)) {
						return (false);
					}

					*param = ((currVal & 0x02) == true);
					break;
				}

				case MIPI_CX3_DVS_GLOBAL_RESET_DURING_READOUT: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_GLOBAL_RESET_READOUT, &currVal)) {
						return (false);
					}

					*param = currVal;
					break;
				}

				case MIPI_CX3_DVS_GLOBAL_HOLD_ENABLE: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_MODE_CONTROL, &currVal)) {
						return (false);
					}

					*param = ((currVal & 0x01) == true);
					break;
				}

				case MIPI_CX3_DVS_FIXED_READ_TIME_ENABLE: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_FIXED_READ_TIME, &currVal)) {
						return (false);
					}

					*param = currVal;
					break;
				}

				case MIPI_CX3_DVS_EXTERNAL_TRIGGER_MODE: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_DIGITAL_EXTERNAL_TRIGGER, &currVal)) {
						return (false);
					}

					*param = currVal;
					break;
				}

				case MIPI_CX3_DVS_TIMING_ED: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GH_COUNT + 1, &currVal)) {
						return (false);
					}

					*param = U32T(currVal << 8);

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GH_COUNT + 2, &currVal)) {
						return (false);
					}

					*param |= currVal;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GH_COUNT, &currVal)) {
						return (false);
					}

					*param *= currVal;
					break;
				}

				case MIPI_CX3_DVS_TIMING_GH2GRS: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GRS_COUNT + 1, &currVal)) {
						return (false);
					}

					*param = U32T(currVal << 8);

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GRS_COUNT + 2, &currVal)) {
						return (false);
					}

					*param |= currVal;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GRS_COUNT, &currVal)) {
						return (false);
					}

					*param *= currVal;
					break;
				}

				case MIPI_CX3_DVS_TIMING_GRS: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GRS_END + 1, &currVal)) {
						return (false);
					}

					*param = U32T(currVal << 8);

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GRS_END + 2, &currVal)) {
						return (false);
					}

					*param |= currVal;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_GRS_END, &currVal)) {
						return (false);
					}

					*param *= currVal;
					break;
				}

				case MIPI_CX3_DVS_TIMING_GH2SEL: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_FIRST_SELX_START, &currVal)) {
						return (false);
					}

					*param = currVal;
					break;
				}

				case MIPI_CX3_DVS_TIMING_SELW: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_SELX_WIDTH, &currVal)) {
						return (false);
					}

					*param = currVal;
					break;
				}

				case MIPI_CX3_DVS_TIMING_SEL2AY_R: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_AY_START, &currVal)) {
						return (false);
					}

					*param = currVal;
					break;
				}

				case MIPI_CX3_DVS_TIMING_SEL2AY_F: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_AY_END, &currVal)) {
						return (false);
					}

					*param = currVal;
					break;
				}

				case MIPI_CX3_DVS_TIMING_SEL2R_R: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_R_START, &currVal)) {
						return (false);
					}

					*param = currVal;
					break;
				}

				case MIPI_CX3_DVS_TIMING_SEL2R_F: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_R_END, &currVal)) {
						return (false);
					}

					*param = currVal;
					break;
				}

				case MIPI_CX3_DVS_TIMING_NEXT_SEL: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_NEXT_SELX_START, &currVal)) {
						return (false);
					}

					*param = U32T(currVal << 8);

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_TIMING_NEXT_SELX_START + 1, &currVal)) {
						return (false);
					}

					*param |= currVal;
					break;
				}

				case MIPI_CX3_DVS_TIMING_NEXT_GH: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_NEXT_GH_CNT, &currVal)) {
						return (false);
					}

					*param = currVal;
					break;
				}

				case MIPI_CX3_DVS_TIMING_READ_FIXED: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_TIMING_READ_TIME_INTERVAL, &currVal)) {
						return (false);
					}

					*param = U32T(currVal << 8);

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_TIMING_READ_TIME_INTERVAL + 1, &currVal)) {
						return (false);
					}

					*param |= currVal;
					break;
				}

				default:
					return (false);
					break;
			}
			break;

		case MIPI_CX3_DVS_CROPPER:
			switch (paramAddr) {
				case MIPI_CX3_DVS_CROPPER_ENABLE: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_CROPPER_BYPASS, &currVal)) {
						return (false);
					}

					*param = !currVal;
					break;
				}

				case MIPI_CX3_DVS_CROPPER_Y_START_ADDRESS: {
					*param = state->dvs.cropperYStart;
					break;
				}

				case MIPI_CX3_DVS_CROPPER_Y_END_ADDRESS: {
					*param = state->dvs.cropperYEnd;
					break;
				}

				case MIPI_CX3_DVS_CROPPER_X_START_ADDRESS: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_CROPPER_X_START_ADDRESS, &currVal)) {
						return (false);
					}

					*param = U32T(currVal << 8);

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_CROPPER_X_START_ADDRESS + 1, &currVal)) {
						return (false);
					}

					*param |= currVal;
					break;
				}

				case MIPI_CX3_DVS_CROPPER_X_END_ADDRESS: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_CROPPER_X_END_ADDRESS, &currVal)) {
						return (false);
					}

					*param = U32T(currVal << 8);

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_CROPPER_X_END_ADDRESS + 1, &currVal)) {
						return (false);
					}

					*param |= currVal;
					break;
				}

				default:
					return (false);
					break;
			}
			break;

		case MIPI_CX3_DVS_ACTIVITY_DECISION:
			switch (paramAddr) {
				case MIPI_CX3_DVS_ACTIVITY_DECISION_ENABLE: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_BYPASS, &currVal)) {
						return (false);
					}

					*param = !currVal;
					break;
				}

				case MIPI_CX3_DVS_ACTIVITY_DECISION_POS_THRESHOLD: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_POS_THRESHOLD, &currVal)) {
						return (false);
					}

					*param = U32T(currVal << 8);

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_POS_THRESHOLD + 1, &currVal)) {
						return (false);
					}

					*param |= currVal;
					break;
				}

				case MIPI_CX3_DVS_ACTIVITY_DECISION_NEG_THRESHOLD: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_NEG_THRESHOLD, &currVal)) {
						return (false);
					}

					*param = U32T(currVal << 8);

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_NEG_THRESHOLD + 1, &currVal)) {
						return (false);
					}

					*param |= currVal;
					break;
				}

				case MIPI_CX3_DVS_ACTIVITY_DECISION_DEC_RATE: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_DEC_RATE, &currVal)) {
						return (false);
					}

					*param = currVal;
					break;
				}

				case MIPI_CX3_DVS_ACTIVITY_DECISION_DEC_TIME: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_DEC_TIME, &currVal)) {
						return (false);
					}

					*param = currVal;
					break;
				}

				case MIPI_CX3_DVS_ACTIVITY_DECISION_POS_MAX_COUNT: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_POS_MAX_COUNT, &currVal)) {
						return (false);
					}

					*param = U32T(currVal << 8);

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_ACTIVITY_DECISION_POS_MAX_COUNT + 1, &currVal)) {
						return (false);
					}

					*param |= currVal;
					break;
				}

				default:
					return (false);
					break;
			}
			break;

		case MIPI_CX3_DVS_BIAS:
			switch (paramAddr) {
				case MIPI_CX3_DVS_BIAS_CURRENT_RANGE_LOG: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST, &currVal)) {
						return (false);
					}

					*param = ((currVal & 0x08) == true);
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_RANGE_SF: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST, &currVal)) {
						return (false);
					}

					*param = ((currVal & 0x04) == true);
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_RANGE_ON: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST, &currVal)) {
						return (false);
					}

					*param = ((currVal & 0x02) == true);
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_RANGE_nRST: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(
							&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGSFONREST, &currVal)) {
						return (false);
					}

					*param = ((currVal & 0x01) == true);
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_RANGE_LOGA: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS,
							REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGALOGD_MONITOR, &currVal)) {
						return (false);
					}

					*param = ((currVal & 0x10) == true);
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_RANGE_LOGD: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS,
							REGISTER_BIAS_CURRENT_RANGE_SELECT_LOGALOGD_MONITOR, &currVal)) {
						return (false);
					}

					*param = ((currVal & 0x0C) >> 2);
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_LEVEL_SF: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_LEVEL_SFOFF, &currVal)) {
						return (false);
					}

					*param = ((currVal & 0x10) == true);
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_LEVEL_nOFF: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_LEVEL_SFOFF, &currVal)) {
						return (false);
					}

					*param = ((currVal & 0x02) == true);
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_AMP: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_AMP, &currVal)) {
						return (false);
					}

					*param = currVal;
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_ON: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_ON, &currVal)) {
						return (false);
					}

					*param = currVal;
					break;
				}

				case MIPI_CX3_DVS_BIAS_CURRENT_OFF: {
					uint8_t currVal = 0;

					if (!i2cConfigReceive(&state->usbState, DEVICE_DVS, REGISTER_BIAS_CURRENT_OFF, &currVal)) {
						return (false);
					}

					*param = currVal;
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

bool mipiCx3DataStart(caerDeviceHandle cdh, void (*dataNotifyIncrease)(void *ptr),
	void (*dataNotifyDecrease)(void *ptr), void *dataNotifyUserPtr, void (*dataShutdownNotify)(void *ptr),
	void *dataShutdownUserPtr) {
	mipiCx3Handle handle = (mipiCx3Handle) cdh;
	mipiCx3State state   = &handle->state;

	usbSetShutdownCallback(&state->usbState, dataShutdownNotify, dataShutdownUserPtr);

	// Store new data available/not available anymore call-backs.
	dataExchangeSetNotify(&state->dataExchange, dataNotifyIncrease, dataNotifyDecrease, dataNotifyUserPtr);

	containerGenerationCommitTimestampReset(&state->container);

	if (!dataExchangeBufferInit(&state->dataExchange)) {
		mipiCx3Log(CAER_LOG_CRITICAL, handle, "Failed to initialize data exchange buffer.");
		return (false);
	}

	// Allocate packets.
	if (!containerGenerationAllocate(&state->container, MIPI_CX3_EVENT_TYPES)) {
		freeAllDataMemory(state);

		mipiCx3Log(CAER_LOG_CRITICAL, handle, "Failed to allocate event packet container.");
		return (false);
	}

	state->currentPackets.polarity
		= caerPolarityEventPacketAllocate(MIPI_CX3_POLARITY_DEFAULT_SIZE, I16T(handle->info.deviceID), 0);
	if (state->currentPackets.polarity == NULL) {
		freeAllDataMemory(state);

		mipiCx3Log(CAER_LOG_CRITICAL, handle, "Failed to allocate polarity event packet.");
		return (false);
	}

	state->currentPackets.special
		= caerSpecialEventPacketAllocate(MIPI_CX3_SPECIAL_DEFAULT_SIZE, I16T(handle->info.deviceID), 0);
	if (state->currentPackets.special == NULL) {
		freeAllDataMemory(state);

		mipiCx3Log(CAER_LOG_CRITICAL, handle, "Failed to allocate special event packet.");
		return (false);
	}

	// And reset the USB side of things.
	usbControlResetDataEndpoint(&state->usbState, MIPI_CX3_DATA_ENDPOINT);

	if (!usbDataTransfersStart(&state->usbState)) {
		freeAllDataMemory(state);

		mipiCx3Log(CAER_LOG_CRITICAL, handle, "Failed to start data transfers.");
		return (false);
	}

	if (dataExchangeStartProducers(&state->dataExchange)) {
		mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_TIMESTAMP_RESET, true);
		mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_MODE, MIPI_CX3_DVS_MODE_STREAM);
	}

	return (true);
}

bool mipiCx3DataStop(caerDeviceHandle cdh) {
	mipiCx3Handle handle = (mipiCx3Handle) cdh;
	mipiCx3State state   = &handle->state;

	if (dataExchangeStopProducers(&state->dataExchange)) {
		mipiCx3ConfigSet(cdh, MIPI_CX3_DVS, MIPI_CX3_DVS_MODE, MIPI_CX3_DVS_MODE_OFF);
	}

	usbDataTransfersStop(&state->usbState);

	dataExchangeBufferEmpty(&state->dataExchange);

	// Free current, uncommitted packets and ringbuffer.
	freeAllDataMemory(state);

	// Reset packet positions.
	state->currentPackets.polarityPosition = 0;
	state->currentPackets.specialPosition  = 0;

	return (true);
}

caerEventPacketContainer mipiCx3DataGet(caerDeviceHandle cdh) {
	mipiCx3Handle handle = (mipiCx3Handle) cdh;
	mipiCx3State state   = &handle->state;

	return (dataExchangeGet(&state->dataExchange, &state->usbState.dataTransfersRun));
}

static inline bool ensureSpaceForEvents(
	caerEventPacketHeader *packet, size_t position, size_t numEvents, mipiCx3Handle handle) {
	if ((position + numEvents) <= (size_t) caerEventPacketHeaderGetEventCapacity(*packet)) {
		return (true);
	}

	caerEventPacketHeader grownPacket
		= caerEventPacketGrow(*packet, caerEventPacketHeaderGetEventCapacity(*packet) * 2);
	if (grownPacket == NULL) {
		mipiCx3Log(CAER_LOG_CRITICAL, handle, "Failed to grow event packet of type %d.",
			caerEventPacketHeaderGetEventType(*packet));
		return (false);
	}

	*packet = grownPacket;
	return (true);
}

static void mipiCx3EventTranslator(void *vhd, const uint8_t *buffer, size_t bufferSize) {
	mipiCx3Handle handle = vhd;
	mipiCx3State state   = &handle->state;

	// Return right away if not running anymore. This prevents useless work if many
	// buffers are still waiting when shut down, as well as incorrect event sequences
	// if a TS_RESET is stuck on ring-buffer commit further down, and detects shut-down;
	// then any subsequent buffers should also detect shut-down and not be handled.
	if (!usbDataTransfersAreRunning(&state->usbState)) {
		return;
	}

	// Truncate off any extra partial event.
	if ((bufferSize & 0x03) != 0) {
		mipiCx3Log(CAER_LOG_ALERT, handle, "%zu bytes received via USB, which is not a multiple of four.", bufferSize);
		bufferSize &= ~((size_t) 0x03);
	}

	for (size_t bufferPos = 0; bufferPos < bufferSize; bufferPos += 4) {
		// Allocate new packets for next iteration as needed.
		if (!containerGenerationAllocate(&state->container, MIPI_CX3_EVENT_TYPES)) {
			mipiCx3Log(CAER_LOG_CRITICAL, handle, "Failed to allocate event packet container.");
			return;
		}

		if (state->currentPackets.special == NULL) {
			state->currentPackets.special
				= caerSpecialEventPacketAllocate(MIPI_CX3_SPECIAL_DEFAULT_SIZE, I16T(handle->info.deviceID), 0);
			if (state->currentPackets.special == NULL) {
				mipiCx3Log(CAER_LOG_CRITICAL, handle, "Failed to allocate special event packet.");
				return;
			}
		}

		if (state->currentPackets.polarity == NULL) {
			state->currentPackets.polarity
				= caerPolarityEventPacketAllocate(MIPI_CX3_POLARITY_DEFAULT_SIZE, I16T(handle->info.deviceID), 0);
			if (state->currentPackets.polarity == NULL) {
				mipiCx3Log(CAER_LOG_CRITICAL, handle, "Failed to allocate polarity event packet.");
				return;
			}
		}

		bool tsReset   = false;
		bool tsBigWrap = false;

		uint32_t event = be32toh(*((const uint32_t *) (&buffer[bufferPos])));

		if (event & 0x80000000) {
			if (state->container.currentPacketContainerCommitTimestamp == -1) {
				// No timestamp received yet.
				continue;
			}

			// SGROUP or MGROUP event.
			if (event & 0x76000000) {
				// TODO: MGROUP support.
				mipiCx3Log(CAER_LOG_CRITICAL, handle, "MGROUP not handled.");
			}
			else {
				// SGROUP address.
				uint16_t groupAddr = (event >> 18) & 0x003F;

				groupAddr *= 8; // 8 pixels per group.

				// 8-pixel group, two polarities, up to 16 events can be generated.
				if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.polarity,
						(size_t) state->currentPackets.polarityPosition, 16, handle)) {
					for (uint16_t i = 0, mask = 0x8000; i < 16; i++, mask >>= 1) {
						// Check if event present first.
						if ((event & mask) == 0) {
							continue;
						}

						bool polarity   = (i >= 8);
						uint16_t offset = 7 - (i & 0x07);

						// Received event!
						caerPolarityEvent currentPolarityEvent = caerPolarityEventPacketGetEvent(
							state->currentPackets.polarity, state->currentPackets.polarityPosition);

						// Timestamp at event-stream insertion point.
						caerPolarityEventSetTimestamp(currentPolarityEvent, state->timestamps.current);
						caerPolarityEventSetPolarity(currentPolarityEvent, polarity);
						caerPolarityEventSetX(currentPolarityEvent, state->dvs.lastX);
						caerPolarityEventSetY(currentPolarityEvent, groupAddr + offset);
						caerPolarityEventValidate(currentPolarityEvent, state->currentPackets.polarity);
						state->currentPackets.polarityPosition++;
					}
				}
			}
		}
		else {
			// COLUMN event.
			if (event & 0x04000000) {
				uint16_t columnAddr   = event & 0x03FF;
				uint16_t timestampSub = (event >> 11) & 0x03FF;
				bool startOfFrame     = (event >> 21) & 0x01;

				state->dvs.lastX = columnAddr;

				// Timestamp handling
				if (timestampSub != state->timestamps.lastSub) {
					if (state->timestamps.currentReference == state->timestamps.lastReference
						&& timestampSub < state->timestamps.lastSub) {
						// Reference did not change, but sub-timestamp did wrap around.
						// We must have lost a main reference timestamp due to high traffic.
						// In this case we increase manually by 1ms, as if we'd received it.
						state->timestamps.lastReference += 1000;
					}

					state->timestamps.currentReference = state->timestamps.lastReference;
				}

				state->timestamps.lastSub = timestampSub;

				state->timestamps.last    = state->timestamps.current;
				state->timestamps.current = I32T(state->timestamps.currentReference + timestampSub);

				// Check monotonicity of timestamps.
				checkMonotonicTimestamp(state->timestamps.current, state->timestamps.last, handle->info.deviceString,
					&state->deviceLogLevel);

				containerGenerationCommitTimestampInit(&state->container, state->timestamps.current);

				if (startOfFrame) {
					mipiCx3Log(CAER_LOG_DEBUG, handle, "Start of Frame column marker detected.");

					if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
							(size_t) state->currentPackets.specialPosition, 1, handle)) {
						caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(
							state->currentPackets.special, state->currentPackets.specialPosition);
						caerSpecialEventSetTimestamp(currentSpecialEvent, state->timestamps.current);
						caerSpecialEventSetType(currentSpecialEvent, EVENT_READOUT_START);
						caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
						state->currentPackets.specialPosition++;
					}
				}
			}

			// TIMESTAMP event.
			if (event & 0x08000000) {
				uint32_t timestampRef = event & 0x003FFFFF;

				// In ms, convert to µs.
				timestampRef *= 1000;

				state->timestamps.lastReference = timestampRef;
			}
		}

		// Thresholds on which to trigger packet container commit.
		// tsReset and tsBigWrap are already defined above.
		// Trigger if any of the global container-wide thresholds are met.
		int32_t currentPacketContainerCommitSize = containerGenerationGetMaxPacketSize(&state->container);
		bool containerSizeCommit                 = (currentPacketContainerCommitSize > 0)
								   && ((state->currentPackets.polarityPosition >= currentPacketContainerCommitSize)
									   || (state->currentPackets.specialPosition >= currentPacketContainerCommitSize));

		bool containerTimeCommit
			= containerGenerationIsCommitTimestampElapsed(&state->container, 0, state->timestamps.current);

		// Commit packet containers to the ring-buffer, so they can be processed by the
		// main-loop, when any of the required conditions are met.
		if (tsReset || tsBigWrap || containerSizeCommit || containerTimeCommit) {
			// One or more of the commit triggers are hit. Set the packet container up to contain
			// any non-empty packets. Empty packets are not forwarded to save memory.
			bool emptyContainerCommit = true;

			if (state->currentPackets.polarityPosition > 0) {
				containerGenerationSetPacket(
					&state->container, POLARITY_EVENT, (caerEventPacketHeader) state->currentPackets.polarity);

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

			containerGenerationExecute(&state->container, emptyContainerCommit, tsReset, 0, state->timestamps.current,
				&state->dataExchange, &state->usbState.dataTransfersRun, handle->info.deviceID,
				handle->info.deviceString, &state->deviceLogLevel);
		}
	}
}

static bool i2cConfigSend(usbState state, uint16_t deviceAddr, uint16_t byteAddr, uint8_t param) {
	return (usbControlTransferOut(state, VENDOR_REQUEST_I2C_WRITE, deviceAddr, byteAddr, &param, 1));
}

static bool i2cConfigReceive(usbState state, uint16_t deviceAddr, uint16_t byteAddr, uint8_t *param) {
	return (usbControlTransferIn(state, VENDOR_REQUEST_I2C_READ, deviceAddr, byteAddr, param, 1));
}
