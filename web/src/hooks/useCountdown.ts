import { useEffect, useRef, useState } from 'react'

/**
 * Ticks `seconds` down locally between polls. `epoch` identifies the poll the
 * value came from; a change resets the count. Keying the reset on `seconds`
 * instead stalls the display whenever two polls report the same value.
 */
export function useCountdown(seconds: number, epoch: number): number {
  const [remaining, setRemaining] = useState(seconds)
  const lastEpoch = useRef(epoch)

  if (lastEpoch.current !== epoch) {
    lastEpoch.current = epoch
    setRemaining(seconds)
  }

  useEffect(() => {
    const timer = setInterval(() => {
      setRemaining((value) => Math.max(0, value - 1))
    }, 1000)

    return () => {
      clearInterval(timer)
    }
  }, [])

  return remaining
}
