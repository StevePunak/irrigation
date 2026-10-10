export type DayMode = 'DaysOfWeek' | 'Odd' | 'Even' | 'EveryNDays'

export const DAY_MODES: readonly DayMode[] = ['DaysOfWeek', 'Odd', 'Even', 'EveryNDays']

export type RunSource = 'program' | 'manual' | 'panel'

/** One open valve. `zone` is the zone number. */
export interface RunningZone {
  zone: number
  secondsRemaining: number
  source: RunSource
}

/** The program the controller is running. `waitingZones` are zone numbers. */
export interface RunningProgram {
  id: number
  name: string
  step: number
  stepCount: number
  waitingZones: number[]
}

export interface QueuedProgram {
  programId: number
  name: string
  queuedAtUtc: string | null
}

/** The outdoor sensor. Both values are null while the sensor has no fresh reading. */
export interface Climate {
  temperatureC: number | null
  humidityPercent: number | null
}

export interface Status {
  running: RunningZone[]
  program: RunningProgram | null
  queue: QueuedProgram[]
  maxConcurrentZones: number
  nextRunUtc: string | null
  timezone: string
  masterEnabled: boolean
  rainDelayUntilUtc: string | null
  /** Null when the controller has no sensor configured. */
  climate: Climate | null
}

export interface Zone {
  id: number
  number: number
  name: string
  enabled: boolean
}

export interface ProgramStartTime {
  id: number
  minutesAfterMidnight: number
  timezone: string
}

/** One step of a program. `zones` holds zone ids (`zone.id`). */
export interface ProgramStep {
  zones: number[]
  durationSeconds: number
}

export interface Program {
  id: number
  name: string
  enabled: boolean
  dayMode: DayMode
  dowMask: number
  intervalDays: number
  anchorDate: string | null
  startTimes: ProgramStartTime[]
  steps: ProgramStep[]
  nextRunUtc: string | null
}

export interface StartTimeDraft {
  minutesAfterMidnight: number
  timezone: string
}

export interface ProgramDraft {
  name: string
  enabled: boolean
  dayMode: DayMode
  dowMask: number
  intervalDays: number
  anchorDate: string | null
  startTimes: StartTimeDraft[]
  steps: ProgramStep[]
}

export type SettingsMap = Record<string, string>

/** Thrown for any non-2xx response. `status` is the HTTP code; `reason` is the daemon's refusal name, such as `cap_reached`, or null. */
export class ApiError extends Error {
  readonly status: number
  readonly reason: string | null

  constructor(status: number, message: string, reason: string | null = null) {
    super(message)
    this.name = 'ApiError'
    this.status = status
    this.reason = reason
  }
}

/** Thrown when a response body does not match the contract. `field` is the path that failed. */
export class DecodeError extends Error {
  readonly field: string

  constructor(field: string, detail: string) {
    super(`${field}: ${detail}`)
    this.name = 'DecodeError'
    this.field = field
  }
}
