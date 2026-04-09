/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "snes9x.h"
#include "scheduler.h"

Scheduler *scheduler = nullptr;

Scheduler::Scheduler()
{
	masterClock = 0;
}

Scheduler::~Scheduler()
{
}

void Scheduler::reset()
{
	while (!eventQueue.empty())
		eventQueue.pop();
	masterClock = 0;
}

void Scheduler::addEvent(int64 timestamp, EventComponent component, EventType type, EventCallback callback, void *data)
{
	SchedulerEvent event;
	event.timestamp = timestamp;
	event.component = component;
	event.type = type;
	event.callback = callback;
	event.data = data;
	
	eventQueue.push(event);
}

void Scheduler::addEventRelative(int64 cycles, EventComponent component, EventType type, EventCallback callback, void *data)
{
	addEvent(masterClock + cycles, component, type, callback, data);
}

void Scheduler::runNext()
{
	if (eventQueue.empty())
		return;
	
	SchedulerEvent event = eventQueue.top();
	eventQueue.pop();
	
	masterClock = event.timestamp;
	
	if (event.callback)
		event.callback();
}

void Scheduler::advanceTo(int64 targetClock)
{
	while (!eventQueue.empty() && eventQueue.top().timestamp <= targetClock)
	{
		runNext();
	}
	
	if (masterClock < targetClock)
		masterClock = targetClock;
}

void Scheduler::advanceBy(int64 cycles)
{
	advanceTo(masterClock + cycles);
}

bool Scheduler::hasEvents() const
{
	return !eventQueue.empty();
}

int64 Scheduler::getNextEventTime() const
{
	if (eventQueue.empty())
		return -1;
	return eventQueue.top().timestamp;
}

int64 Scheduler::getMasterClock() const
{
	return masterClock;
}

void Scheduler::setMasterClock(int64 clock)
{
	masterClock = clock;
}

int64 Scheduler::cpuCyclesToMaster(int32 cpuCycles) const
{
	return (int64)cpuCycles * ONE_CYCLE;
}

int32 Scheduler::masterToCpuCycles(int64 masterCycles) const
{
	return (int32)(masterCycles / ONE_CYCLE);
}

void S9xInitScheduler()
{
	if (!scheduler)
		scheduler = new Scheduler();
	else
		scheduler->reset();
}

void S9xDeinitScheduler()
{
	if (scheduler)
	{
		delete scheduler;
		scheduler = nullptr;
	}
}
