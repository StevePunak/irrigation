/** Rendered in place of a time that cannot be formatted. */
export const INVALID_ZONE_MARKER = '--'

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec']
const WEEKDAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat']

interface ZonedParts {
  year: number
  month: number
  day: number
  hour: number
  minute: number
  weekday: number
}

const partsCache = new Map<string, Intl.DateTimeFormat>()

function partsFormatter(zone: string): Intl.DateTimeFormat | null {
  const cached = partsCache.get(zone)
  if (cached !== undefined) {
    return cached
  }
  try {
    const formatter = new Intl.DateTimeFormat('en-GB', {
      timeZone: zone,
      hourCycle: 'h23',
      year: 'numeric',
      month: '2-digit',
      day: '2-digit',
      hour: '2-digit',
      minute: '2-digit',
      weekday: 'short',
    })
    partsCache.set(zone, formatter)
    return formatter
  } catch {
    return null
  }
}

/**
 * Breaks a UTC instant into calendar fields as they read in `zone`. Returns null
 * when the zone is unknown to the runtime or the instant will not parse.
 */
function zonedParts(utcIso: string, zone: string): ZonedParts | null {
  const ms = Date.parse(utcIso)
  if (Number.isFinite(ms) === false) {
    return null
  }

  const formatter = partsFormatter(zone)
  if (formatter === null) {
    return null
  }

  const found = new Map<string, string>()
  for (const part of formatter.formatToParts(new Date(ms))) {
    found.set(part.type, part.value)
  }

  const weekdayIndex = WEEKDAYS.indexOf(found.get('weekday') ?? '')
  if (weekdayIndex < 0) {
    return null
  }

  return {
    year: Number(found.get('year')),
    month: Number(found.get('month')),
    day: Number(found.get('day')),
    hour: Number(found.get('hour')),
    minute: Number(found.get('minute')),
    weekday: weekdayIndex,
  }
}

function clockFromHourMinute(hour: number, minute: number): string {
  const period = hour < 12 ? 'AM' : 'PM'
  const hour12 = hour % 12 === 0 ? 12 : hour % 12
  return `${hour12}:${String(minute).padStart(2, '0')} ${period}`
}

/** Renders the time of day of a UTC instant as it reads in `zone`. */
export function formatClock(utcIso: string, zone: string): string {
  const parts = zonedParts(utcIso, zone)
  if (parts === null) {
    return INVALID_ZONE_MARKER
  }
  return clockFromHourMinute(parts.hour, parts.minute)
}

function dayNumber(parts: ZonedParts): number {
  return parts.year * 10000 + parts.month * 100 + parts.day
}

/** Renders a UTC instant as a day label plus a time, both read in `zone`. */
export function formatDayAndClock(utcIso: string, zone: string, nowMs: number = Date.now()): string {
  const parts = zonedParts(utcIso, zone)
  const today = zonedParts(new Date(nowMs).toISOString(), zone)
  if (parts === null || today === null) {
    return INVALID_ZONE_MARKER
  }

  const clock = clockFromHourMinute(parts.hour, parts.minute)
  const dayGap = Math.round(
    (Date.UTC(parts.year, parts.month - 1, parts.day) - Date.UTC(today.year, today.month - 1, today.day)) / 86400000,
  )

  if (dayNumber(parts) === dayNumber(today)) {
    return `Today ${clock}`
  }
  if (dayGap === 1) {
    return `Tomorrow ${clock}`
  }
  if (dayGap > 1 && dayGap < 7) {
    return `${WEEKDAYS[parts.weekday]} ${clock}`
  }
  return `${WEEKDAYS[parts.weekday]} ${parts.day} ${MONTHS[parts.month - 1]} ${clock}`
}

/** Renders a run length for display, e.g. 5400 -> "1 h 30 min". */
export function formatDuration(seconds: number): string {
  if (seconds < 60) {
    return `${Math.max(0, Math.round(seconds))} s`
  }

  const totalMinutes = Math.round(seconds / 60)
  const hours = Math.floor(totalMinutes / 60)
  const minutes = totalMinutes % 60

  if (hours === 0) {
    return `${minutes} min`
  }
  if (minutes === 0) {
    return `${hours} h`
  }
  return `${hours} h ${minutes} min`
}

