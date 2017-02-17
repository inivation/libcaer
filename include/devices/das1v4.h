/**
 * @file DAS1V4.h
 *
 * DAS1V4 specific configuration defines and information structures.
 */

#ifndef LIBCAER_DEVICES_DAS1V4_H_
#define LIBCAER_DEVICES_DAS1V4_H_

#include "usb.h"
#include "../events/spike.h"
#include "../events/special.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Device type definition for iniLabs CochleaAMS1cV4 FX2-based boards.
 */
#define CAER_DEVICE_DAS1V4 4


/**
 * CochleaAMS1cV4 chip identifier.
 */
#define DAS1V4_CHIP_DAS1V4 65

/**
 * Module address: device-side Multiplexer configuration.
 * The Multiplexer is responsible for mixing, timestamping and outputting
 * (via USB) the various event types generated by the device. It is also
 * responsible for timestamp generation.
 */
#define DAS1V4_CONFIG_MUX      0
/**
 * Module address: device-side AER configuration (from chip).
 * The AER state machine handshakes with the chip's AER bus and gets the
 * spike events from it. It supports various configurable delays.
 */
#define DAS1V4_CONFIG_AER      1
/**
 * Module address: device-side chip control configuration.
 * This state machine is responsible for configuring the chip's internal
 * control registers, to set special options and biases.
 */
#define DAS1V4_CONFIG_CHIP     5
/**
 * Module address: device-side system information.
 * The system information module provides various details on the device, such
 * as currently installed logic revision or clock speeds.
 * All its parameters are read-only.
 * This is reserved for internal use and should not be used by
 * anything other than libcaer. Please see the 'struct caer_das1v4_info'
 * documentation for more details on what information is available.
 */
#define DAS1V4_CONFIG_SYSINFO  6
/**
 * Module address: device-side USB output configuration.
 * The USB output module forwards the data from the device and the FPGA/CPLD to
 * the USB chip, usually a Cypress FX2 or FX3.
 */
#define DAS1V4_CONFIG_USB      9
/**
 * Parameter address for module DYNAPSE_CONFIG_CHIP:
 * set the configuration content to send to the chip.
 * Every time this changes, the chip ID is appended
 * and the configuration is sent out to the chip.
 */
#define DAS1V4_CONFIG_CHIP_CONTENT         2

/**
 * Parameter address for module DAS1V4_CONFIG_MUX:
 * run the Multiplexer state machine, which is responsible for
 * mixing the various event types at the device level, timestamping
 * them and outputting them via USB or other connectors.
 */
#define DAS1V4_CONFIG_MUX_RUN                             0
/**
 * Parameter address for module DAS1V4_CONFIG_MUX:
 * run the Timestamp Generator inside the Multiplexer state machine,
 * which will provide microsecond accurate timestamps to the
 * events passing through.
 */
#define DAS1V4_CONFIG_MUX_TIMESTAMP_RUN                   1
/**
 * Parameter address for module DAS1V4_CONFIG_MUX:
 * reset the Timestamp Generator to zero. This also sends a reset
 * pulse to all connected slave devices, resetting their timestamp too.
 */
#define DAS1V4_CONFIG_MUX_TIMESTAMP_RESET                 2
/**
 * Parameter address for module DAS1V4_CONFIG_MUX:
 * under normal circumstances, the chip's bias generator is only powered
 * up when either the AER or the configuration state machines are running, to save
 * power. This flag forces the bias generator to be powered up all the time.
 */
#define DAS1V4_CONFIG_MUX_FORCE_CHIP_BIAS_ENABLE          3
/**
 * Parameter address for module DAS1V4_CONFIG_MUX:
 * drop AER events if the USB output FIFO is full, instead of having
 * them pile up at the input FIFOs.
 */
#define DAS1V4_CONFIG_MUX_DROP_AER_ON_TRANSFER_STALL      4

