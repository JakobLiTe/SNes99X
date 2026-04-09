/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "snes9x.h"
#include "memmap.h"
#include "cpuops.h"
#include "dma.h"
#include "apu/apu.h"
#include "fxemu.h"
#include "snapshot.h"
#include "movie.h"
#include "scheduler.h"
#ifdef DEBUGGER
#include "debug.h"
#include "missing.h"
#endif

static inline void S9xReschedule (void);

// Scheduler event callbacks
static void S9xSchedulerHBlankStart();
static void S9xSchedulerHDMAStart();
static void S9xSchedulerHCounterMax();
static void S9xSchedulerHDMAInit();
static void S9xSchedulerRender();
static void S9xSchedulerWRAMRefresh();

// Initialize horizontal events for the current scanline
static void S9xScheduleHorizontalEvents()
{
	if (!scheduler)
		return;

	// Schedule events for this scanline based on horizontal positions
	// Only schedule events that are in the future
	if (Timings.HBlankStart > CPU.Cycles)
		scheduler->addEventRelative(scheduler->cpuCyclesToMaster(Timings.HBlankStart - CPU.Cycles),
									EVENT_PPU, EVENT_HBLANK_START, S9xSchedulerHBlankStart);

	if (Timings.HDMAStart > CPU.Cycles)
		scheduler->addEventRelative(scheduler->cpuCyclesToMaster(Timings.HDMAStart - CPU.Cycles),
									EVENT_HDMA, EVENT_HDMA_START, S9xSchedulerHDMAStart);

	if (Timings.H_Max > CPU.Cycles)
		scheduler->addEventRelative(scheduler->cpuCyclesToMaster(Timings.H_Max - CPU.Cycles),
									EVENT_PPU, EVENT_HCOUNTER_MAX, S9xSchedulerHCounterMax);

	if (Timings.HDMAInit > CPU.Cycles)
		scheduler->addEventRelative(scheduler->cpuCyclesToMaster(Timings.HDMAInit - CPU.Cycles),
									EVENT_HDMA, EVENT_HDMA_INIT, S9xSchedulerHDMAInit);

	if (Timings.RenderPos > CPU.Cycles)
		scheduler->addEventRelative(scheduler->cpuCyclesToMaster(Timings.RenderPos - CPU.Cycles),
									EVENT_PPU, EVENT_RENDER, S9xSchedulerRender);

	if (Timings.WRAMRefreshPos > CPU.Cycles)
		scheduler->addEventRelative(scheduler->cpuCyclesToMaster(Timings.WRAMRefreshPos - CPU.Cycles),
									EVENT_CPU, EVENT_WRAM_REFRESH, S9xSchedulerWRAMRefresh);
}

/*
 * S9xMainLoop - Main emulation loop
 *
 * The scheduler runs alongside the legacy event system in "observation mode".
 * Callbacks are currently no-ops — the legacy S9xDoHEventProcessing() system
 * still drives all actual emulation events. This avoids double-execution of
 * scanline logic (V-counter increment, HDMA, render) which previously caused
 * CPU.Cycles to drift and trip the deadlock guard.
 *
 * Migration path:
 *   Phase 2 - Replace legacy callbacks one at a time with scheduler versions
 *   Phase 3 - APU port-triggered sync
 *   Phase 4 - Accurate DMA/HDMA cycle stealing
 */
