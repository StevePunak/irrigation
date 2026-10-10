import { describe, expect, it } from 'vitest'
import { formatInches, nearestWaterIndex, waterStep } from './WeatherChart'

describe('waterStep', () => {
  it.each([
    [0, 0.01],
    [0.03, 0.01],
    [0.05, 0.02],
    [0.56, 0.2],
    [1.2, 0.5],
    [30, 5],
  ])('steps %s in at %s', (highest, step) => {
    expect(waterStep(highest)).toBe(step)
  })
})

describe('nearestWaterIndex', () => {
  const points = [
    { ms: 0, rain: 0, et0: 0 },
    { ms: 3600000, rain: 1, et0: null },
    { ms: 7200000, rain: null, et0: 0.1 },
  ]

  it('picks the bucket whose centre is nearest', () => {
    expect(nearestWaterIndex(points, 3600000, 0)).toBe(0)
    expect(nearestWaterIndex(points, 3600000, 5000000)).toBe(1)
    expect(nearestWaterIndex(points, 3600000, 99999999)).toBe(2)
  })

  it('answers null with no points', () => {
    expect(nearestWaterIndex([], 3600000, 0)).toBeNull()
  })
})

describe('formatInches', () => {
  it('shows hundredths', () => {
    expect(formatInches(0.256)).toBe('0.26')
  })
})
