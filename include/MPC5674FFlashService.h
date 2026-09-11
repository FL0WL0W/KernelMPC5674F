#ifndef MPC5674F_FLASH_SERVICE_H
#define MPC5674F_FLASH_SERVICE_H

#include "ICommunicationService.h"
#include "MPC5xxx.h"
#include "SHA256.h"
#include "UDSService.h"

#include <cstddef>
#include <cstdint>

namespace E92
{
	class MPC5674FFlashService final
	{
	public:
		bool HandleRoutineControl(
			std::uint8_t subFunction,
			std::uint16_t routineIdentifier,
			const std::uint8_t* optionRecord,
			std::size_t optionRecordLength,
			const EmbeddedIOServices::communication_send_callback_t& send);

		// QueueWrite copies data before returning. One request may be active and
		// one may wait behind it, allowing UDS TransferData to remain pipelined.
		bool QueueWrite(
			std::uint32_t address,
			const std::uint8_t* data,
			std::size_t length,
			UDSFlashWriteCompletion completion);

		// Service performs at most one short state-machine step. It must be called
		// continuously, including while ISO-TP is transmitting a response.
		void Service(std::uint32_t now, bool communicationReady = true);

		bool Ready() const;

	private:
		static constexpr std::uint16_t InventoryRoutineIdentifier = 0xFF00U;
		static constexpr std::uint16_t EraseRoutineIdentifier = 0xFF01U;
		static constexpr std::size_t BlockCount = 20U;
		static constexpr std::size_t MaximumWriteLength = 4096U;
		static constexpr std::size_t HashLength = 32U;
		static constexpr std::size_t BlockRecordLength = 9U + HashLength;
		static constexpr std::size_t InventoryResponseLength = 5U + BlockRecordLength;
		static constexpr std::size_t HashBytesPerService = 1024U;
		static constexpr std::uint32_t StatusPeriodTicks = 16000000U;

		enum class EraseStatus : std::uint8_t
		{
			Queued = 0U,
			Running = 1U,
			Successful = 2U,
			Failed = 3U,
		};

		enum class Operation : std::uint8_t
		{
			Idle,
			EraseStart,
			EraseWait,
			ProgramStart,
			ProgramWait,
		};

		enum class CacheStatus : std::uint8_t
		{
			NeedsHash,
			ErasedAwaitingRewrite,
			Valid,
		};

		enum class Controller : std::uint8_t
		{
			None,
			A,
			B,
		};

		struct EraseRequest
		{
			std::uint8_t Block = 0U;
			std::uint32_t Sequence = 0U;
			EmbeddedIOServices::communication_send_callback_t Send;
		};

		struct ProgramRequest
		{
			std::uint32_t Address = 0U;
			std::size_t Length = 0U;
			std::uint8_t* Data = nullptr;
			std::uint32_t Sequence = 0U;
			UDSFlashWriteCompletion Completion;
		};

		struct ControllerSnapshot
		{
			std::uint32_t BIUCR = 0U;
			std::uint32_t LMLR = 0U;
			std::uint32_t SLMLR = 0U;
			std::uint32_t HLR = 0U;
			bool Valid = false;
		};

		Operation _operation = Operation::Idle;
		Controller _eraseController = Controller::None;
		Controller _programController = Controller::None;
		std::uint8_t _erasePhase = 0U;
		std::uint8_t _eraseBlock = 0U;
		std::uint8_t _programBlock = 0U;
		std::size_t _programOffset = 0U;
		std::size_t _programNextOffset = 0U;
		std::uint32_t _lastStatusTime = 0U;
		std::uint32_t _nextOperationSequence = 0U;

		ControllerSnapshot _controllerSnapshot;
		EraseRequest _eraseQueue[BlockCount];
		std::size_t _eraseQueueHead = 0U;
		std::size_t _eraseQueueCount = 0U;
		ProgramRequest _activeProgram;
		ProgramRequest _queuedProgram;

		Kernel::SHA256 _sha256;
		std::uint8_t _hashCache[BlockCount][HashLength] = {};
		CacheStatus _cacheStatus[BlockCount] = {};
		std::uint32_t _rewriteThrough[BlockCount] = {};
		bool _hashActive = false;
		std::uint8_t _hashBlock = 0U;
		std::uint32_t _hashOffset = 0U;

		EmbeddedIOServices::communication_send_callback_t _inventorySend;
		std::uint8_t _inventoryResponse[InventoryResponseLength] = {};
		bool _inventoryActive = false;
		bool _communicationReady = true;
		std::uint8_t _inventoryBlock = 0U;

		void SendNegative(
			const EmbeddedIOServices::communication_send_callback_t& send,
			std::uint8_t code) const;
		void SendEraseStatus(
			const EmbeddedIOServices::communication_send_callback_t& send,
			std::uint8_t block,
			EraseStatus status) const;

		void QueueErase(
			std::uint8_t block,
			const EmbeddedIOServices::communication_send_callback_t& send);
		void BeginErase(std::uint32_t now);
		void StartErasePhase();
		void WaitForErase();
		void CompleteErase(EraseStatus status);

		void BeginProgram();
		void StartProgramPage();
		void WaitForProgram();
		void CompleteProgram(bool successful);

		bool OpenController(Controller controller, std::uint8_t block);
		void CloseController(bool programming);
		static Controller ControllerForAddress(std::uint32_t address);
		static volatile struct FLASH_tag& FlashController(Controller controller);

		void StartInventory();
		void ServiceInventory();
		void StartHash(std::uint8_t block);
		void ServiceHash();
		void ServiceBackgroundHash();
		void CancelHash(std::uint8_t block);
		void MarkEraseResult(std::uint8_t block, bool successful);
		void MarkProgramResult(bool successful);
	};
}

#endif
