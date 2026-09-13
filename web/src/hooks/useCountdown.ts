import { useEffect, useRef, useState } from 'react'

/**
 * Ticks `seconds` down locally between polls. `epoch` identifies the poll the
 * value came from. A change to `epoch` resets the count. A repeated `seconds`
 * value alone does not.
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
