import { describe, expect, it } from 'vitest'
import {
  INVALID_ZONE_MARKER,
  axisTicks,
  formatAxisTick,
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

  it('follows a DST transition in the zone offset', () => {
    // Same UTC wall clock either side of the spring transition. PST is UTC-8,
    // PDT is UTC-7, so the local hour differs by one.
    expect(formatClock('2026-01-08T18:00:00Z', LA)).toBe('10:00 AM')
    expect(formatClock('2026-03-12T18:00:00Z', LA)).toBe('11:00 AM')
  })

  it('marks an unusable zone with the invalid-zone marker', () => {
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

  it('uses the controller zone for the day boundary', () => {
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

  it('computes the day gap from calendar fields across a DST transition', () => {
    // 2026-11-01 is when America/Los_Angeles falls back from PDT to PST.
    // now: Oct 31 7:00 PM local (still PDT). target: Nov 6 6:00 PM local
    // (already PST), six calendar days later. A raw millisecond gap between
    // local midnights would cross the one-hour fallback and risk rounding to
    // the wrong day count; the calendar-field gap must not.
    const dstNow = Date.parse('2026-11-01T02:00:00Z')
    expect(formatDayAndClock('2026-11-07T02:00:00Z', LA, dstNow)).toBe('Fri 6:00 PM')
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

  it('marks an out-of-range or NaN minute value', () => {
    expect(minutesToClock(-1)).toBe(INVALID_ZONE_MARKER)
    expect(minutesToClock(NaN)).toBe(INVALID_ZONE_MARKER)
    expect(minutesToInputValue(-1)).toBe('00:00')
    expect(minutesToInputValue(NaN)).toBe('00:00')
  })
})

describe('axisTicks', () => {
  const at = (iso: string) => Date.parse(iso)

  it('ticks a day every six local hours', () => {
    // 2026-10-10 07:00Z is midnight in Los Angeles (PDT).
    const ticks = axisTicks(at('2026-10-10T06:30:00Z'), at('2026-10-11T06:30:00Z'), LA, 24)
    expect(ticks.map((ms) => formatAxisTick(ms, LA, 24))).toEqual(['12 AM', '6 AM', '12 PM', '6 PM'])
    expect(new Date(ticks[0]!).toISOString()).toBe('2026-10-10T07:00:00.000Z')
  })

  it('ticks a week at each local midnight', () => {
    const ticks = axisTicks(at('2026-10-03T18:00:00Z'), at('2026-10-10T18:00:00Z'), LA, 168)
    expect(ticks.map((ms) => formatAxisTick(ms, LA, 168))).toEqual([
      'Sun 4',
      'Mon 5',
      'Tue 6',
      'Wed 7',
      'Thu 8',
      'Fri 9',
      'Sat 10',
    ])
  })

  it('keeps local midnight across the fall-back day', () => {
    // 2026-11-01 is when America/Los_Angeles falls back; midnight moves from 07:00Z to 08:00Z.
    const ticks = axisTicks(at('2026-10-31T12:00:00Z'), at('2026-11-03T12:00:00Z'), LA, 168)
    expect(ticks.map((ms) => new Date(ms).toISOString())).toEqual([
      '2026-11-01T07:00:00.000Z',
      '2026-11-02T08:00:00.000Z',
      '2026-11-03T08:00:00.000Z',
    ])
  })

  it('ticks a month on the 1st, 8th, 15th, 22nd and 29th', () => {
    const ticks = axisTicks(at('2026-09-10T00:00:00Z'), at('2026-10-10T00:00:00Z'), 'UTC', 720)
    expect(ticks.map((ms) => formatAxisTick(ms, 'UTC', 720))).toEqual(['15 Sep', '22 Sep', '29 Sep', '1 Oct', '8 Oct'])
  })

  it('ticks a year on the first of each month and names the year at January', () => {
    const ticks = axisTicks(at('2026-10-10T00:00:00Z'), at('2027-03-10T00:00:00Z'), 'UTC', 8760)
    expect(ticks.map((ms) => formatAxisTick(ms, 'UTC', 8760))).toEqual(['Nov', 'Dec', 'Jan 2027', 'Feb', 'Mar'])
  })

  it('gives no ticks for a zone the runtime does not know', () => {
    expect(axisTicks(at('2026-10-10T00:00:00Z'), at('2026-10-11T00:00:00Z'), 'Not/AZone', 24)).toEqual([])
    expect(formatAxisTick(at('2026-10-10T00:00:00Z'), 'Not/AZone', 24)).toBe(INVALID_ZONE_MARKER)
  })
})
