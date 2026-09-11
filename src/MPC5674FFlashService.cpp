#include "MPC5674FFlashService.h"

#include <new>

namespace
{
	enum class BlockArea : std::uint8_t
	{
		ALow,
		AMid,
		BLow,
		BMid,
		High,
	};

	struct FlashBlock
	{
		std::uint32_t Address;
		std::uint32_t Length;
		BlockArea Area;
		std::uint8_t HardwareIndex;
	};

	// The high blocks are logical 512 KiB blocks. Each is composed of a
	// 256 KiB block in Flash A and a 256 KiB block in Flash B, interleaved in
	// 16-byte halves of each 32-byte logical line.
	constexpr FlashBlock FlashBlocks[] = {
		{0x00000000U, 0x00004000U, BlockArea::ALow, 0U},
		{0x00004000U, 0x00004000U, BlockArea::ALow, 1U},
		{0x00008000U, 0x00004000U, BlockArea::ALow, 2U},
		{0x0000C000U, 0x00004000U, BlockArea::ALow, 3U},
		{0x00010000U, 0x00004000U, BlockArea::ALow, 4U},
		{0x00014000U, 0x00004000U, BlockArea::ALow, 5U},
		{0x00018000U, 0x00004000U, BlockArea::ALow, 6U},
		{0x0001C000U, 0x00004000U, BlockArea::ALow, 7U},
		{0x00020000U, 0x00010000U, BlockArea::ALow, 8U},
		{0x00030000U, 0x00010000U, BlockArea::ALow, 9U},
		{0x00040000U, 0x00020000U, BlockArea::AMid, 0U},
		{0x00060000U, 0x00020000U, BlockArea::AMid, 1U},
		{0x00080000U, 0x00040000U, BlockArea::BLow, 0U},
		{0x000C0000U, 0x00040000U, BlockArea::BMid, 0U},
		{0x00100000U, 0x00080000U, BlockArea::High, 0U},
		{0x00180000U, 0x00080000U, BlockArea::High, 1U},
		{0x00200000U, 0x00080000U, BlockArea::High, 2U},
		{0x00280000U, 0x00080000U, BlockArea::High, 3U},
		{0x00300000U, 0x00080000U, BlockArea::High, 4U},
		{0x00380000U, 0x00080000U, BlockArea::High, 5U},
	};

	constexpr std::size_t FlashBlockCount =
		sizeof(FlashBlocks) / sizeof(FlashBlocks[0]);
	static_assert(FlashBlockCount == 20U, "Unexpected MPC5674F block count");

	constexpr std::uint32_t FlashEnd = 0x00400000U;
	constexpr std::uint32_t LowMidLockPassword = 0xA1A11111U;
	constexpr std::uint32_t SecondaryLockPassword = 0xC3C33333U;
	constexpr std::uint32_t HighLockPassword = 0xB2B22222U;
	constexpr std::uint32_t ProgramPageSize = 16U;
	constexpr std::uint32_t ECCSegmentSize = 8U;

	// These factory/reserved doublewords are known to be unreadable when their
	// ECC syndrome has never been programmed. Hash them as erased without ever
	// issuing a flash-bus read.
	constexpr std::uint32_t InvalidECCAddresses[] = {
		0x0001FFF8U,
		0x0002FFF8U,
	};

	bool IsInvalidECCAddress(std::uint32_t address)
	{
		for (std::uint32_t invalid : InvalidECCAddresses)
		{
			if (address >= invalid && address < invalid + ECCSegmentSize)
				return true;
		}
		return false;
	}

	void Synchronize()
	{
		asm volatile("msync" ::: "memory");
	}

	void WriteBigEndian32(std::uint8_t* destination, std::uint32_t value)
	{
		destination[0] = static_cast<std::uint8_t>(value >> 24U);
		destination[1] = static_cast<std::uint8_t>(value >> 16U);
		destination[2] = static_cast<std::uint8_t>(value >> 8U);
		destination[3] = static_cast<std::uint8_t>(value);
	}

