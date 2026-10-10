import { describe, expect, it } from 'vitest'
import { nearestIndex, runsOf, type ChartPoint } from './ClimateChart'

const BUCKET = 300000

function point(minute: number): ChartPoint {
  return { ms: minute * 60000, min: 1, mean: 2, max: 3 }
}

describe('runsOf', () => {
  it('keeps consecutive buckets in one run', () => {
    expect(runsOf([point(0), point(5), point(10)], BUCKET)).toHaveLength(1)
  })

  it('breaks the line where a bucket is missing', () => {
    const runs = runsOf([point(0), point(5), point(15), point(20)], BUCKET)
    expect(runs.map((run) => run.map((entry) => entry.ms / 60000))).toEqual([
      [0, 5],
      [15, 20],
    ])
  })

  it('returns nothing for no points', () => {
    expect(runsOf([], BUCKET)).toEqual([])
  })
})

describe('nearestIndex', () => {
  const points = [point(0), point(5), point(15)]

  it('snaps to the bucket whose centre is closest', () => {
    // Centres sit at 2.5, 7.5 and 17.5 minutes.
    expect(nearestIndex(points, BUCKET, 0)).toBe(0)
    expect(nearestIndex(points, BUCKET, 4.9 * 60000)).toBe(0)
    expect(nearestIndex(points, BUCKET, 5.1 * 60000)).toBe(1)
    expect(nearestIndex(points, BUCKET, 13 * 60000)).toBe(2)
    expect(nearestIndex(points, BUCKET, 99 * 60000)).toBe(2)
  })
})
