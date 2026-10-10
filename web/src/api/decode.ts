import {
  DAY_MODES,
  DecodeError,
  type DayMode,
  type Climate,
  type ClimateBucket,
  type ClimateHistory,
  type ClimateRange,
  type Program,
  type ProgramStartTime,
  type ProgramStep,
  type QueuedProgram,
  type RunningProgram,
  type RunningZone,
  type SettingsMap,
  type Status,
  type WeatherBucket,
  type WeatherHistory,
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

function numbers(value: unknown, field: string): number[] {
  return asArray(value, field).map((element, index) => {
    if (typeof element !== 'number' || Number.isFinite(element) === false) {
      throw new DecodeError(`${field}[${index}]`, `expected a number, received ${typeof element}`)
    }
    return element
  })
}

function decodeRunningZone(element: unknown, field: string): RunningZone {
  const source = asRecord(element, field)
  const runSource = str(source, 'source', field)
  if (runSource !== 'program' && runSource !== 'manual' && runSource !== 'panel') {
    throw new DecodeError(`${field}.source`, `unknown source "${runSource}"`)
  }
  return {
    zone: num(source, 'zone', field),
    secondsRemaining: num(source, 'secondsRemaining', field),
    source: runSource,
  }
}

/** The daemon sends null when no program runs; an absent key is a contract break. */
function decodeRunningProgram(value: unknown, field: string): RunningProgram | null {
  if (value === null) {
    return null
  }
  const source = asRecord(value, field)
  return {
    id: num(source, 'id', field),
    name: str(source, 'name', field),
    step: num(source, 'step', field),
    stepCount: num(source, 'stepCount', field),
    waitingZones: numbers(source['waitingZones'], `${field}.waitingZones`),
  }
}

function decodeQueuedProgram(element: unknown, field: string): QueuedProgram {
  const source = asRecord(element, field)
  return {
    programId: num(source, 'programId', field),
    name: str(source, 'name', field),
    queuedAtUtc: instant(source, 'queuedAtUtc', field),
  }
}

function nullableNum(source: Record<string, unknown>, key: string, field: string): number | null {
  return source[key] === null ? null : num(source, key, field)
}

/** A daemon older than the climate logger omits the key, which reads as no sensor. */
function decodeClimate(value: unknown, field: string): Climate | null {
  if (value === undefined || value === null) {
    return null
  }
  const source = asRecord(value, field)
  return {
    temperatureC: nullableNum(source, 'temperatureC', field),
    humidityPercent: nullableNum(source, 'humidityPercent', field),
  }
}

export function decodeStatus(payload: unknown): Status {
  const source = asRecord(payload, 'status')
  return {
    running: asArray(source['running'], 'status.running').map((element, index) =>
      decodeRunningZone(element, `status.running[${index}]`),
    ),
    program: decodeRunningProgram(source['program'], 'status.program'),
    queue: asArray(source['queue'], 'status.queue').map((element, index) =>
      decodeQueuedProgram(element, `status.queue[${index}]`),
    ),
    maxConcurrentZones: num(source, 'maxConcurrentZones', 'status'),
    nextRunUtc: instant(source, 'nextRunUtc', 'status'),
    timezone: str(source, 'timezone', 'status'),
    masterEnabled: bool(source, 'masterEnabled', 'status'),
    rainDelayUntilUtc: instant(source, 'rainDelayUntilUtc', 'status'),
    climate: decodeClimate(source['climate'], 'status.climate'),
  }
}

function decodeClimateRange(value: unknown, field: string): ClimateRange {
  const source = asRecord(value, field)
  return { min: num(source, 'min', field), mean: num(source, 'mean', field), max: num(source, 'max', field) }
}

function decodeClimateBucket(element: unknown, field: string): ClimateBucket {
  const source = asRecord(element, field)
  return {
    startUtc: str(source, 'startUtc', field),
    count: num(source, 'count', field),
    temperatureC: decodeClimateRange(source['temperatureC'], `${field}.temperatureC`),
    humidityPercent: decodeClimateRange(source['humidityPercent'], `${field}.humidityPercent`),
  }
}

function decodeWeatherBucket(element: unknown, field: string): WeatherBucket {
  const source = asRecord(element, field)
  return {
    startUtc: str(source, 'startUtc', field),
    precipitationMm: nullableNum(source, 'precipitationMm', field),
    et0Mm: nullableNum(source, 'et0Mm', field),
    temperatureC: nullableNum(source, 'temperatureC', field),
    humidityPercent: nullableNum(source, 'humidityPercent', field),
  }
}

/** A daemon older than the weather poller omits the key, which reads as no weather. */
function decodeWeatherHistory(value: unknown, field: string): WeatherHistory {
  if (value === undefined) {
    return { bucketSeconds: 0, buckets: [] }
  }
  const source = asRecord(value, field)
  return {
    bucketSeconds: num(source, 'bucketSeconds', field),
    buckets: asArray(source['buckets'], `${field}.buckets`).map((element, index) =>
      decodeWeatherBucket(element, `${field}.buckets[${index}]`),
    ),
  }
}

export function decodeClimateHistory(payload: unknown): ClimateHistory {
  const source = asRecord(payload, 'climate')
  return {
    fromUtc: str(source, 'fromUtc', 'climate'),
    toUtc: str(source, 'toUtc', 'climate'),
    bucketSeconds: num(source, 'bucketSeconds', 'climate'),
    buckets: asArray(source['buckets'], 'climate.buckets').map((element, index) =>
      decodeClimateBucket(element, `climate.buckets[${index}]`),
    ),
    weather: decodeWeatherHistory(source['weather'], 'climate.weather'),
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

function decodeProgramStep(element: unknown, field: string): ProgramStep {
  const source = asRecord(element, field)
  return {
    zones: numbers(source['zones'], `${field}.zones`),
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
      steps: asArray(source['steps'], `${field}.steps`).map((step, i) =>
        decodeProgramStep(step, `${field}.steps[${i}]`),
      ),
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