/**
 * Parameter address for module DAS1V4_CONFIG_AER:
 * run the AER state machine and get spike events from the chip by
 * handshaking with its AER bus.
 */
#define DAS1V4_CONFIG_AER_RUN                    3
/**
 * Parameter address for module DAS1V4_CONFIG_AER:
 * delay capturing the data and acknowledging it on the AER bus for
 * the events by this many LogicClock cycles.
 */
#define DAS1V4_CONFIG_AER_ACK_DELAY          4
/**
 * Parameter address for module DAS1V4_CONFIG_AER:
 * extend the length of the acknowledge on the AER bus for
 * the events by this many LogicClock cycles.
 */
#define DAS1V4_CONFIG_AER_ACK_EXTENSION      6
/**
 * Parameter address for module DAS1V4_CONFIG_AER:
 * if the output FIFO for this module is full, stall the AER handshake with
 * the chip and wait until it's free again, instead of just continuing
 * the handshake and dropping the resulting events.
 */
#define DAS1V4_CONFIG_AER_WAIT_ON_TRANSFER_STALL 8
/**
 * Parameter address for module DAS1V4_CONFIG_AER:
 * enable external AER control. This ensures the chip and the neuron
 * array are running, but doesn't do the handshake and leaves the ACK
 * pin in high-impedance, to allow for an external system to take
 * over the AER communication with the chip.
 * DAS1V4_CONFIG_AER_RUN has to be turned off for this to work.
 */
#define DAS1V4_CONFIG_AER_EXTERNAL_AER_CONTROL   10



/**
 * Parameter address for module DAS1V4_CONFIG_CHIP:
 * enable the configuration AER state machine to send
 * bias and control configuration to the chip.
 */
#define DAS1V4_CONFIG_CHIP_RUN             0
/**
 * Parameter address for module DAS1V4_CONFIG_CHIP:
 * delay doing the request after putting out the data
 * by this many LogicClock cycles.
 */
#define DAS1V4_CONFIG_CHIP_REQ_DELAY       3
/**
 * Parameter address for module DAS1V4_CONFIG_CHIP:
 * extend the request after receiving the ACK by
 * this many LogicClock cycles.
 */
#define DAS1V4_CONFIG_CHIP_REQ_EXTENSION   4

/**
 * Parameter address for module DAS1V4_CONFIG_SYSINFO:
 * read-only parameter, the version of the logic currently
 * running on the device's FPGA/CPLD. It usually represents
 * a specific SVN revision, at which the logic code was
 * synthesized.
 * This is reserved for internal use and should not be used by
 * anything other than libcaer. Please see the 'struct caer_das1v4_info'
 * documentation to get this information.
 */
#define DAS1V4_CONFIG_SYSINFO_LOGIC_VERSION    0
/**
 * Parameter address for module DAS1V4_CONFIG_SYSINFO:
 * read-only parameter, an integer used to identify the different
 * types of sensor chips used on the device.
 * This is reserved for internal use and should not be used by
 * anything other than libcaer. Please see the 'struct caer_das1v4_info'
 * documentation to get this information.
 */
#define DAS1V4_CONFIG_SYSINFO_CHIP_IDENTIFIER  1
/**
 * Parameter address for module DAS1V4_CONFIG_SYSINFO:
 * read-only parameter, whether the device is currently a timestamp
 * master or slave when synchronizing multiple devices together.
 * This is reserved for internal use and should not be used by
 * anything other than libcaer. Please see the 'struct caer_das1v4_info'
 * documentation to get this information.
 */
#define DAS1V4_CONFIG_SYSINFO_DEVICE_IS_MASTER 2
/**
 * Parameter address for module DAS1V4_CONFIG_SYSINFO:
 * read-only parameter, the frequency in MHz at which the main
 * FPGA/CPLD logic is running.
 * This is reserved for internal use and should not be used by
 * anything other than libcaer. Please see the 'struct caer_das1v4_info'
 * documentation to get this information.
 */
