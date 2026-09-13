import { describe, expect, it } from 'vitest'
import { minutesToClock, minutesToInputValue } from './zonedformat'

const hostZone = Intl.DateTimeFormat().resolvedOptions().timeZone

describe('wall-clock minutes under a non-UTC host zone', () => {
  it('runs under a non-UTC zone', () => {
    expect(hostZone, 'this file proves nothing under UTC — run it via npm run test:wallclock').not.toBe('UTC')
  })

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
  })
})
