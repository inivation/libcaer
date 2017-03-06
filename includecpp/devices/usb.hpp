#ifndef LIBCAER_DEVICES_USB_HPP_
#define LIBCAER_DEVICES_USB_HPP_

#include <libcaer/devices/usb.h>
#include "../libcaer.hpp"
#include "../events/packetContainer.hpp"
#include "../events/utils.hpp"

namespace libcaer {
namespace devices {

class usb {
protected:
	caerDeviceHandle handle;

	usb(uint16_t deviceID, uint16_t deviceType) :
			usb(deviceID, deviceType, 0, 0, std::string()) {
	}

	usb(uint16_t deviceID, uint16_t deviceType, uint8_t busNumberRestrict, uint8_t devAddressRestrict,
		const std::string &serialNumberRestrict) {
		handle = caerDeviceOpen(deviceID, deviceType, busNumberRestrict, devAddressRestrict,
			(serialNumberRestrict.empty()) ? (nullptr) : (serialNumberRestrict.c_str()));

		// Handle constructor failure.
		if (handle == nullptr) {
			throw std::runtime_error("Failed to open device.");
		}
	}

public:
	// This can be called from base class pointers!
	~usb() {
		// Run destructors, free all memory.
		// Never fails in current implementation.
		caerDeviceClose(&handle);
	}

	void sendDefaultConfig() const {
		bool success = caerDeviceSendDefaultConfig(handle);
		if (!success) {
			throw std::runtime_error("Failed to send default configuration.");
		}
	}

	void configSet(int8_t modAddr, uint8_t paramAddr, uint32_t param) const {
		bool success = caerDeviceConfigSet(handle, modAddr, paramAddr, param);
		if (!success) {
			throw std::runtime_error("Failed to set configuration parameter.");
		}
	}

	void configGet(int8_t modAddr, uint8_t paramAddr, uint32_t *param) const {
		bool success = caerDeviceConfigGet(handle, modAddr, paramAddr, param);
		if (!success) {
			throw std::runtime_error("Failed to get configuration parameter.");
		}
	}

	uint32_t configGet(int8_t modAddr, uint8_t paramAddr) const {
		uint32_t param = 0;
		configGet(modAddr, paramAddr, &param);
		return (param);
	}

	void dataStart() const {
		dataStart(nullptr, nullptr, nullptr, nullptr, nullptr);
	}

	void dataStart(void (*dataNotifyIncrease)(void *ptr), void (*dataNotifyDecrease)(void *ptr),
		void *dataNotifyUserPtr, void (*dataShutdownNotify)(void *ptr), void *dataShutdownUserPtr) const {
		bool success = caerDeviceDataStart(handle, dataNotifyIncrease, dataNotifyDecrease, dataNotifyUserPtr,
			dataShutdownNotify, dataShutdownUserPtr);
		if (!success) {
			throw std::runtime_error("Failed to start getting data.");
		}
	}

	void dataStop() const {
		bool success = caerDeviceDataStop(handle);
		if (!success) {
			throw std::runtime_error("Failed to stop getting data.");
		}
	}

	libcaer::events::EventPacketContainer *dataGet() const {
		caerEventPacketContainer cContainer = caerDeviceDataGet(handle);
		if (cContainer == nullptr) {
			// NULL return means no data, forward that.
			return (nullptr);
		}

		libcaer::events::EventPacketContainer *cppContainer = new libcaer::events::EventPacketContainer();

		for (int32_t i = 0; i < caerEventPacketContainerGetEventPacketsNumber(cContainer); i++) {
			caerEventPacketHeader packet = caerEventPacketContainerGetEventPacket(cContainer, i);

			// NULL packets just get added directly.
			if (packet == nullptr) {
				cppContainer->add(nullptr);
			}
			else {
				// Make sure the proper constructors are called when building the shared_ptr.
				cppContainer->add(libcaer::events::utils::constructFromCStruct(packet));
			}
		}

		// Free original C container. The event packet memory is now managed by
		// the EventPacket classes inside the new C++ EventPacketContainer.
		free(cContainer);

		return (cppContainer);
	}
};

}
}

#endif /* LIBCAER_DEVICES_USB_HPP_ */
