import { act, renderHook } from '@testing-library/react'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { useCountdown } from './useCountdown'

beforeEach(() => {
  vi.useFakeTimers()
})

afterEach(() => {
  vi.useRealTimers()
})

describe('useCountdown', () => {
  it('starts at the value it is given', () => {
    const { result } = renderHook(() => useCountdown(120, 1))
    expect(result.current).toBe(120)
  })

  it('decrements once a second between polls', () => {
    const { result } = renderHook(() => useCountdown(120, 1))

    act(() => {
      vi.advanceTimersByTime(3000)
    })

    expect(result.current).toBe(117)
  })

  it('resets when a poll delivers a new epoch', () => {
    const { result, rerender } = renderHook(({ seconds, epoch }) => useCountdown(seconds, epoch), {
      initialProps: { seconds: 120, epoch: 1 },
    })

    act(() => {
      vi.advanceTimersByTime(5000)
    })
    expect(result.current).toBe(115)

    rerender({ seconds: 118, epoch: 2 })
    expect(result.current).toBe(118)
  })

  it('holds at the same value when a poll repeats the epoch', () => {
    const { result, rerender } = renderHook(({ seconds, epoch }) => useCountdown(seconds, epoch), {
      initialProps: { seconds: 120, epoch: 1 },
    })

    act(() => {
      vi.advanceTimersByTime(4000)
    })
    expect(result.current).toBe(116)

    rerender({ seconds: 120, epoch: 1 })
    expect(result.current).toBe(116)
  })

  it('resets when a new epoch repeats the previous seconds value', () => {
    const { result, rerender } = renderHook(({ seconds, epoch }) => useCountdown(seconds, epoch), {
      initialProps: { seconds: 120, epoch: 1 },
    })

    act(() => {
      vi.advanceTimersByTime(4000)
    })
    expect(result.current).toBe(116)

    rerender({ seconds: 120, epoch: 2 })
    expect(result.current).toBe(120)
  })

  it('floors at zero', () => {
    const { result } = renderHook(() => useCountdown(2, 1))

    act(() => {
      vi.advanceTimersByTime(10000)
    })

    expect(result.current).toBe(0)
  })

  it('stops the timer once unmounted', () => {
    const { unmount } = renderHook(() => useCountdown(120, 1))
    unmount()
    expect(vi.getTimerCount()).toBe(0)
  })
})
