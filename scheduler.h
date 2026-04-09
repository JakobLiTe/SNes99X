/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_

#include "port.h"
#include <queue>
#include <vector>

enum EventComponent
{
	EVENT_CPU,
	EVENT_PPU,
	EVENT_APU,
	EVENT_DMA,
	EVENT_HDMA,
	EVENT_TIMER,
	EVENT_NMI,
	EVENT_IRQ
};

enum EventType
{
	EVENT_HBLANK_START,
	EVENT_HDMA_START,
	EVENT_HCOUNTER_MAX,
	EVENT_HDMA_INIT,
	EVENT_RENDER,
	EVENT_WRAM_REFRESH,
	EVENT_CPU_STEP,
	EVENT_APU_SYNC,
	EVENT_TIMER_IRQ,
	EVENT_NMI_TRIGGER,
	EVENT_CUSTOM
};

typedef void (*EventCallback)(void);

struct SchedulerEvent
{
	int64		timestamp;
	EventComponent	component;
	EventType	type;
	EventCallback	callback;
	void		*data;

	bool operator>(const SchedulerEvent &other) const
	{
		return timestamp > other.timestamp;
	}
};

class Scheduler
{
private:
	std::priority_queue<SchedulerEvent, std::vector<SchedulerEvent>, std::greater<SchedulerEvent>> eventQueue;
	int64 masterClock;

public:
	Scheduler();
	~Scheduler();

	void reset();
	
	void addEvent(int64 timestamp, EventComponent component, EventType type, EventCallback callback = nullptr, void *data = nullptr);
	
	void addEventRelative(int64 cycles, EventComponent component, EventType type, EventCallback callback = nullptr, void *data = nullptr);
	
	void runNext();
	
	void advanceTo(int64 targetClock);
	
	void advanceBy(int64 cycles);
	
	bool hasEvents() const;
	
	int64 getNextEventTime() const;
	
	int64 getMasterClock() const;
	
	void setMasterClock(int64 clock);
	
	int64 cpuCyclesToMaster(int32 cpuCycles) const;
	
	int32 masterToCpuCycles(int64 masterCycles) const;
};

extern Scheduler *scheduler;

void S9xInitScheduler();
void S9xDeinitScheduler();

#endif