	std::uint32_t ReadBigEndian32(const std::uint8_t* source)
	{
		return (static_cast<std::uint32_t>(source[0]) << 24U) |
			(static_cast<std::uint32_t>(source[1]) << 16U) |
			(static_cast<std::uint32_t>(source[2]) << 8U) |
			static_cast<std::uint32_t>(source[3]);
	}

	std::size_t FindBlock(std::uint32_t address)
	{
		for (std::size_t i = 0U; i < FlashBlockCount; ++i)
		{
			const FlashBlock& block = FlashBlocks[i];
			if (address >= block.Address &&
				address < block.Address + block.Length)
				return i;
		}
		return FlashBlockCount;
	}

	bool IsFlashRange(std::uint32_t address, std::size_t length)
	{
		if (length == 0U || length > 0xFFFFFFFFU)
			return false;
		const std::uint32_t end =
			address + static_cast<std::uint32_t>(length);
		return address < FlashEnd && end > address && end <= FlashEnd;
	}

	bool ControllerIdle(volatile struct FLASH_tag& flash)
	{
		return flash.MCR.B.PGM == 0U && flash.MCR.B.ERS == 0U &&
			flash.MCR.B.EHV == 0U && flash.MCR.B.DONE != 0U;
	}

	void RestoreLocks(volatile struct FLASH_tag& flash,
		std::uint32_t lmlr, std::uint32_t slmlr, std::uint32_t hlr)
	{
		flash.LMLR.B.SLOCK = (lmlr >> 20U) & 1U;
		flash.LMLR.B.MLOCK = (lmlr >> 16U) & 3U;
		flash.LMLR.B.LLOCK = lmlr & 0x3FFU;
		flash.SLMLR.B.SSLOCK = (slmlr >> 20U) & 1U;
		flash.SLMLR.B.SMLOCK = (slmlr >> 16U) & 3U;
		flash.SLMLR.B.SLLOCK = slmlr & 0x3FFU;
		flash.HLR.B.HBLOCK = hlr & 0x3FU;
	}

	bool SequenceBefore(std::uint32_t left, std::uint32_t right)
	{
		return static_cast<std::int32_t>(left - right) < 0;
	}
}

namespace E92
{
	MPC5674FFlashService::Controller
	MPC5674FFlashService::ControllerForAddress(std::uint32_t address)
	{
		if (address < 0x00080000U)
			return Controller::A;
		if (address < 0x00100000U)
			return Controller::B;
		return (address & 0x10U) == 0U ? Controller::A : Controller::B;
	}

	volatile struct FLASH_tag& MPC5674FFlashService::FlashController(
		Controller controller)
	{
		return controller == Controller::A ? FLASH_A : FLASH_B;
	}

	void MPC5674FFlashService::SendNegative(
		const EmbeddedIOServices::communication_send_callback_t& send,
		std::uint8_t code) const
	{
		const std::uint8_t response[] = {0x7FU, 0x31U, code};
		send(response, sizeof(response));
	}

	void MPC5674FFlashService::SendEraseStatus(
		const EmbeddedIOServices::communication_send_callback_t& send,
		std::uint8_t block,
		EraseStatus status) const
	{
		const std::uint8_t response[] = {
			0x71U,
			0x01U,
			static_cast<std::uint8_t>(EraseRoutineIdentifier >> 8U),
			static_cast<std::uint8_t>(EraseRoutineIdentifier),
			block,
			static_cast<std::uint8_t>(status),
		};
		send(response, sizeof(response));
	}