void S9xMainLoop (void)
{
	#define CHECK_FOR_IRQ_CHANGE() \
	if (Timings.IRQFlagChanging) \
	{ \
		if (Timings.IRQFlagChanging & IRQ_TRIGGER_NMI) \
		{ \
			CPU.NMIPending = TRUE; \
			Timings.NMITriggerPos = CPU.Cycles + 6; \
		} \
		if (Timings.IRQFlagChanging & IRQ_CLEAR_FLAG) \
			ClearIRQ(); \
		else if (Timings.IRQFlagChanging & IRQ_SET_FLAG) \
			SetIRQ(); \
		Timings.IRQFlagChanging = IRQ_NONE; \
	}

	// Sync scheduler to current CPU state and register this scanline's events
	if (scheduler)
	{
		scheduler->setMasterClock(scheduler->cpuCyclesToMaster(CPU.Cycles));
		S9xScheduleHorizontalEvents();
	}

	if (CPU.Flags & SCAN_KEYS_FLAG)
	{
		CPU.Flags &= ~SCAN_KEYS_FLAG;
		S9xMovieUpdate();
	}

	for (;;)
	{
		if (CPU.NMIPending)
		{
			#ifdef DEBUGGER
			if (Settings.TraceHCEvent)
			   S9xTraceFormattedMessage ("Comparing %d to %d\n", Timings.NMITriggerPos, CPU.Cycles);
			#endif
			if (Timings.NMITriggerPos <= CPU.Cycles)
			{
				CPU.NMIPending = FALSE;
				Timings.NMITriggerPos = 0xffff;
				if (CPU.WaitingForInterrupt)
				{
					CPU.WaitingForInterrupt = FALSE;
					Registers.PCw++;
					CPU.Cycles += TWO_CYCLES + ONE_DOT_CYCLE / 2;
					while (CPU.Cycles >= CPU.NextEvent)
						S9xDoHEventProcessing();
				}

				CHECK_FOR_IRQ_CHANGE();
				S9xOpcode_NMI();
			}
		}

		if (CPU.Cycles >= Timings.NextIRQTimer)
		{
			#ifdef DEBUGGER
			S9xTraceMessage ("Timer triggered\n");
			#endif

			S9xUpdateIRQPositions(false);
			CPU.IRQLine = TRUE;
		}

		if (CPU.IRQLine || CPU.IRQExternal)
		{
			if (CPU.WaitingForInterrupt)
			{
				CPU.WaitingForInterrupt = FALSE;
				Registers.PCw++;
				CPU.Cycles += TWO_CYCLES + ONE_DOT_CYCLE / 2;
				while (CPU.Cycles >= CPU.NextEvent)
					S9xDoHEventProcessing();
			}

			if (!CheckFlag(IRQ))
			{
				/* The flag pushed onto the stack is the new value */
				CHECK_FOR_IRQ_CHANGE();
				S9xOpcode_IRQ();
			}
		}

		/* Change IRQ flag for instructions that set it only on last cycle */
		CHECK_FOR_IRQ_CHANGE();

	#ifdef DEBUGGER
		if ((CPU.Flags & BREAK_FLAG) && !(CPU.Flags & SINGLE_STEP_FLAG))
		{
			for (int Break = 0; Break != 6; Break++)
			{
				if (S9xBreakpoint[Break].Enabled &&
					S9xBreakpoint[Break].Bank == Registers.PB &&
					S9xBreakpoint[Break].Address == Registers.PCw)
				{
					if (S9xBreakpoint[Break].Enabled == 2)
						S9xBreakpoint[Break].Enabled = TRUE;
					else
						CPU.Flags |= DEBUG_MODE_FLAG;
				}
			}
		}

		if (CPU.Flags & DEBUG_MODE_FLAG)
			break;

		if (CPU.Flags & TRACE_FLAG)
			S9xTrace();

		if (CPU.Flags & SINGLE_STEP_FLAG)
		{
			CPU.Flags &= ~SINGLE_STEP_FLAG;
			CPU.Flags |= DEBUG_MODE_FLAG;
		}
	#endif

		if (CPU.Flags & SCAN_KEYS_FLAG)
		{
			break;
		}

		uint8			Op;
		struct SOpcodes	*Opcodes;

		// Capture CPU cycles before opcode fetch/execute so scheduler tracks
		// the full instruction cost, including memory fetch timing.
		int32 cyclesBefore = CPU.Cycles;

		if (CPU.PCBase)
		{
			Op = CPU.PCBase[Registers.PCw];
			CPU.Cycles += CPU.MemSpeed;
			Opcodes = ICPU.S9xOpcodes;
		}
		else
		{
			Op = S9xGetByte(Registers.PBPC);
			OpenBus = Op;
			Opcodes = S9xOpcodesSlow;
		}

		if ((Registers.PCw & MEMMAP_MASK) + ICPU.S9xOpLengths[Op] >= MEMMAP_BLOCK_SIZE)
		{
			uint8	*oldPCBase = CPU.PCBase;

			CPU.PCBase = S9xGetBasePointer(ICPU.ShiftedPB + ((uint16) (Registers.PCw + 4)));
			if (oldPCBase != CPU.PCBase || (Registers.PCw & ~MEMMAP_MASK) == (0xffff & ~MEMMAP_MASK))
				Opcodes = S9xOpcodesSlow;
		}

		Registers.PCw++;

		// Execute the instruction
		(*Opcodes[Op].S9xOpcode)();

		// Advance scheduler by exact master clock cycles used
		// The legacy event system (CPU.NextEvent / S9xDoHEventProcessing) still
		// drives all actual scanline logic. The scheduler tracks timing in parallel
		// and will take over each subsystem incrementally in future phases.
		if (scheduler)
		{
			int32 cyclesUsed = CPU.Cycles - cyclesBefore;
			if (cyclesUsed > 0)
				scheduler->advanceBy(scheduler->cpuCyclesToMaster(cyclesUsed));
		}

		while (CPU.Cycles >= CPU.NextEvent)
			S9xDoHEventProcessing();

		if (Settings.SA1)
			S9xSA1MainLoop();
	}

	S9xPackStatus();
}

