"""EventBus — fan out log lines + state changes to any number of WebSocket subscribers.

Deliberately tiny: an in-process async pub/sub with per-subscriber queues. The orchestrator and the
node console publish; the FastAPI ``/ws`` handler subscribes. Synchronous callers (the serial
``on_line`` callback runs off the event loop thread) use ``emit_threadsafe``.
"""

from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from typing import Any


@dataclass
class Event:
    type: str  # "log" | "state" | "progress" | "candidate" | "error"
    data: Any = None


class EventBus:
    def __init__(self) -> None:
        self._subs: set[asyncio.Queue[Event]] = set()
        self._loop: asyncio.AbstractEventLoop | None = None

    def bind_loop(self, loop: asyncio.AbstractEventLoop) -> None:
        self._loop = loop

    def subscribe(self) -> asyncio.Queue[Event]:
        q: asyncio.Queue[Event] = asyncio.Queue(maxsize=1000)
        self._subs.add(q)
        return q

    def unsubscribe(self, q: asyncio.Queue[Event]) -> None:
        self._subs.discard(q)

    def emit(self, ev: Event) -> None:
        for q in list(self._subs):
            try:
                q.put_nowait(ev)
            except asyncio.QueueFull:
                pass  # a slow UI must never stall the scan

    def emit_threadsafe(self, ev: Event) -> None:
        if self._loop is not None:
            self._loop.call_soon_threadsafe(self.emit, ev)
        else:
            self.emit(ev)

    def log(self, msg: str) -> None:
        self.emit(Event("log", msg))
