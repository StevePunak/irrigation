import type { Program, ProgramDraft } from '../api/types'

/**
 * Index 0 is bit 0 of `dowMask`, which the daemon defines as Monday
 * (`QDate::dayOfWeek()` minus one). Re-indexing this array to Sunday-first waters
 * on the wrong days and breaks nothing that would fail to compile.
 */
export const WEEKDAY_LABELS: readonly string[] = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun']

const MONTH_LABELS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec']
const FULL_WEEK = 0b1111111

export function weekdaysFromMask(mask: number): number[] {
  const days: number[] = []
  for (let index = 0; index < WEEKDAY_LABELS.length; index += 1) {
    if ((mask & (1 << index)) !== 0) {
      days.push(index)
    }
  }
  return days
}

export function maskFromWeekdays(indices: number[]): number {
  return indices.reduce((mask, index) => {
    if (index < 0 || index >= WEEKDAY_LABELS.length) {
      return mask
    }
    return mask | (1 << index)
  }, 0)
}

export function toggleWeekday(mask: number, index: number): number {
  if (index < 0 || index >= WEEKDAY_LABELS.length) {
    return mask
  }
  return mask ^ (1 << index)
}

/** Formats a `YYYY-MM-DD` calendar date. No instant and no timezone are involved. */
function formatAnchor(anchorDate: string | null): string {
  const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(anchorDate ?? '')
  if (match === null) {
    return 'an unset date'
  }
  return `${Number(match[3])} ${MONTH_LABELS[Number(match[2]) - 1]} ${match[1]}`
}

export function dayRuleSummary(
  program: Pick<Program, 'dayMode' | 'dowMask' | 'intervalDays' | 'anchorDate'>,
): string {
  switch (program.dayMode) {
    case 'Odd':
      return 'Odd days'
    case 'Even':
      return 'Even days'
    case 'EveryNDays': {
      const every = program.intervalDays === 1 ? 'Every day' : `Every ${program.intervalDays} days`
      return `${every} from ${formatAnchor(program.anchorDate)}`
    }
    case 'DaysOfWeek': {
      const masked = program.dowMask & FULL_WEEK
      if (masked === FULL_WEEK) {
        return 'Every day'
      }
      if (masked === 0) {
        return 'No days selected'
      }
      return weekdaysFromMask(masked)
        .map((index) => WEEKDAY_LABELS[index])
        .join(', ')
    }
  }
}

export function totalRuntimeSeconds(zones: { durationSeconds: number }[]): number {
  return zones.reduce((total, zone) => total + zone.durationSeconds, 0)
}

/** Converts a loaded program into the body shape POST and PUT accept. */
export function toDraft(program: Program): ProgramDraft {
  return {
    name: program.name,
    enabled: program.enabled,
    dayMode: program.dayMode,
    dowMask: program.dowMask,
    intervalDays: program.intervalDays,
    anchorDate: program.anchorDate,
    startTimes: program.startTimes.map((start) => ({
      minutesAfterMidnight: start.minutesAfterMidnight,
      timezone: start.timezone,
    })),
    zones: program.zones.map((zone, index) => ({
      zoneId: zone.zoneId,
      sequence: index + 1,
      durationSeconds: zone.durationSeconds,
    })),
  }
}
