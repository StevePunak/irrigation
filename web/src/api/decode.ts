import {
  DAY_MODES,
  DecodeError,
  type DayMode,
  type Program,
  type ProgramStartTime,
  type ProgramZone,
  type SettingsMap,
  type Status,
  type Zone,
} from './types'

function asRecord(value: unknown, field: string): Record<string, unknown> {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new DecodeError(field, 'expected an object')
  }
  return value as Record<string, unknown>
}

function asArray(value: unknown, field: string): unknown[] {
  if (Array.isArray(value) === false) {
    throw new DecodeError(field, 'expected an array')
  }
  return value as unknown[]
}

function num(source: Record<string, unknown>, key: string, field: string): number {
  const value = source[key]
  if (typeof value !== 'number' || Number.isFinite(value) === false) {
    throw new DecodeError(`${field}.${key}`, `expected a number, received ${typeof value}`)
  }
  return value
}

function str(source: Record<string, unknown>, key: string, field: string): string {
  const value = source[key]
  if (typeof value !== 'string') {
    throw new DecodeError(`${field}.${key}`, `expected a string, received ${typeof value}`)
  }
  return value
}

function bool(source: Record<string, unknown>, key: string, field: string): boolean {
  const value = source[key]
  if (typeof value !== 'boolean') {
    throw new DecodeError(`${field}.${key}`, `expected a boolean, received ${typeof value}`)
  }
  return value
}

/** An absent or empty Qt QDateTime serialises to "". */
function instant(source: Record<string, unknown>, key: string, field: string): string | null {
  const value = source[key]
  if (value === undefined || value === null || value === '') {
    return null
  }
  if (typeof value !== 'string') {
    throw new DecodeError(`${field}.${key}`, `expected an ISO string, received ${typeof value}`)
  }
  return value
}

function dayMode(source: Record<string, unknown>, field: string): DayMode {
  const value = str(source, 'dayMode', field)
  if (DAY_MODES.includes(value as DayMode) === false) {
    throw new DecodeError(`${field}.dayMode`, `unknown day mode "${value}"`)
  }
  return value as DayMode
}

export function decodeStatus(payload: unknown): Status {
  const source = asRecord(payload, 'status')
  return {
    runningZone: num(source, 'runningZone', 'status'),
    secondsRemaining: num(source, 'secondsRemaining', 'status'),
    nextRunUtc: instant(source, 'nextRunUtc', 'status'),
    timezone: str(source, 'timezone', 'status'),
    masterEnabled: bool(source, 'masterEnabled', 'status'),
    rainDelayUntilUtc: instant(source, 'rainDelayUntilUtc', 'status'),
  }
}

export function decodeZones(payload: unknown): Zone[] {
  return asArray(payload, 'zones').map((element, index) => {
    const field = `zones[${index}]`
    const source = asRecord(element, field)
    return {
      id: num(source, 'id', field),
      number: num(source, 'number', field),
      name: str(source, 'name', field),
      enabled: bool(source, 'enabled', field),
    }
  })
}

function decodeStartTime(element: unknown, field: string): ProgramStartTime {
  const source = asRecord(element, field)
  return {
    id: num(source, 'id', field),
    minutesAfterMidnight: num(source, 'minutesAfterMidnight', field),
    timezone: str(source, 'timezone', field),
  }
}

function decodeProgramZone(element: unknown, field: string): ProgramZone {
  const source = asRecord(element, field)
  return {
    id: num(source, 'id', field),
    zoneId: num(source, 'zoneId', field),
    sequence: num(source, 'sequence', field),
    durationSeconds: num(source, 'durationSeconds', field),
  }
}

export function decodePrograms(payload: unknown): Program[] {
  return asArray(payload, 'programs').map((element, index) => {
    const field = `programs[${index}]`
    const source = asRecord(element, field)
    const anchor = source['anchorDate']
    if (anchor !== null && anchor !== undefined && typeof anchor !== 'string') {
      throw new DecodeError(`${field}.anchorDate`, 'expected a YYYY-MM-DD string or null')
    }
    return {
      id: num(source, 'id', field),
      name: str(source, 'name', field),
      enabled: bool(source, 'enabled', field),
      dayMode: dayMode(source, field),
      dowMask: num(source, 'dowMask', field),
      intervalDays: num(source, 'intervalDays', field),
      anchorDate: anchor === undefined || anchor === '' ? null : (anchor as string | null),
      startTimes: asArray(source['startTimes'], `${field}.startTimes`).map((start, i) =>
        decodeStartTime(start, `${field}.startTimes[${i}]`),
      ),
      zones: asArray(source['zones'], `${field}.zones`)
        .map((zone, i) => decodeProgramZone(zone, `${field}.zones[${i}]`))
        .sort((left, right) => left.sequence - right.sequence),
      nextRunUtc: instant(source, 'nextRunUtc', field),
    }
  })
}

export function decodeSettings(payload: unknown): SettingsMap {
  const source = asRecord(payload, 'settings')
  const map: SettingsMap = {}
  for (const [key, value] of Object.entries(source)) {
    if (typeof value !== 'string') {
      throw new DecodeError(`settings.${key}`, `expected a string, received ${typeof value}`)
    }
    map[key] = value
  }
  return map
}
