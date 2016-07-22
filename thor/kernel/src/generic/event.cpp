
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// AsyncEvent
// --------------------------------------------------------

AsyncEvent::AsyncEvent(EventType type, SubmitInfo submit_info)
		: type(type), submitInfo(submit_info) { }

AsyncEvent AsyncHandleLoad::getEvent() {
	AsyncEvent event(kEventMemoryLoad, completer.get<PostEventCompleter>().submitInfo);
	event.error = kErrSuccess;
	event.offset = offset;
	event.length = length;
	return event;
}
AsyncEvent AsyncInitiateLoad::getEvent() {
	AsyncEvent event(kEventMemoryLock, completer.get<PostEventCompleter>().submitInfo);
	event.error = kErrSuccess;
	return event;
}
AsyncEvent AsyncObserve::getEvent() {
	AsyncEvent event(kEventObserve, completer.get<PostEventCompleter>().submitInfo);
	event.error = kErrSuccess;
	return event;
}
AsyncEvent AsyncSendString::getEvent() {
	AsyncEvent event(kEventSendString, completer.get<PostEventCompleter>().submitInfo);
	event.error = error;
	return event;
}
AsyncEvent AsyncSendDescriptor::getEvent() {
	AsyncEvent event(kEventSendDescriptor, completer.get<PostEventCompleter>().submitInfo);
	event.error = error;
	return event;
}
AsyncEvent AsyncRecvString::getEvent() {
	if(type == kTypeNormal) {
		AsyncEvent event(kEventRecvString, completer.get<PostEventCompleter>().submitInfo);
		event.error = error;
		event.msgRequest = msgRequest;
		event.msgSequence = msgSequence;
		event.length = length;
		return event;
	}else{
		assert(type == kTypeToRing);
		
		AsyncEvent event(kEventRecvStringToRing, completer.get<PostEventCompleter>().submitInfo);
		event.error = error;
		event.msgRequest = msgRequest;
		event.msgSequence = msgSequence;
		event.offset = offset;
		event.length = length;
		return event;
	}
}
AsyncEvent AsyncRecvDescriptor::getEvent() {
	AsyncEvent event(kEventRecvDescriptor, completer.get<PostEventCompleter>().submitInfo);
	event.error = error;
	event.msgRequest = msgRequest;
	event.msgSequence = msgSequence;
	event.handle = handle;
	return event;
}
AsyncEvent AsyncAccept::getEvent() {
	AsyncEvent event(kEventAccept, completer.get<PostEventCompleter>().submitInfo);
	event.error = kErrSuccess;
	event.handle = handle;
	return event;
}
AsyncEvent AsyncConnect::getEvent() {
	AsyncEvent event(kEventConnect, completer.get<PostEventCompleter>().submitInfo);
	event.error = kErrSuccess;
	event.handle = handle;
	return event;
}
AsyncEvent AsyncRingItem::getEvent() { assert(false); }
AsyncEvent AsyncIrq::getEvent() {
	AsyncEvent event(kEventIrq, completer.get<PostEventCompleter>().submitInfo);
	event.error = kErrSuccess;
	return event;
}

// --------------------------------------------------------
// AsyncOperation
// --------------------------------------------------------

void AsyncOperation::complete(frigg::SharedPtr<AsyncOperation> operation) {
	AsyncCompleter &completer = operation->completer;
	switch(completer.tag()) {
	case AsyncCompleter::tagOf<NullCompleter>():
		break;
	case AsyncCompleter::tagOf<PostEventCompleter>(): {
		auto event_hub = completer.get<PostEventCompleter>().eventHub.grab();
		assert(event_hub);

		EventHub::Guard hub_guard(&event_hub->lock);
		event_hub->raiseEvent(hub_guard, frigg::move(operation));
	} break;
	case AsyncCompleter::tagOf<ReturnFromForkCompleter>(): {
		auto thread = completer.get<ReturnFromForkCompleter>().thread.grab();
		assert(thread);

		ScheduleGuard schedule_guard(scheduleLock.get());
		enqueueInSchedule(schedule_guard, frigg::move(thread));
	} break;
	default:
		assert(!"Unexpected AsyncCompleter");
	}
}

// --------------------------------------------------------
// EventHub
// --------------------------------------------------------

EventHub::EventHub() : p_waitingThreads(*kernelAlloc) { }

void EventHub::raiseEvent(Guard &guard, frigg::SharedPtr<AsyncOperation> operation) {
	assert(guard.protects(&lock));

	_eventQueue.addBack(frigg::move(operation));

	while(!p_waitingThreads.empty()) {
		KernelSharedPtr<Thread> waiting = p_waitingThreads.removeFront().grab();

		ScheduleGuard schedule_guard(scheduleLock.get());
		enqueueInSchedule(schedule_guard, waiting);
		schedule_guard.unlock();
	}
}

bool EventHub::hasEvent(Guard &guard) {
	assert(guard.protects(&lock));

	return !_eventQueue.empty();
}

frigg::SharedPtr<AsyncOperation> EventHub::dequeueEvent(Guard &guard) {
	assert(guard.protects(&lock));

	return _eventQueue.removeFront();
}

void EventHub::blockCurrentThread(Guard &guard) {
	assert(!intsAreEnabled());
	assert(guard.protects(&lock));

	if(forkExecutor()) {
		KernelUnsafePtr<Thread> this_thread = getCurrentThread();
		p_waitingThreads.addBack(this_thread.toWeak());
		
		// keep the lock on this hub unlocked while we sleep
		guard.unlock();
		
		ScheduleGuard schedule_guard(scheduleLock.get());
		doSchedule(frigg::move(schedule_guard));
		// note: doSchedule() takes care of the schedule_guard lock
	}
	
	// the guard lock was released during the first return of saveThisThread()
	guard.lock();
}

} // namespace thor
