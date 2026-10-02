import type { Program, Status, Zone } from '../api/types'

/**
 * Zone ids are offset from zone numbers by 6. The API addresses a valve by
 * `number` and a program references a zone by `id`; a fixture where the two are
 * equal cannot tell a transposed identifier from a correct one.
 */
export const zoneFixtures: Zone[] = [
  { id: 7, number: 1, name: 'Front lawn', enabled: true },
  { id: 8, number: 2, name: 'Side strip', enabled: true },
  { id: 9, number: 3, name: 'Roses', enabled: true },
  { id: 10, number: 4, name: 'Back lawn', enabled: true },
  { id: 11, number: 5, name: 'Vegetable bed', enabled: true },
  { id: 12, number: 6, name: 'Hedge', enabled: true },
  { id: 13, number: 7, name: 'Planters', enabled: false },
  { id: 14, number: 8, name: 'Orchard', enabled: true },
]

export const idleStatus: Status = {
  running: [],
  program: null,
  queue: [],
  maxConcurrentZones: 2,
  nextRunUtc: '2026-09-14T13:00:00Z',
  timezone: 'America/Los_Angeles',
  masterEnabled: true,
  rainDelayUntilUtc: null,
}

export const runningStatus: Status = {
  ...idleStatus,
  running: [{ zone: 3, secondsRemaining: 120, source: 'manual' }],
}

export const morningProgram: Program = {
  id: 1,
  name: 'Morning',
  enabled: true,
  dayMode: 'DaysOfWeek',
  dowMask: 0b0000101,
  intervalDays: 0,
  anchorDate: null,
  startTimes: [{ id: 3, minutesAfterMidnight: 360, timezone: 'America/Los_Angeles' }],
  steps: [
    { zones: [7], durationSeconds: 600 },
    { zones: [9], durationSeconds: 300 },
  ],
  nextRunUtc: '2026-09-14T13:00:00Z',
}
