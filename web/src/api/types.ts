export type DayMode = 'DaysOfWeek' | 'Odd' | 'Even' | 'EveryNDays'

export const DAY_MODES: readonly DayMode[] = ['DaysOfWeek', 'Odd', 'Even', 'EveryNDays']

export interface Status {
  runningZone: number
  secondsRemaining: number
  nextRunUtc: string | null
  timezone: string
  masterEnabled: boolean
  rainDelayUntilUtc: string | null
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

export interface ProgramZone {
  id: number
  zoneId: number
  sequence: number
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
  zones: ProgramZone[]
  nextRunUtc: string | null
}

export interface StartTimeDraft {
  minutesAfterMidnight: number
  timezone: string
}

export interface ProgramZoneDraft {
  zoneId: number
  sequence: number
  durationSeconds: number
}

export interface ProgramDraft {
  name: string
  enabled: boolean
  dayMode: DayMode
  dowMask: number
  intervalDays: number
  anchorDate: string | null
  startTimes: StartTimeDraft[]
  zones: ProgramZoneDraft[]
}

export type SettingsMap = Record<string, string>

/** Thrown for any non-2xx response. `status` is the HTTP code. */
export class ApiError extends Error {
  readonly status: number

  constructor(status: number, message: string) {
    super(message)
    this.name = 'ApiError'
    this.status = status
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
