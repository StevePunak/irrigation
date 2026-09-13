import { describe, expect, it } from 'vitest'
import {
  INVALID_ZONE_MARKER,
  formatClock,
  formatCountdown,
  formatDayAndClock,
  formatDuration,
  inputValueToMinutes,
  minutesToClock,
  minutesToInputValue,
} from './zonedformat'

const LA = 'America/Los_Angeles'
const AUCKLAND = 'Pacific/Auckland'

describe('formatClock', () => {
  it('renders in the zone it is given', () => {
    expect(formatClock('2026-09-13T13:00:00Z', LA)).toBe('6:00 AM')
    expect(formatClock('2026-09-13T13:00:00Z', 'UTC')).toBe('1:00 PM')
  })

  it('renders the same instant differently in two zones', () => {
    const instant = '2026-09-13T13:00:00Z'
    expect(formatClock(instant, LA)).not.toBe(formatClock(instant, AUCKLAND))
  })

  it('tracks a DST transition rather than a fixed offset', () => {
    // Same UTC wall clock either side of the spring transition. PST is UTC-8,
    // PDT is UTC-7, so the local hour differs by one.
    expect(formatClock('2026-01-08T18:00:00Z', LA)).toBe('10:00 AM')
    expect(formatClock('2026-03-12T18:00:00Z', LA)).toBe('11:00 AM')
  })

  it('marks an unusable zone instead of falling back to the browser', () => {
    expect(formatClock('2026-09-13T13:00:00Z', 'Not/AZone')).toBe(INVALID_ZONE_MARKER)
    expect(formatClock('2026-09-13T13:00:00Z', '')).toBe(INVALID_ZONE_MARKER)
  })

  it('marks an unparseable instant', () => {
    expect(formatClock('', LA)).toBe(INVALID_ZONE_MARKER)
    expect(formatClock('not a timestamp', LA)).toBe(INVALID_ZONE_MARKER)
  })
})

describe('formatDayAndClock', () => {
  const now = Date.parse('2026-09-13T20:00:00Z') // 1:00 PM in Los Angeles

  it('says Today for an instant on the same calendar day in the controller zone', () => {
    expect(formatDayAndClock('2026-09-14T01:00:00Z', LA, now)).toBe('Today 6:00 PM')
  })

  it('uses the controller zone for the day boundary, not the host zone', () => {
    // 2026-09-14T01:00:00Z is already the 14th in UTC and still the 13th in
    // Los Angeles. A host-zone day boundary calls this Tomorrow.
    expect(formatDayAndClock('2026-09-14T01:00:00Z', LA, now)).toMatch(/^Today/)
    expect(formatDayAndClock('2026-09-14T01:00:00Z', 'UTC', now)).toMatch(/^Tomorrow/)
  })

  it('says Tomorrow for the next calendar day', () => {
    expect(formatDayAndClock('2026-09-14T13:00:00Z', LA, now)).toBe('Tomorrow 6:00 AM')
  })

  it('names the weekday within the next week', () => {
    expect(formatDayAndClock('2026-09-16T13:00:00Z', LA, now)).toBe('Wed 6:00 AM')
  })

  it('adds the date beyond a week out', () => {
    expect(formatDayAndClock('2026-09-28T13:00:00Z', LA, now)).toBe('Mon 28 Sep 6:00 AM')
  })
})

describe('formatDuration', () => {
  it('renders seconds, minutes and hours', () => {
    expect(formatDuration(45)).toBe('45 s')
    expect(formatDuration(600)).toBe('10 min')
    expect(formatDuration(5400)).toBe('1 h 30 min')
    expect(formatDuration(3600)).toBe('1 h')
    expect(formatDuration(0)).toBe('0 s')
  })
})

describe('formatCountdown', () => {
  it('renders mm:ss under an hour and h:mm:ss above', () => {
    expect(formatCountdown(120)).toBe('2:00')
    expect(formatCountdown(65)).toBe('1:05')
    expect(formatCountdown(9)).toBe('0:09')
    expect(formatCountdown(3930)).toBe('1:05:30')
  })

  it('floors at zero', () => {
    expect(formatCountdown(-5)).toBe('0:00')
  })
})

describe('wall-clock minutes', () => {
  it('converts minutes after midnight without consulting a timezone', () => {
    expect(minutesToClock(0)).toBe('12:00 AM')
    expect(minutesToClock(360)).toBe('6:00 AM')
    expect(minutesToClock(720)).toBe('12:00 PM')
    expect(minutesToClock(1140)).toBe('7:00 PM')
    expect(minutesToClock(1439)).toBe('11:59 PM')
  })

  it('round-trips through the time input format', () => {
    expect(minutesToInputValue(360)).toBe('06:00')
    expect(minutesToInputValue(1439)).toBe('23:59')
    expect(minutesToInputValue(0)).toBe('00:00')
    expect(inputValueToMinutes('06:00')).toBe(360)
    expect(inputValueToMinutes('23:59')).toBe(1439)
    expect(inputValueToMinutes('00:00')).toBe(0)
  })

  it('rejects an unparseable time input', () => {
    expect(inputValueToMinutes('')).toBe(-1)
    expect(inputValueToMinutes('25:00')).toBe(-1)
    expect(inputValueToMinutes('12:60')).toBe(-1)
  })
})
