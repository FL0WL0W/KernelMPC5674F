#include "MPC5674FCANService.h"

using namespace EmbeddedIOServices;
using namespace MPC5674F;

extern "C" int main(void) {
    auto canService = new MPC5674FCANService(CANBaudRate::Kbps500, CANBaudRate::Disabled, CANBaudRate::Disabled, CANBaudRate::Disabled, false);
    canService->Send({0x7E8, 0}, {{0x01, 0x99}}, 2);
    while(true) {
        
    }
    return 0;
}