#define DAS1V4_CONFIG_SYSINFO_LOGIC_CLOCK      3

/**
 * Parameter address for module DAS1V4_CONFIG_USB:
 * enable the USB FIFO module, which transfers the data from the
 * FPGA/CPLD to the USB chip, to be then sent to the host.
 * Turning this off will suppress any USB data communication!
 */
#define DAS1V4_CONFIG_USB_RUN                0
/**
 * Parameter address for module DAS1V4_CONFIG_USB:
 * the time delay after which a packet of data is committed to
 * USB, even if it is not full yet (short USB packet).
 * The value is in 125µs time-slices, corresponding to how
 * USB schedules its operations (a value of 4 for example
 * would mean waiting at most 0.5ms until sending a short
 * USB packet to the host).
 */
#define DAS1V4_CONFIG_USB_EARLY_PACKET_DELAY 1


/*
 * CochleaAMS1cV4 channel number
 * */
#define DAS1V4_CONFIG_NUMCHANNEL	64


/*
*  maximum user memory per query, libusb will digest it in chuncks of max 512 bytes per single transfer
* */
#define DAS1V4_MAX_USER_USB_PACKET_SIZE	1024
/*
 *  libusb max 512 bytes per single transfer
 * */
#define DAS1V4_CONFIG_MAX_USB_TRANSFER 512
/*
 *  maximum number of 6 bytes configuration parameters, it needs to fit in 512
 * */
#define DAS1V4_CONFIG_MAX_PARAM_SIZE 85


/**
 * Parameter address for module DAS1V4_CONFIG_BIAS:
 * DAS1V4 chip biases.
 * Bias configuration values must be generated using the proper
 * functions, which are:
 * - convertBias() for coarse-fine (current) biases.
 * See 'http://inilabs.com/support/biasing/' for more details.
 */

/**
 * CochleaAMS1cV4 device-related information.
 */
struct caer_das1v4_info {
	/// Unique device identifier. Also 'source' for events.
	int16_t deviceID;
	/// Device serial number.
	char deviceSerialNumber[8 + 1];
	/// Device USB bus number.
	uint8_t deviceUSBBusNumber;
	/// Device USB device address.
	uint8_t deviceUSBDeviceAddress;
	/// Device information string, for logging purposes.
	char *deviceString;
	/// Logic (FPGA/CPLD) version.
	int16_t logicVersion;
	/// Whether the device is a time-stamp master or slave.
	bool deviceIsMaster;
	/// Clock in MHz for main logic (FPGA/CPLD).
	int16_t logicClock;
	/// Chip identifier/type.
	int16_t chipID;
};

/**
  * On-chip coarse-fine bias current configuration.
  * See 'http://inilabs.com/support/biasing/' for more details.
 */
 struct caer_bias_das1v4 {
 	/// Coarse current, from 0 to 7, creates big variations in output current.
 	uint8_t coarseValue;
 	/// Fine current, from 0 to 255, creates small variations in output current.
 	uint8_t fineValue;
 	/// Bias current level: true for 'HighBias, false for 'LowBias'.
 	bool BiasLowHi;
 	/// Bias type: true for 'Normal', false for 'Cascode'.
 	bool currentLevel;
 	/// Bias sex: true for 'NBias' type, false for 'PBias' type.
 	bool sex;
 	/// Whether this bias is enabled or not.
 	bool enabled;
 	/// whether this is a special bias.
 	bool special;
 };

/**
 * Return basic information on the device, such as its ID, the logic
 * version, and so on. See the 'struct caer_das1v4_info' documentation
 * for more details.
 *
 * @param handle a valid device handle.
 *
 * @return a copy of the device information structure if successful,
 *         an empty structure (all zeros) on failure.
 */
struct caer_das1v4_info caerDas1v4InfoGet(caerDeviceHandle handle);


#ifdef __cplusplus
}
#endif

#endif /* LIBCAER_DEVICES_DAS1V4_H_ */