	void MPC5674FFlashService::QueueErase(
		std::uint8_t block,
		const EmbeddedIOServices::communication_send_callback_t& send)
	{
		// Treat a repeated request as a status query. This makes retrying a lost
		// UDS response safe without erasing the same block twice.
		for (std::size_t offset = 0U; offset < _eraseQueueCount; ++offset)
		{
			const std::size_t index = (_eraseQueueHead + offset) % BlockCount;
			if (_eraseQueue[index].Block != block)
				continue;
			const bool running = offset == 0U &&
				(_operation == Operation::EraseStart ||
				 _operation == Operation::EraseWait);
			SendEraseStatus(send, block,
				running ? EraseStatus::Running : EraseStatus::Queued);
			return;
		}

		if (_eraseQueueCount == BlockCount)
		{
			SendEraseStatus(send, block, EraseStatus::Failed);
			return;
		}

		const std::size_t tail =
			(_eraseQueueHead + _eraseQueueCount) % BlockCount;
		_eraseQueue[tail].Block = block;
		_eraseQueue[tail].Sequence = _nextOperationSequence++;
		_eraseQueue[tail].Send = send;
		++_eraseQueueCount;
		SendEraseStatus(send, block, EraseStatus::Queued);
	}

	void MPC5674FFlashService::BeginErase(std::uint32_t now)
	{
		_eraseBlock = _eraseQueue[_eraseQueueHead].Block;
		_lastStatusTime = now;
		CancelHash(_eraseBlock);
		SendEraseStatus(_eraseQueue[_eraseQueueHead].Send,
			_eraseBlock, EraseStatus::Running);
		_operation = Operation::EraseStart;
	}

	bool MPC5674FFlashService::OpenController(
		Controller controller, std::uint8_t blockIndex)
	{
		ControllerSnapshot& snapshot =
			_controllerSnapshots[controller == Controller::A ? 0U : 1U];
		if (controller == Controller::None || blockIndex >= BlockCount ||
			snapshot.Valid)
			return false;

		volatile struct FLASH_tag& flash = FlashController(controller);
		if (!ControllerIdle(flash))
			return false;

		const FlashBlock& block = FlashBlocks[blockIndex];
		snapshot.BIUCR = flash.BIUCR.R;
		snapshot.LMLR = flash.LMLR.R;
		snapshot.SLMLR = flash.SLMLR.R;
		snapshot.HLR = flash.HLR.R;
		snapshot.Valid = true;

		flash.BIUCR.B.IPFEN = 0U;
		flash.BIUCR.B.DPFEN = 0U;
		Synchronize();

		if (block.Area == BlockArea::High)
		{
			flash.HLR.R = HighLockPassword;
			flash.HLR.B.HBLOCK &= ~(1UL << block.HardwareIndex);
			if ((flash.HLR.B.HBLOCK & (1UL << block.HardwareIndex)) != 0U)
				return false;
		}
		else
		{
			flash.LMLR.R = LowMidLockPassword;
			flash.SLMLR.R = SecondaryLockPassword;
			if (block.Area == BlockArea::ALow || block.Area == BlockArea::BLow)
			{
				flash.LMLR.B.LLOCK &= ~(1UL << block.HardwareIndex);
				flash.SLMLR.B.SLLOCK &= ~(1UL << block.HardwareIndex);
				if ((flash.LMLR.B.LLOCK & (1UL << block.HardwareIndex)) != 0U ||
					(flash.SLMLR.B.SLLOCK & (1UL << block.HardwareIndex)) != 0U)
					return false;
			}
			else
			{
				flash.LMLR.B.MLOCK &= ~(1UL << block.HardwareIndex);
				flash.SLMLR.B.SMLOCK &= ~(1UL << block.HardwareIndex);
				if ((flash.LMLR.B.MLOCK & (1UL << block.HardwareIndex)) != 0U ||
					(flash.SLMLR.B.SMLOCK & (1UL << block.HardwareIndex)) != 0U)
					return false;
			}
		}
		Synchronize();
		return true;
	}

