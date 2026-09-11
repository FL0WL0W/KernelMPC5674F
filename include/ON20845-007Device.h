#ifndef ON20845_007DEVICE_H
#define ON20845_007DEVICE_H

#include "ISPIService.h"

#include <cstdint>

namespace E92
{
	class ON20845_007Device final
	{
	private:
		EmbeddedIOServices::ISPIService& _service;
		bool SendCommand(std::uint8_t first, std::uint8_t second);

	public:
		explicit ON20845_007Device(EmbeddedIOServices::ISPIService& service)
			: _service(service) {}

		void ServiceWatchdog();
		void SendOutputConfiguration();
		void SendGroup5Base();
		void SendGroup5Enabled();
		void SendGroup4();
		void SendGroup6(std::uint8_t control);
	};
}

#endif
