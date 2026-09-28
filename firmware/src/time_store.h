#pragma once

// This board has no battery-backed RTC chip, so on a real power loss there's
// no way to know how long the device was off. What this DOES give you:
// the clock shows the last-known time immediately on boot (instead of a
// blank "waiting for phone") and self-corrects within seconds once the
// phone reconnects and re-syncs -- rather than sitting on a stale value
// until you notice.

// Call once in setup(), before the first screen draw, to seed baseEpoch /
// utcOffsetMinutes from flash if a previous sync was saved.
void loadPersistedTime();

// Call whenever a fresh time sync arrives from the phone, to save it for
// next boot.
void persistTime();