	void MPC5674FFlashService::CloseController(
		Controller controller, bool programming)
	{
		if (controller == Controller::None)
			return;
		ControllerSnapshot& snapshot =
			_controllerSnapshots[controller == Controller::A ? 0U : 1U];
		if (!snapshot.Valid)
		{
			if (programming && _programController == controller)
				_programController = Controller::None;
			else if (!programming && _eraseController == controller)
				_eraseController = Controller::None;
			return;
		}

		volatile struct FLASH_tag& flash = FlashController(controller);
		flash.MCR.B.EHV = 0U;
		Synchronize();
		if (programming)
			flash.MCR.B.PGM = 0U;
		else
		{
			flash.LMSR.R = 0U;
			flash.HSR.R = 0U;
			flash.MCR.B.ERS = 0U;
		}
		Synchronize();

		RestoreLocks(flash,
			snapshot.LMLR,
			snapshot.SLMLR,
			snapshot.HLR);
		// Toggling BFEN invalidates stale line-buffer contents before restoring
		// the original prefetch configuration.
		flash.BIUCR.B.BFEN = 0U;
		Synchronize();
		flash.BIUCR.R = snapshot.BIUCR;
		Synchronize();

		snapshot = ControllerSnapshot();
		if (programming && _programController == controller)
			_programController = Controller::None;
		else if (!programming && _eraseController == controller)
			_eraseController = Controller::None;
	}

	bool MPC5674FFlashService::PrepareEraseController(Controller controller)
	{
		const FlashBlock& block = FlashBlocks[_eraseBlock];
		if (!OpenController(controller, _eraseBlock))
			return false;

		volatile struct FLASH_tag& flash = FlashController(controller);
		flash.MCR.B.ERS = 1U;
		if (block.Area == BlockArea::High)
			flash.HSR.B.HBSEL = 1UL << block.HardwareIndex;
		else if (block.Area == BlockArea::ALow || block.Area == BlockArea::BLow)
			flash.LMSR.B.LSEL = 1UL << block.HardwareIndex;
		else
			flash.LMSR.B.MSEL = 1UL << block.HardwareIndex;

		// Bit 4 selects the physical half of an interleaved high-flash line.
		const std::uint32_t interlockAddress = block.Address +
			((block.Area == BlockArea::High &&
			  controller == Controller::B) ? 0x10U : 0U);
		*reinterpret_cast<volatile std::uint32_t*>(interlockAddress) = 0xFFFFFFFFU;
		Synchronize();
		return true;
	}

	void MPC5674FFlashService::StartErasePhase()
	{
		const FlashBlock& block = FlashBlocks[_eraseBlock];
		if (block.Area == BlockArea::High)
		{
			_eraseController = Controller::None;
			if (!PrepareEraseController(Controller::A) ||
				!PrepareEraseController(Controller::B))
			{
				CompleteErase(EraseStatus::Failed);
				return;
			}

			// Both controllers are completely configured before either internal
			// erase algorithm begins. The writes are only a few peripheral cycles
			// apart, so the two 256 KiB physical halves erase concurrently.
			FLASH_A.MCR.B.EHV = 1U;
			Synchronize();
			FLASH_B.MCR.B.EHV = 1U;
			Synchronize();
		}
		else
		{
			_eraseController =
				(block.Area == BlockArea::ALow || block.Area == BlockArea::AMid)
				? Controller::A : Controller::B;
			if (!PrepareEraseController(_eraseController))
			{
				CompleteErase(EraseStatus::Failed);
				return;
			}
			FlashController(_eraseController).MCR.B.EHV = 1U;
			Synchronize();
		}
		_operation = Operation::EraseWait;
	}

	void MPC5674FFlashService::WaitForErase()
	{
		if (FlashBlocks[_eraseBlock].Area == BlockArea::High)
		{
			if (FLASH_A.MCR.B.DONE == 0U || FLASH_B.MCR.B.DONE == 0U)
				return;

			const bool successful =
				FLASH_A.MCR.B.PEG != 0U && FLASH_B.MCR.B.PEG != 0U;
			CloseController(Controller::A, false);
			CloseController(Controller::B, false);
			CompleteErase(successful
				? EraseStatus::Successful : EraseStatus::Failed);
			return;
		}

		volatile struct FLASH_tag& flash = FlashController(_eraseController);
		if (flash.MCR.B.DONE == 0U)
			return;

		const bool successful = flash.MCR.B.PEG != 0U;
		CloseController(_eraseController, false);
		if (!successful)
		{
			CompleteErase(EraseStatus::Failed);
			return;
		}

		CompleteErase(EraseStatus::Successful);
	}

