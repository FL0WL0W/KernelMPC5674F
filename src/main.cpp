#include "MPC5674FCANService.h"
#include "MPC5674F.h"

using namespace EmbeddedIOServices;
using namespace MPC5674F;


namespace {

constexpr std::uint32_t kDSPIDModuleConfiguration = 0x813F1900U;
constexpr std::uint32_t kDSPIDCTAR1Configuration = 0x78175561U;
constexpr std::uint32_t kDSPIPCS0 = 0x00010000U;
constexpr std::uint32_t kDSPICTAR1 = 0x10000000U;
constexpr std::uint32_t kDSPIContinuousPCS = 0x80000000U;
constexpr std::uint32_t kDSPIRxFifoDrainFlag = 0x00020000U;

// The bootloader services the companion from a 12.5 ms eMIOS interrupt.
// Run STM at 1 MHz (256 MHz system clock / 256) and poll it from main instead.
constexpr std::uint32_t kSTMConfiguration1MHz = 0x0000FF01U;
constexpr std::uint32_t kCompanionServicePeriodTicks = 12500U;

void InitializeCompanionDSPI() {
    // Match the DSPI-D setup used by the bootloader. The bootloader has already
    // configured the DSPI-D pads before handing control to this image.
    DSPI_D.MCR.R = kDSPIDModuleConfiguration;
    DSPI_D.TCR.R &= 0x0000FFFFU;
    DSPI_D.RSER.R = kDSPIRxFifoDrainFlag;
    DSPI_D.CTAR[0].R = 0x00000000U;
    DSPI_D.CTAR[1].R = kDSPIDCTAR1Configuration;
    DSPI_D.CTAR[2].R = 0x3AFC3879U;
    DSPI_D.CTAR[3].R = 0x3ADC3B79U;
    DSPI_D.CTAR[4].R = 0x3AEC3C09U;
    DSPI_D.SR.R = 0x90020000U;
}

std::uint16_t TransferCompanionWord(std::uint16_t word, bool keepPCSAsserted) {
    // RFDF is write-one-to-clear. Clear stale receive data indication before
    // starting the next frame, then wait for the matching received frame.
    DSPI_D.SR.R = kDSPIRxFifoDrainFlag;
    DSPI_D.PUSHR.R = kDSPICTAR1 | kDSPIPCS0 |
                     (keepPCSAsserted ? kDSPIContinuousPCS : 0U) | word;

    while ((DSPI_D.SR.R & kDSPIRxFifoDrainFlag) == 0U) {}
    return static_cast<std::uint16_t>(DSPI_D.POPR.R);
}

void ServiceCompanionWatchdog() {
    // Same two three-word commands emitted by the bootloader. PCS0 remains
    // asserted within each command and is released between the commands.
    static constexpr std::uint16_t message[] = {
        0x6AA4U, 0xA1F0U, 0x0000U,
        0x6944U, 0xA1F0U, 0x0000U,
    };

    volatile std::uint16_t response;
    for (std::uint32_t i = 0; i < 6U; ++i) {
        response = TransferCompanionWord(message[i], (i % 3U) != 2U);
    }
    (void)response;
}

} // namespace

volatile uint8_t emiosHits = 0;

inline void ServiceCoreWatchdog()
{
    const std::uint32_t watchdogService = 0x40000000U;

    asm volatile(
        "isync\n"
        "mtspr 336, %0\n"
        "isync\n"
        :
        : "r"(watchdogService)
        : "memory"
    );
}

extern "C" void EMIOS_11_Handler()
{
    // CSR.FLAG is write-one-to-clear. Clear the interrupt source before
    // completing the handler and writing INTC.EOIR in the assembly wrapper.
    EMIOS.CH[11].CSR.R = 1U;
    ServiceCoreWatchdog();
    ServiceCompanionWatchdog();
    emiosHits++;
}

extern "C" int main(void) 
{
    InitializeCompanionDSPI();
    asm("wrteei 1");
    auto canService = new MPC5674FCANService(CANBaudRate::Kbps500, CANBaudRate::Disabled, CANBaudRate::Disabled, CANBaudRate::Disabled, false);
    canService->Send({0x7E8, 0}, {{0x01, 0x99}}, 2);
    while(true) 
    {
        for(volatile int i = 0; i < 10000; i++) ;

    }
    return 0;
}