/** Renders remaining seconds as a ticking clock. */
export function formatCountdown(seconds: number): string {
  const total = Math.max(0, Math.floor(seconds))
  const hours = Math.floor(total / 3600)
  const minutes = Math.floor((total % 3600) / 60)
  const secs = total % 60

  if (hours > 0) {
    return `${hours}:${String(minutes).padStart(2, '0')}:${String(secs).padStart(2, '0')}`
  }
  return `${minutes}:${String(secs).padStart(2, '0')}`
}

function isValidMinutes(value: number): boolean {
  return Number.isInteger(value) && value >= 0 && value <= 1439
}

/**
 * A start time is a wall-clock rule with no instant behind it, so this takes no
 * zone. Converting it through a Date shifts every start time by the host offset.
 */
export function minutesToClock(minutesAfterMidnight: number): string {
  if (isValidMinutes(minutesAfterMidnight) === false) {
    return INVALID_ZONE_MARKER
  }
  return clockFromHourMinute(Math.floor(minutesAfterMidnight / 60), minutesAfterMidnight % 60)
}

/** Formats wall-clock minutes for an `<input type="time">` value. Invalid input yields `'00:00'`, indistinguishable from midnight. */
export function minutesToInputValue(minutesAfterMidnight: number): string {
  if (isValidMinutes(minutesAfterMidnight) === false) {
    return '00:00'
  }
  return `${String(Math.floor(minutesAfterMidnight / 60)).padStart(2, '0')}:${String(minutesAfterMidnight % 60).padStart(2, '0')}`
}

/** Parses an `<input type="time">` value. Returns -1 when it will not parse. */
export function inputValueToMinutes(value: string): number {
  const match = /^(\d{2}):(\d{2})$/.exec(value)
  if (match === null) {
    return -1
  }
  const hours = Number(match[1])
  const minutes = Number(match[2])
  if (hours > 23 || minutes > 59) {
    return -1
  }
  return hours * 60 + minutes
}

const HOUR_MS = 3600000

function tickMatches(parts: ZonedParts, hours: number): boolean {
  if (hours <= 24) {
    return parts.hour % 6 === 0
  }
  if (parts.hour !== 0) {
    return false
  }
  if (hours <= 168) {
    return true
  }
  if (hours <= 744) {
    return (parts.day - 1) % 7 === 0
  }
  return parts.day === 1
}

/**
 * Returns the instants in [fromMs, toMs] where a chart axis spanning `hours` puts a tick,
 * read in `zone`: every six hours for a day, local midnights for a week, every seventh
 * day of the month for a month, and the first of each month beyond that.
 */
export function axisTicks(fromMs: number, toMs: number, zone: string, hours: number): number[] {
  const ticks: number[] = []
  for (let ms = Math.ceil(fromMs / HOUR_MS) * HOUR_MS; ms <= toMs; ms += HOUR_MS) {
    const parts = zonedParts(new Date(ms).toISOString(), zone)
    if (parts === null) {
      return []
    }
    if (tickMatches(parts, hours)) {
      ticks.push(ms)
    }
  }
  return ticks
}

/** Labels an axisTicks() instant for an axis spanning `hours`, e.g. "6 PM", "Sat 10", "8 Oct", "Oct". */
export function formatAxisTick(ms: number, zone: string, hours: number): string {
  const parts = zonedParts(new Date(ms).toISOString(), zone)
  if (parts === null) {
    return INVALID_ZONE_MARKER
  }
  if (hours <= 24) {
    const period = parts.hour < 12 ? 'AM' : 'PM'
    const hour12 = parts.hour % 12 === 0 ? 12 : parts.hour % 12
    return `${hour12} ${period}`
  }
  if (hours <= 168) {
    return `${WEEKDAYS[parts.weekday]} ${parts.day}`
  }
  if (hours <= 744) {
    return `${parts.day} ${MONTHS[parts.month - 1]}`
  }
  return parts.month === 1 ? `${MONTHS[0]} ${parts.year}` : MONTHS[parts.month - 1]!
}
