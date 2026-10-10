import { describe, expect, it } from 'vitest'
import { decodeClimateHistory, decodePrograms, decodeSettings, decodeStatus, decodeZones } from './decode'
import { DecodeError } from './types'

const goodStatus = {
  running: [
    { zone: 5, secondsRemaining: 1712, source: 'program' },
    { zone: 1, secondsRemaining: 240, source: 'manual' },
  ],
  program: { id: 2, name: 'Morning Drip', step: 1, stepCount: 2, waitingZones: [7] },
  queue: [{ programId: 1, name: 'Summer', queuedAtUtc: '2026-09-13T13:00:04Z' }],
  maxConcurrentZones: 2,
  nextRunUtc: '2026-09-13T13:00:00Z',
  timezone: 'America/Los_Angeles',
  masterEnabled: true,
  stopHeld: false,
  rainDelayUntilUtc: '',
}

describe('decodeStatus', () => {
  it('reads every field', () => {
    const status = decodeStatus(goodStatus)
    expect(status.running).toEqual([
      { zone: 5, secondsRemaining: 1712, source: 'program' },
      { zone: 1, secondsRemaining: 240, source: 'manual' },
    ])
    expect(status.program).toEqual({ id: 2, name: 'Morning Drip', step: 1, stepCount: 2, waitingZones: [7] })
    expect(status.queue).toEqual([{ programId: 1, name: 'Summer', queuedAtUtc: '2026-09-13T13:00:04Z' }])
    expect(status.maxConcurrentZones).toBe(2)
    expect(status.nextRunUtc).toBe('2026-09-13T13:00:00Z')
    expect(status.timezone).toBe('America/Los_Angeles')
    expect(status.masterEnabled).toBe(true)
  })

  it('reads a fresh climate reading', () => {
    expect(decodeStatus({ ...goodStatus, climate: { temperatureC: 25.4, humidityPercent: 65.5 } }).climate).toEqual({
      temperatureC: 25.4,
      humidityPercent: 65.5,
    })
  })

  it('keeps null climate values as a sensor with no fresh reading', () => {
    expect(decodeStatus({ ...goodStatus, climate: { temperatureC: null, humidityPercent: null } }).climate).toEqual({
      temperatureC: null,
      humidityPercent: null,
    })
  })

  it('reads a null or absent climate as no sensor', () => {
    expect(decodeStatus({ ...goodStatus, climate: null }).climate).toBeNull()
    expect(decodeStatus(goodStatus).climate).toBeNull()
  })

  it('throws when a climate value arrives as a string', () => {
    expect(() =>
      decodeStatus({ ...goodStatus, climate: { temperatureC: '25.4', humidityPercent: 65.5 } }),
    ).toThrow(/status\.climate\.temperatureC/)
  })

  it('reads a null program as no program running', () => {
    expect(decodeStatus({ ...goodStatus, program: null }).program).toBeNull()
  })

  it('throws when program is absent', () => {
    const { program, ...withoutProgram } = goodStatus
    expect(program).toBeDefined()
    expect(() => decodeStatus(withoutProgram)).toThrow(/status\.program/)
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
    expect(() => decodeStatus({ ...goodStatus, maxConcurrentZones: '2' })).toThrow(/maxConcurrentZones/)
  })

  it('throws when a boolean arrives as a string', () => {
    expect(() => decodeStatus({ ...goodStatus, masterEnabled: 'true' })).toThrow(/masterEnabled/)
  })

  it('names the running entry with an unknown source', () => {
    const running = [{ zone: 5, secondsRemaining: 10, source: 'timer' }]
    expect(() => decodeStatus({ ...goodStatus, running })).toThrow(/running\[0\]\.source/)
  })

  it('reads a panel source', () => {
    const running = [{ zone: 3, secondsRemaining: 600, source: 'panel' }]
    expect(decodeStatus({ ...goodStatus, running }).running).toEqual(running)
  })

  it('names the waiting zone that is not a number', () => {
    const program = { ...goodStatus.program, waitingZones: [7, '8'] }
    expect(() => decodeStatus({ ...goodStatus, program })).toThrow(/waitingZones\[1\]/)
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
  steps: [{ id: 7, zones: [9, 11], durationSeconds: 600 }],
  nextRunUtc: '2026-09-14T13:00:00Z',
}

describe('decodePrograms', () => {
  it('reads nested start times and steps', () => {
    const program = decodePrograms([goodProgram])[0]
    expect(program?.startTimes[0]?.minutesAfterMidnight).toBe(360)
    expect(program?.steps).toEqual([{ zones: [9, 11], durationSeconds: 600 }])
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

  it('keeps steps in the order the daemon sent them', () => {
    const twoSteps = {
      ...goodProgram,
      steps: [
        { id: 8, zones: [10], durationSeconds: 300 },
        { id: 7, zones: [9], durationSeconds: 600 },
      ],
    }
    expect(decodePrograms([twoSteps])[0]?.steps.map((step) => step.zones[0])).toEqual([10, 9])
  })

  it('names the step zone that is not a number', () => {
    const badStep = { ...goodProgram, steps: [{ id: 7, zones: ['9'], durationSeconds: 600 }] }
    expect(() => decodePrograms([badStep])).toThrow(/steps\[0\]\.zones\[0\]/)
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

describe('decodeClimateHistory', () => {
  const wire = {
    fromUtc: '2026-10-09T18:43:43Z',
    toUtc: '2026-10-10T18:43:43Z',
    bucketSeconds: 300,
    buckets: [
      {
        startUtc: '2026-10-10T18:40:00Z',
        count: 43,
        temperatureC: { min: 25.7, mean: 25.75, max: 25.8 },
        humidityPercent: { min: 70.4, mean: 70.8, max: 71.3 },
      },
    ],
  }

  it('decodes the daemon wire shape', () => {
    const history = decodeClimateHistory(wire)
    expect(history.bucketSeconds).toBe(300)
    expect(history.fromUtc).toBe('2026-10-09T18:43:43Z')
    expect(history.buckets).toHaveLength(1)
    expect(history.buckets[0]!.temperatureC).toEqual({ min: 25.7, mean: 25.75, max: 25.8 })
    expect(history.buckets[0]!.humidityPercent.max).toBe(71.3)
  })

  it('names the field of a malformed bucket', () => {
    const broken = { ...wire, buckets: [{ ...wire.buckets[0], humidityPercent: { min: 1, max: 2 } }] }
    expect(() => decodeClimateHistory(broken)).toThrow(DecodeError)
    expect(() => decodeClimateHistory(broken)).toThrow(/climate\.buckets\[0\]\.humidityPercent\.mean/)
  })
})