	void MPC5674FFlashService::CompleteErase(EraseStatus status)
	{
		if (_controllerSnapshots[0].Valid)
			CloseController(Controller::A, false);
		if (_controllerSnapshots[1].Valid)
			CloseController(Controller::B, false);
		const bool successful = status == EraseStatus::Successful;
		MarkEraseResult(_eraseBlock, successful);

		EraseRequest& request = _eraseQueue[_eraseQueueHead];
		SendEraseStatus(request.Send, request.Block, status);
		request = EraseRequest();
		_eraseQueueHead = (_eraseQueueHead + 1U) % BlockCount;
		--_eraseQueueCount;
		_operation = Operation::Idle;
	}

	bool MPC5674FFlashService::QueueWrite(
		std::uint32_t address,
		const std::uint8_t* data,
		std::size_t length,
		UDSFlashWriteCompletion completion)
	{
		if (data == nullptr || !completion || length > MaximumWriteLength ||
			!IsFlashRange(address, length) || (address & 7U) != 0U ||
			(length & 7U) != 0U)
			return false;

		std::uint8_t* const copy = new (std::nothrow) std::uint8_t[length];
		if (copy == nullptr)
			return false;
		for (std::size_t i = 0U; i < length; ++i)
			copy[i] = data[i];

		ProgramRequest request;
		request.Address = address;
		request.Length = length;
		request.Data = copy;
		request.Sequence = _nextOperationSequence++;
		request.Completion = completion;

		if (_activeProgram.Data == nullptr)
			_activeProgram = request;
		else if (_queuedProgram.Data == nullptr)
			_queuedProgram = request;
		else
		{
			delete[] copy;
			return false;
		}
		return true;
	}

	void MPC5674FFlashService::BeginProgram()
	{
		_programOffset = 0U;
		_programNextOffset = 0U;
		_programBlock = static_cast<std::uint8_t>(FindBlock(_activeProgram.Address));
		_lastStatusTime = 0U;
		_operation = _programBlock < BlockCount
			? Operation::ProgramStart : Operation::Idle;
		if (_programBlock >= BlockCount)
			CompleteProgram(false);
	}

	void MPC5674FFlashService::StartProgramPage()
	{
		const std::uint32_t address = _activeProgram.Address +
			static_cast<std::uint32_t>(_programOffset);
		const std::size_t blockIndex = FindBlock(address);
		if (blockIndex == BlockCount)
		{
			CompleteProgram(false);
			return;
		}
		const bool blockKnownErased =
			_cacheStatus[blockIndex] == CacheStatus::ErasedAwaitingRewrite;

		const Controller targetController = ControllerForAddress(address);
		if (_programController != targetController || _programBlock != blockIndex)
		{
			if (_programController != Controller::None)
				CloseController(_programController, true);
			_programController = targetController;
			_programBlock = static_cast<std::uint8_t>(blockIndex);
			if (!OpenController(_programController, _programBlock))
			{
				CompleteProgram(false);
				return;
			}
		}

		const std::uint32_t pageEnd =
			(address & ~(ProgramPageSize - 1U)) + ProgramPageSize;
		const std::size_t remaining = _activeProgram.Length - _programOffset;
		const std::size_t pageLength = remaining < pageEnd - address
			? remaining : pageEnd - address;
		std::uint8_t programMask = 0U;

		for (std::size_t offset = 0U; offset < pageLength; offset += ECCSegmentSize)
		{
			const std::uint8_t* const desired =
				_activeProgram.Data + _programOffset + offset;
			bool allFF = true;
			for (std::size_t byte = 0U; byte < ECCSegmentSize; ++byte)
				allFF = allFF && desired[byte] == 0xFFU;

			const bool segmentKnownErased = blockKnownErased &&
				address + offset >= _rewriteThrough[blockIndex];
			if (segmentKnownErased)
			{
				if (!allFF)
					programMask |= static_cast<std::uint8_t>(
						1U << (offset / ECCSegmentSize));
				continue;
			}

			// Without a recorded erase, programming the same 64-bit ECC segment
			// again is unsafe even if the visible data only changes from 1 to 0.
			// Accept an exact match as a no-op; otherwise require an erase first.
			if (IsInvalidECCAddress(address + offset))
			{
				if (!allFF)
				{
					CompleteProgram(false);
					return;
				}
				continue;
			}

			const volatile std::uint8_t* const current =
				reinterpret_cast<const volatile std::uint8_t*>(address + offset);
			bool equal = true;
			for (std::size_t byte = 0U; byte < ECCSegmentSize; ++byte)
				equal = equal && current[byte] == desired[byte];
			if (!equal)
			{
				CompleteProgram(false);
				return;
			}
		}

		_programNextOffset = _programOffset + pageLength;
		if (programMask == 0U)
		{
			_programOffset = _programNextOffset;
			if (_programOffset == _activeProgram.Length)
				CompleteProgram(true);
			return;
		}

		volatile struct FLASH_tag& flash = FlashController(_programController);
		flash.MCR.B.PGM = 1U;
		Synchronize();
		for (std::size_t offset = 0U; offset < pageLength; offset += ECCSegmentSize)
		{
			if ((programMask & (1U << (offset / ECCSegmentSize))) == 0U)
				continue;
			const std::uint8_t* const desired =
				_activeProgram.Data + _programOffset + offset;
			volatile std::uint32_t* const destination =
				reinterpret_cast<volatile std::uint32_t*>(address + offset);
			destination[0] = ReadBigEndian32(desired);
			destination[1] = ReadBigEndian32(desired + 4U);
		}
		Synchronize();
		flash.MCR.B.EHV = 1U;
		Synchronize();
		_operation = Operation::ProgramWait;
	}

