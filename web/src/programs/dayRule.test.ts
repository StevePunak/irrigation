import { describe, expect, it } from 'vitest'
import type { Program } from '../api/types'
import {
  WEEKDAY_LABELS,
  dayRuleSummary,
  maskFromWeekdays,
  toDraft,
  toggleWeekday,
  totalRuntimeSeconds,
  weekdaysFromMask,
} from './dayRule'
import { morningProgram } from '../test/fixtures'

describe('weekday bits', () => {
  it('puts Monday at bit 0 and Sunday at bit 6', () => {
    expect(WEEKDAY_LABELS[0]).toBe('Mon')
    expect(WEEKDAY_LABELS[6]).toBe('Sun')
    expect(weekdaysFromMask(0b0000001)).toEqual([0])
    expect(weekdaysFromMask(0b1000000)).toEqual([6])
  })

  it('reads a multi-day mask in order', () => {
    expect(weekdaysFromMask(0b0010101)).toEqual([0, 2, 4])
  })

  it('round-trips a mask', () => {
    expect(maskFromWeekdays([0, 2, 4])).toBe(0b0010101)
    expect(maskFromWeekdays(weekdaysFromMask(0b1010101))).toBe(0b1010101)
    expect(weekdaysFromMask(maskFromWeekdays([6]))).toEqual([6])
  })

  it('toggles one bit and leaves the rest alone', () => {
    expect(toggleWeekday(0b0000001, 2)).toBe(0b0000101)
    expect(toggleWeekday(0b0000101, 0)).toBe(0b0000100)
  })

  it('ignores bits outside the week', () => {
    expect(weekdaysFromMask(0b11111111)).toEqual([0, 1, 2, 3, 4, 5, 6])
    expect(toggleWeekday(0, 7)).toBe(0)
    expect(toggleWeekday(0, -1)).toBe(0)
  })
})

describe('dayRuleSummary', () => {
  const base: Pick<Program, 'dayMode' | 'dowMask' | 'intervalDays' | 'anchorDate'> = {
    dayMode: 'DaysOfWeek',
    dowMask: 0,
    intervalDays: 0,
    anchorDate: null,
  }

  it('names the selected weekdays', () => {
    expect(dayRuleSummary({ ...base, dowMask: 0b0000001 })).toBe('Mon')
    expect(dayRuleSummary({ ...base, dowMask: 0b1000000 })).toBe('Sun')
    expect(dayRuleSummary({ ...base, dowMask: 0b0010101 })).toBe('Mon, Wed, Fri')
  })

  it('collapses a full week', () => {
    expect(dayRuleSummary({ ...base, dowMask: 0b1111111 })).toBe('Every day')
  })

  it('says so when no day is selected', () => {
    expect(dayRuleSummary({ ...base, dowMask: 0 })).toBe('No days selected')
  })

  it('names the other three modes', () => {
    expect(dayRuleSummary({ ...base, dayMode: 'Odd' })).toBe('Odd days')
    expect(dayRuleSummary({ ...base, dayMode: 'Even' })).toBe('Even days')
    expect(
      dayRuleSummary({ ...base, dayMode: 'EveryNDays', intervalDays: 3, anchorDate: '2026-04-01' }),
    ).toBe('Every 3 days from 1 Apr 2026')
  })

  it('handles an interval of one', () => {
    expect(
      dayRuleSummary({ ...base, dayMode: 'EveryNDays', intervalDays: 1, anchorDate: '2026-04-01' }),
    ).toBe('Every day from 1 Apr 2026')
  })

  it('ignores the mask when the mode is not DaysOfWeek', () => {
    expect(dayRuleSummary({ ...base, dayMode: 'Odd', dowMask: 0b1111111 })).toBe('Odd days')
  })

  it('treats an out-of-range anchor date as unset rather than rendering undefined', () => {
    expect(
      dayRuleSummary({ ...base, dayMode: 'EveryNDays', intervalDays: 3, anchorDate: '2026-13-01' }),
    ).toBe('Every 3 days from an unset date')
  })
})

describe('totalRuntimeSeconds', () => {
  it('sums every zone duration', () => {
    expect(totalRuntimeSeconds([{ durationSeconds: 600 }, { durationSeconds: 300 }])).toBe(900)
  })

  it('is zero for an empty program', () => {
    expect(totalRuntimeSeconds([])).toBe(0)
  })
})

describe('toDraft', () => {
  it('strips the ids the daemon assigns', () => {
    const draft = toDraft(morningProgram)

    expect(draft).toEqual({
      name: 'Morning',
      enabled: true,
      dayMode: 'DaysOfWeek',
      dowMask: 0b0000101,
      intervalDays: 0,
      anchorDate: null,
      startTimes: [{ minutesAfterMidnight: 360, timezone: 'America/Los_Angeles' }],
      zones: [
        { zoneId: 7, sequence: 1, durationSeconds: 600 },
        { zoneId: 9, sequence: 2, durationSeconds: 300 },
      ],
    })
  })

  it('keeps the zone ids and drops the row ids', () => {
    const draft = toDraft(morningProgram)
    expect(draft.zones.map((zone) => zone.zoneId)).toEqual([7, 9])
    expect(JSON.stringify(draft)).not.toContain('"id"')
  })
})