static inline void S9xReschedule (void)
{
	switch (CPU.WhichEvent)
	{
		case HC_HBLANK_START_EVENT:
			CPU.WhichEvent = HC_HDMA_START_EVENT;
			CPU.NextEvent  = Timings.HDMAStart;
			break;

		case HC_HDMA_START_EVENT:
			CPU.WhichEvent = HC_HCOUNTER_MAX_EVENT;
			CPU.NextEvent  = Timings.H_Max;
			break;

		case HC_HCOUNTER_MAX_EVENT:
			CPU.WhichEvent = HC_HDMA_INIT_EVENT;
			CPU.NextEvent  = Timings.HDMAInit;
			break;

		case HC_HDMA_INIT_EVENT:
			CPU.WhichEvent = HC_RENDER_EVENT;
			CPU.NextEvent  = Timings.RenderPos;
			break;

		case HC_RENDER_EVENT:
			CPU.WhichEvent = HC_WRAM_REFRESH_EVENT;
			CPU.NextEvent  = Timings.WRAMRefreshPos;
			break;

		case HC_WRAM_REFRESH_EVENT:
			CPU.WhichEvent = HC_HBLANK_START_EVENT;
			CPU.NextEvent  = Timings.HBlankStart;
			break;
	}
}

void S9xDoHEventProcessing (void)
{
#ifdef DEBUGGER
	static char	eventname[7][32] =
	{
		"",
		"HC_HBLANK_START_EVENT",
		"HC_HDMA_START_EVENT  ",
		"HC_HCOUNTER_MAX_EVENT",
		"HC_HDMA_INIT_EVENT   ",
		"HC_RENDER_EVENT      ",
		"HC_WRAM_REFRESH_EVENT"
	};
#endif

#ifdef DEBUGGER
	if (Settings.TraceHCEvent)
		S9xTraceFormattedMessage("--- HC event processing  (%s)  expected HC:%04d  executed HC:%04d VC:%04d",
			eventname[CPU.WhichEvent], CPU.NextEvent, CPU.Cycles, CPU.V_Counter);
#endif

	switch (CPU.WhichEvent)
	{
		case HC_HBLANK_START_EVENT:
			S9xReschedule();
			break;

		case HC_HDMA_START_EVENT:
			S9xReschedule();

			if (PPU.HDMA && CPU.V_Counter <= PPU.ScreenHeight)
			{
			#ifdef DEBUGGER
				S9xTraceFormattedMessage("*** HDMA Transfer HC:%04d, Channel:%02x", CPU.Cycles, PPU.HDMA);
			#endif
				PPU.HDMA = S9xDoHDMA(PPU.HDMA);
			}

			break;

		case HC_HCOUNTER_MAX_EVENT:
			if (Settings.SuperFX)
			{
				if (!SuperFX.oneLineDone)
					S9xSuperFXExec();
				SuperFX.oneLineDone = FALSE;
			}

			S9xAPUEndScanline();
			CPU.Cycles -= Timings.H_Max;
			if (Timings.NMITriggerPos != 0xffff)
				Timings.NMITriggerPos -= Timings.H_Max;
			if (Timings.NextIRQTimer != 0x0fffffff)
				Timings.NextIRQTimer -= Timings.H_Max;
			S9xAPUSetReferenceTime(CPU.Cycles);

			if (Settings.SA1)
				SA1.Cycles -= Timings.H_Max * 3;

			CPU.V_Counter++;
			if (CPU.V_Counter >= Timings.V_Max)
			{
				CPU.V_Counter = 0;

				if (IPPU.Interlace && S9xInterlaceField())
					Timings.V_Max = Timings.V_Max_Master + 1;
				else
					Timings.V_Max = Timings.V_Max_Master;

				Memory.FillRAM[0x213F] ^= 0x80;
				PPU.RangeTimeOver = 0;

				// FIXME: reading $4210 will wait 2 cycles, then perform reading, then wait 4 more cycles.
				Memory.FillRAM[0x4210] = Model->_5A22;

				ICPU.Frame++;
				PPU.HVBeamCounterLatched = 0;
			}

			if (CPU.V_Counter == 240 && !IPPU.Interlace && S9xInterlaceField())
				Timings.H_Max = Timings.H_Max_Master - ONE_DOT_CYCLE;
			else
				Timings.H_Max = Timings.H_Max_Master;

			if (Model->_5A22 == 2)
			{
				if (CPU.V_Counter != 240 || IPPU.Interlace || !S9xInterlaceField())
				{
					if (Timings.WRAMRefreshPos == SNES_WRAM_REFRESH_HC_v2 - ONE_DOT_CYCLE)
						Timings.WRAMRefreshPos = SNES_WRAM_REFRESH_HC_v2;
					else
						Timings.WRAMRefreshPos = SNES_WRAM_REFRESH_HC_v2 - ONE_DOT_CYCLE;
				}
			}
			else
				Timings.WRAMRefreshPos = SNES_WRAM_REFRESH_HC_v1;

			if (CPU.V_Counter == PPU.ScreenHeight + FIRST_VISIBLE_LINE)
			{
				S9xEndScreenRefresh();
				#ifdef DEBUGGER
					if (!(CPU.Flags & FRAME_ADVANCE_FLAG))
				#endif
				{
					S9xSyncSpeed();
				}

				CPU.Flags |= SCAN_KEYS_FLAG;

				PPU.HDMA = 0;
			#ifdef DEBUGGER
				missing.dma_this_frame = 0;
			#endif
				IPPU.MaxBrightness = PPU.Brightness;
				PPU.ForcedBlanking = (Memory.FillRAM[0x2100] >> 7) & 1;

				if (!PPU.ForcedBlanking)
				{
					PPU.OAMAddr = PPU.SavedOAMAddr;

					uint8	tmp = 0;

					if (PPU.OAMPriorityRotation)
						tmp = (PPU.OAMAddr & 0xFE) >> 1;
					if ((PPU.OAMFlip & 1) || PPU.FirstSprite != tmp)
					{
						PPU.FirstSprite = tmp;
						IPPU.OBJChanged = TRUE;
					}

					PPU.OAMFlip = 0;
				}

				// FIXME: writing to $4210 will wait 6 cycles.
				Memory.FillRAM[0x4210] = 0x80 | Model->_5A22;
				if (Memory.FillRAM[0x4200] & 0x80)
				{
#ifdef DEBUGGER
					if (Settings.TraceHCEvent)
					   S9xTraceFormattedMessage ("NMI Scheduled for next scanline.");
#endif
					CPU.NMIPending = TRUE;
					Timings.NMITriggerPos = 6 + 6;
				}
			}

			if (CPU.V_Counter == PPU.ScreenHeight + 3)
			{
				if (Memory.FillRAM[0x4200] & 1)
					S9xDoAutoJoypad();
			}

			if (CPU.V_Counter == FIRST_VISIBLE_LINE)
				S9xStartScreenRefresh();

			S9xReschedule();

			// Re-sync scheduler to the reset CPU.Cycles and schedule next scanline
			if (scheduler)
			{
				scheduler->setMasterClock(scheduler->cpuCyclesToMaster(CPU.Cycles));
				S9xScheduleHorizontalEvents();
			}

			break;

		case HC_HDMA_INIT_EVENT:
			S9xReschedule();

			if (CPU.V_Counter == 0)
			{
			#ifdef DEBUGGER
				S9xTraceFormattedMessage("*** HDMA Init     HC:%04d, Channel:%02x", CPU.Cycles, PPU.HDMA);
			#endif
				S9xStartHDMA();
			}

			break;

		case HC_RENDER_EVENT:
			if (CPU.V_Counter >= FIRST_VISIBLE_LINE && CPU.V_Counter <= PPU.ScreenHeight)
				RenderLine((uint8) (CPU.V_Counter - FIRST_VISIBLE_LINE));

			S9xReschedule();

			break;

		case HC_WRAM_REFRESH_EVENT:
		#ifdef DEBUGGER
			S9xTraceFormattedMessage("*** WRAM Refresh  HC:%04d", CPU.Cycles);
		#endif

			CPU.Cycles += SNES_WRAM_REFRESH_CYCLES;

			S9xReschedule();

			break;
	}

#ifdef DEBUGGER
	if (Settings.TraceHCEvent)
		S9xTraceFormattedMessage("--- HC event rescheduled (%s)  expected HC:%04d  current  HC:%04d",
			eventname[CPU.WhichEvent], CPU.NextEvent, CPU.Cycles);
#endif
}

// ---------------------------------------------------------------------------
// Scheduler event callbacks — currently no-ops.
//
// The legacy S9xDoHEventProcessing() system above handles all actual scanline
// logic. These callbacks exist so the scheduler can track timing accurately
// in parallel without interfering. Replace them one at a time in future phases
// once the legacy path for that subsystem is removed.
// ---------------------------------------------------------------------------

static void S9xSchedulerHBlankStart()
{
	// Phase 2: add mid-scanline PPU register snapshot here
}

static void S9xSchedulerHDMAStart()
{
	// Phase 4: replace HC_HDMA_START_EVENT in legacy system with this
}

static void S9xSchedulerHCounterMax()
{
	// Phase 2: take over HC_HCOUNTER_MAX_EVENT once legacy path is removed
}

static void S9xSchedulerHDMAInit()
{
	// Phase 4: replace HC_HDMA_INIT_EVENT in legacy system with this
}

static void S9xSchedulerRender()
{
	// Phase 2: replace HC_RENDER_EVENT with dirty-register aware render here
}

static void S9xSchedulerWRAMRefresh()
{
	// Phase 2: replace HC_WRAM_REFRESH_EVENT in legacy system with this
}