	void MPC5674FFlashService::WaitForProgram()
	{
		volatile struct FLASH_tag& flash = FlashController(_programController);
		if (flash.MCR.B.DONE == 0U)
			return;
		const bool successful = flash.MCR.B.PEG != 0U;
		flash.MCR.B.EHV = 0U;
		Synchronize();
		flash.MCR.B.PGM = 0U;
		Synchronize();
		if (!successful)
		{
			CompleteProgram(false);
			return;
		}

		_programOffset = _programNextOffset;
		if (_programOffset == _activeProgram.Length)
			CompleteProgram(true);
		else
			_operation = Operation::ProgramStart;
	}

	void MPC5674FFlashService::CompleteProgram(bool successful)
	{
		if (_programController != Controller::None)
			CloseController(_programController, true);
		MarkProgramResult(successful);

		UDSFlashWriteCompletion completion = _activeProgram.Completion;
		delete[] _activeProgram.Data;
		_activeProgram = _queuedProgram;
		_queuedProgram = ProgramRequest();
		_operation = Operation::Idle;
		if (completion)
			completion(successful);
	}

	void MPC5674FFlashService::MarkEraseResult(
		std::uint8_t block, bool successful)
	{
		CancelHash(block);
		if (successful)
		{
			_cacheStatus[block] = CacheStatus::ErasedAwaitingRewrite;
			_rewriteThrough[block] = FlashBlocks[block].Address;
		}
		else
		{
			_cacheStatus[block] = CacheStatus::NeedsHash;
		}
	}

	void MPC5674FFlashService::MarkProgramResult(bool successful)
	{
		if (_activeProgram.Data == nullptr || _activeProgram.Length == 0U)
			return;
		const std::uint32_t requestStart = _activeProgram.Address;
		const std::uint32_t requestEnd = requestStart +
			static_cast<std::uint32_t>(_activeProgram.Length);

		for (std::size_t i = 0U; i < BlockCount; ++i)
		{
			const FlashBlock& block = FlashBlocks[i];
			const std::uint32_t blockEnd = block.Address + block.Length;
			const std::uint32_t writeStart = requestStart > block.Address
				? requestStart : block.Address;
			const std::uint32_t writeEnd = requestEnd < blockEnd
				? requestEnd : blockEnd;
			if (writeStart >= writeEnd)
				continue;

			CancelHash(static_cast<std::uint8_t>(i));
			if (!successful)
			{
				_cacheStatus[i] = CacheStatus::NeedsHash;
				continue;
			}
			if (_cacheStatus[i] != CacheStatus::ErasedAwaitingRewrite)
			{
				_cacheStatus[i] = CacheStatus::NeedsHash;
				continue;
			}

			if (writeStart <= _rewriteThrough[i] && writeEnd > _rewriteThrough[i])
				_rewriteThrough[i] = writeEnd;
			if (_rewriteThrough[i] >= blockEnd)
				_cacheStatus[i] = CacheStatus::NeedsHash;
		}
	}

