#include "ON20845-007Device.h"

#include <cstddef>

namespace E92
{
	bool ON20845_007Device::SendCommand(
		std::uint8_t first,
		std::uint8_t second)
	{
		std::uint8_t command[] = {first, second};
		return _service.Transfer(command, sizeof(command), nullptr);
	}

	void ON20845_007Device::ServiceWatchdog()
	{
		// The periodic watchdog is best-effort and non-blocking.
		if (!_service.Ready())
			return;

		static std::uint16_t message[] = {
			0x6AA4U, 0xA1F0U, 0x0000U,
			0x6944U, 0xA1F0U, 0x0000U,
		};

		_service.Transfer(
			reinterpret_cast< std::uint8_t *>(message),
			6,
			nullptr);
		_service.Transfer(
			reinterpret_cast< std::uint8_t *>(message) + 6,
			6,
			nullptr);

	}

	void ON20845_007Device::SendGroup5Base()
	{
		SendCommand(0x6AU, 0x0CU);
	}

	void ON20845_007Device::SendGroup5Enabled()
	{
		SendCommand(0x6AU, 0x2CU);
	}

	void ON20845_007Device::SendGroup4()
	{
		SendCommand(0x00U, 0x00U);
	}

	void ON20845_007Device::SendGroup6(std::uint8_t control)
	{
		SendCommand(0x80U, control);
	}

	void ON20845_007Device::SendOutputConfiguration()
	{
		SendGroup6(0xFCU);
	}
}
