import { describe, expect, it } from 'vitest'
import { decodePrograms, decodeSettings, decodeStatus, decodeZones } from './decode'
import { DecodeError } from './types'

const goodStatus = {
  runningZone: 4,
  secondsRemaining: 120,
  nextRunUtc: '2026-09-13T13:00:00Z',
  timezone: 'America/Los_Angeles',
  masterEnabled: true,
  rainDelayUntilUtc: '',
}

describe('decodeStatus', () => {
  it('reads every field', () => {
    const status = decodeStatus(goodStatus)
    expect(status.runningZone).toBe(4)
    expect(status.secondsRemaining).toBe(120)
    expect(status.nextRunUtc).toBe('2026-09-13T13:00:00Z')
    expect(status.timezone).toBe('America/Los_Angeles')
    expect(status.masterEnabled).toBe(true)
  })

  it('maps the empty timestamp to null', () => {
    expect(decodeStatus(goodStatus).rainDelayUntilUtc).toBeNull()
    expect(decodeStatus({ ...goodStatus, nextRunUtc: '' }).nextRunUtc).toBeNull()
  })

  it('throws naming the field when one is missing', () => {
    const { timezone, ...withoutZone } = goodStatus
    expect(timezone).toBeDefined()
    expect(() => decodeStatus(withoutZone)).toThrow(DecodeError)
    expect(() => decodeStatus(withoutZone)).toThrow(/timezone/)
  })

  it('throws when a number arrives as a string', () => {
    expect(() => decodeStatus({ ...goodStatus, secondsRemaining: '120' })).toThrow(/secondsRemaining/)
  })

  it('throws when a boolean arrives as a string', () => {
    expect(() => decodeStatus({ ...goodStatus, masterEnabled: 'true' })).toThrow(/masterEnabled/)
  })
})

describe('decodeZones', () => {
  it('reads a list', () => {
    const zones = decodeZones([{ id: 9, number: 3, name: 'Front lawn', enabled: true }])
    expect(zones).toHaveLength(1)
    expect(zones[0]?.id).toBe(9)
    expect(zones[0]?.number).toBe(3)
  })

  it('throws when the payload is not an array', () => {
    expect(() => decodeZones({ id: 9 })).toThrow(DecodeError)
  })

  it('names the index of the bad element', () => {
    expect(() =>
      decodeZones([
        { id: 9, number: 3, name: 'Front lawn', enabled: true },
        { id: 10, number: 4, name: 'Roses' },
      ]),
    ).toThrow(/\[1\]\.enabled/)
  })
})

const goodProgram = {
  id: 1,
  name: 'Morning',
  enabled: true,
  dayMode: 'DaysOfWeek',
  dowMask: 5,
  intervalDays: 0,
  anchorDate: null,
  startTimes: [{ id: 3, programId: 1, minutesAfterMidnight: 360, timezone: 'America/Los_Angeles' }],
  zones: [{ id: 7, programId: 1, zoneId: 9, sequence: 1, durationSeconds: 600 }],
  nextRunUtc: '2026-09-14T13:00:00Z',
}

describe('decodePrograms', () => {
  it('reads nested start times and zones', () => {
    const program = decodePrograms([goodProgram])[0]
    expect(program?.startTimes[0]?.minutesAfterMidnight).toBe(360)
    expect(program?.zones[0]?.zoneId).toBe(9)
    expect(program?.zones[0]?.durationSeconds).toBe(600)
  })

  it('throws on an unknown dayMode', () => {
    expect(() => decodePrograms([{ ...goodProgram, dayMode: 'days_of_week' }])).toThrow(/dayMode/)
    expect(() => decodePrograms([{ ...goodProgram, dayMode: 'everyNDays' }])).toThrow(/dayMode/)
    expect(() => decodePrograms([{ ...goodProgram, dayMode: 'DAYSOFWEEK' }])).toThrow(/dayMode/)
  })

  it('accepts a program with no nextRunUtc', () => {
    const { nextRunUtc, ...withoutNext } = goodProgram
    expect(nextRunUtc).toBeDefined()
    expect(decodePrograms([withoutNext])[0]?.nextRunUtc).toBeNull()
  })

  it('reads the empty anchorDate the daemon sends for an unset date as null', () => {
    expect(decodePrograms([{ ...goodProgram, anchorDate: '' }])[0]?.anchorDate).toBeNull()
  })

  it('sorts zones by sequence', () => {
    const shuffled = {
      ...goodProgram,
      zones: [
        { id: 8, programId: 1, zoneId: 10, sequence: 2, durationSeconds: 300 },
        { id: 7, programId: 1, zoneId: 9, sequence: 1, durationSeconds: 600 },
      ],
    }
    expect(decodePrograms([shuffled])[0]?.zones.map((zone) => zone.zoneId)).toEqual([9, 10])
  })
})

describe('decodeSettings', () => {
  it('accepts a string map', () => {
    expect(decodeSettings({ master_enabled: 'true', max_zone_seconds: '1800' })).toEqual({
      master_enabled: 'true',
      max_zone_seconds: '1800',
    })
  })

  it('throws when a value is not a string', () => {
    expect(() => decodeSettings({ master_enabled: true })).toThrow(/master_enabled/)
  })
})