	void MPC5674FFlashService::StartHash(std::uint8_t block)
	{
		_hashBlock = block;
		_hashOffset = 0U;
		_hashActive = true;
		_sha256.Reset();
	}

	void MPC5674FFlashService::CancelHash(std::uint8_t block)
	{
		if (_hashActive && _hashBlock == block)
			_hashActive = false;
	}

	void MPC5674FFlashService::ServiceHash()
	{
		const FlashBlock& block = FlashBlocks[_hashBlock];
		const std::uint32_t remaining = block.Length - _hashOffset;
		const std::uint32_t count = remaining < HashBytesPerService
			? remaining : static_cast<std::uint32_t>(HashBytesPerService);
		std::uint32_t cursor = block.Address + _hashOffset;
		const std::uint32_t end = cursor + count;
		while (cursor < end)
		{
			if (IsInvalidECCAddress(cursor))
			{
				const std::uint8_t erased = 0xFFU;
				_sha256.Update(&erased, 1U);
				++cursor;
				continue;
			}

			std::uint32_t directEnd = end;
			for (std::uint32_t invalid : InvalidECCAddresses)
			{
				if (cursor < invalid && invalid < directEnd)
					directEnd = invalid;
			}
			_sha256.Update(
				reinterpret_cast<const volatile std::uint8_t*>(cursor),
				directEnd - cursor);
			cursor = directEnd;
		}

		_hashOffset += count;
		if (_hashOffset < block.Length)
			return;
		_sha256.Final(_hashCache[_hashBlock]);
		_cacheStatus[_hashBlock] = CacheStatus::Valid;
		_hashActive = false;
	}

	void MPC5674FFlashService::ServiceBackgroundHash()
	{
		if (!_hashActive)
		{
			for (std::size_t block = 0U; block < BlockCount; ++block)
			{
				if (_cacheStatus[block] != CacheStatus::NeedsHash)
					continue;
				StartHash(static_cast<std::uint8_t>(block));
				break;
			}
		}
		if (_hashActive)
			ServiceHash();
	}

	void MPC5674FFlashService::StartInventory()
	{
		_inventoryResponse[0] = 0x71U;
		_inventoryResponse[1] = 0x01U;
		_inventoryResponse[2] =
			static_cast<std::uint8_t>(InventoryRoutineIdentifier >> 8U);
		_inventoryResponse[3] =
			static_cast<std::uint8_t>(InventoryRoutineIdentifier);
		_inventoryResponse[4] = static_cast<std::uint8_t>(BlockCount);
		_inventoryBlock = 0U;
		_lastStatusTime = 0U;
		_inventoryActive = true;

		for (std::size_t block = 0U; block < BlockCount; ++block)
		{
			if (_cacheStatus[block] == CacheStatus::ErasedAwaitingRewrite)
				_cacheStatus[block] = CacheStatus::NeedsHash;
		}
		const std::uint8_t pending[] = {0x7FU, 0x31U, 0x78U};
		_inventorySend(pending, sizeof(pending));
	}

	void MPC5674FFlashService::ServiceInventory()
	{
		if (_cacheStatus[_inventoryBlock] != CacheStatus::Valid)
		{
			if (!_hashActive)
				StartHash(_inventoryBlock);
			ServiceHash();
			return;
		}
		if (!_communicationReady)
			return;

		const FlashBlock& block = FlashBlocks[_inventoryBlock];
		constexpr std::size_t record = 5U;
		_inventoryResponse[record] = _inventoryBlock;
		WriteBigEndian32(_inventoryResponse + record + 1U, block.Address);
		WriteBigEndian32(_inventoryResponse + record + 5U, block.Length);
		for (std::size_t i = 0U; i < HashLength; ++i)
			_inventoryResponse[record + 9U + i] = _hashCache[_inventoryBlock][i];
		_inventorySend(_inventoryResponse, sizeof(_inventoryResponse));

		++_inventoryBlock;
		_lastStatusTime = 0U;
		if (_inventoryBlock == BlockCount)
		{
			_inventorySend = EmbeddedIOServices::communication_send_callback_t();
			_inventoryActive = false;
		}
	}

	bool MPC5674FFlashService::HandleRoutineControl(
		std::uint8_t subFunction,
		std::uint16_t routineIdentifier,
		const std::uint8_t* optionRecord,
		std::size_t optionRecordLength,
		const EmbeddedIOServices::communication_send_callback_t& send)
	{
		if (routineIdentifier != InventoryRoutineIdentifier &&
			routineIdentifier != EraseRoutineIdentifier)
			return false;
		if (subFunction != 0x01U)
		{
			SendNegative(send, 0x12U);
			return true;
		}

		if (routineIdentifier == InventoryRoutineIdentifier)
		{
			if (optionRecordLength != 0U)
			{
				SendNegative(send, 0x13U);
				return true;
			}
			if (_inventoryActive || _operation != Operation::Idle ||
				_eraseQueueCount != 0U || _activeProgram.Data != nullptr)
			{
				SendNegative(send, 0x21U);
				return true;
			}
			_inventorySend = send;
			StartInventory();
			return true;
		}

		if (optionRecord == nullptr || optionRecordLength != 1U ||
			optionRecord[0] >= BlockCount)
		{
			SendNegative(send, optionRecordLength == 1U ? 0x31U : 0x13U);
			return true;
		}
		QueueErase(optionRecord[0], send);
		return true;
	}

	void MPC5674FFlashService::Service(
		std::uint32_t now, bool communicationReady)
	{
		_communicationReady = communicationReady;
		if (_operation == Operation::Idle && !_inventoryActive)
		{
			const bool eraseFirst = _eraseQueueCount != 0U &&
				(_activeProgram.Data == nullptr ||
				 SequenceBefore(_eraseQueue[_eraseQueueHead].Sequence,
					_activeProgram.Sequence));
			if (eraseFirst)
				BeginErase(now);
			else if (_activeProgram.Data != nullptr)
				BeginProgram();
		}

		if (_operation == Operation::EraseStart)
			StartErasePhase();
		else if (_operation == Operation::EraseWait)
			WaitForErase();
		else if (_operation == Operation::ProgramStart)
			StartProgramPage();
		else if (_operation == Operation::ProgramWait)
			WaitForProgram();

		if (_operation == Operation::EraseStart ||
			_operation == Operation::EraseWait)
		{
			if (static_cast<std::uint32_t>(now - _lastStatusTime) >=
				StatusPeriodTicks)
			{
				_lastStatusTime = now;
				SendEraseStatus(_eraseQueue[_eraseQueueHead].Send,
					_eraseQueue[_eraseQueueHead].Block, EraseStatus::Running);
			}
			return;
		}
		if (_operation != Operation::Idle)
			return;

		if (_inventoryActive)
		{
			if (_communicationReady && _lastStatusTime != 0U &&
				static_cast<std::uint32_t>(now - _lastStatusTime) >=
				StatusPeriodTicks)
			{
				_lastStatusTime = now;
				const std::uint8_t pending[] = {0x7FU, 0x31U, 0x78U};
				_inventorySend(pending, sizeof(pending));
				return;
			}
			if (_lastStatusTime == 0U)
				_lastStatusTime = now;
			ServiceInventory();
			return;
		}
		ServiceBackgroundHash();
	}

	bool MPC5674FFlashService::Ready() const
	{
		return _operation == Operation::Idle && _eraseQueueCount == 0U &&
			_activeProgram.Data == nullptr && !_inventoryActive;
	}
}
